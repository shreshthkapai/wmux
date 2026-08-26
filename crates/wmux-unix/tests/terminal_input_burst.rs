#![cfg(unix)]

use nix::{
    fcntl::{fcntl, FcntlArg, OFlag},
    pty::openpty,
    unistd::dup,
};
use std::{
    fs::File,
    io::{self, Read, Write},
    os::unix::process::CommandExt,
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant},
};
use wmux_platform::{MouseEventKind, TerminalBackend, TerminalInput};
use wmux_unix::UnixTerminalBackend;

const CHILD_ENV: &str = "WMUX_TERMINAL_INPUT_BURST_CHILD";
const READY: &[u8] = b"WMUX_INPUT_READY";
const COMPLETE: &[u8] = b"WMUX_INPUT_COMPLETE_100";

#[test]
fn terminal_input_burst_child() {
    if std::env::var_os(CHILD_ENV).is_none() {
        return;
    }

    let terminal = UnixTerminalBackend;
    let _guard = terminal.enter().expect("child terminal enters raw mode");
    println!("{}", String::from_utf8_lossy(READY));
    io::stdout().flush().expect("ready marker flushes");

    for index in 0..100 {
        let input = terminal
            .read_input()
            .unwrap_or_else(|error| panic!("input event {index} is readable: {error}"));
        assert!(matches!(
            input,
            Some(TerminalInput::Mouse(event)) if event.kind == MouseEventKind::ScrollUp
        ));
    }

    println!("{}", String::from_utf8_lossy(COMPLETE));
    io::stdout().flush().expect("completion marker flushes");
}

#[test]
fn one_large_terminal_write_delivers_every_complete_event() {
    let pty = openpty(None, None).expect("test PTY opens");
    let stdin = File::from(dup(&pty.slave).expect("stdin descriptor duplicates"));
    let stdout = File::from(dup(&pty.slave).expect("stdout descriptor duplicates"));
    let stderr = File::from(dup(&pty.slave).expect("stderr descriptor duplicates"));

    let mut command = Command::new(std::env::current_exe().expect("test executable is known"));
    command
        .arg("--exact")
        .arg("terminal_input_burst_child")
        .arg("--nocapture")
        .env(CHILD_ENV, "1")
        .stdin(Stdio::from(stdin))
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr));
    unsafe {
        command.pre_exec(|| {
            if libc::setsid() == -1 {
                return Err(io::Error::last_os_error());
            }
            if libc::ioctl(0, libc::TIOCSCTTY, 0) == -1 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let mut child = command.spawn().expect("PTY child starts");
    drop(pty.slave);

    let mut master = File::from(pty.master);
    let flags = OFlag::from_bits_truncate(
        fcntl(&master, FcntlArg::F_GETFL).expect("master flags are readable"),
    );
    fcntl(&master, FcntlArg::F_SETFL(flags | OFlag::O_NONBLOCK))
        .expect("master becomes nonblocking");

    let mut output = Vec::new();
    wait_for_marker(&mut master, &mut output, READY, &mut child);
    let report = b"\x1b[<64;10;10M";
    assert_eq!(report.len() * 100, 1_200);
    master
        .write_all(&report.repeat(100))
        .expect("one large terminal write succeeds");
    wait_for_marker(&mut master, &mut output, COMPLETE, &mut child);

    let status = child.wait().expect("PTY child exits");
    assert!(
        status.success(),
        "child output: {}",
        String::from_utf8_lossy(&output)
    );
}

fn wait_for_marker(
    master: &mut File,
    output: &mut Vec<u8>,
    marker: &[u8],
    child: &mut std::process::Child,
) {
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut chunk = [0_u8; 4 * 1024];
    loop {
        match master.read(&mut chunk) {
            Ok(0) => {}
            Ok(read) => output.extend_from_slice(&chunk[..read]),
            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
            Err(error) if error.raw_os_error() == Some(libc::EIO) => {}
            Err(error) => panic!("read PTY child output: {error}"),
        }
        if output.windows(marker.len()).any(|window| window == marker) {
            return;
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            let _ = child.wait();
            panic!(
                "timed out waiting for {:?}; child output: {}",
                String::from_utf8_lossy(marker),
                String::from_utf8_lossy(output)
            );
        }
        if let Some(status) = child.try_wait().expect("child status is readable") {
            panic!(
                "child exited with {status} before {:?}; output: {}",
                String::from_utf8_lossy(marker),
                String::from_utf8_lossy(output)
            );
        }
        thread::sleep(Duration::from_millis(2));
    }
}
