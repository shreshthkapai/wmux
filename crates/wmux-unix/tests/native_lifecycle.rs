#![cfg(unix)]

use std::{
    fs, io,
    path::{Path, PathBuf},
    process,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use wmux_platform::{BoxedIpcStream, ClientTransport};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    FRAME_HEADER_LEN, VERSION,
};
use wmux_unix::{UnixClientTransport, UnixServerPlatform};

struct TestDirectory(PathBuf);

impl TestDirectory {
    fn new() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let nonce = NEXT.fetch_add(1, Ordering::Relaxed);
        Self(test_temporary_root().join(format!("wn-{:x}-{nonce:x}", process::id())))
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TestDirectory {
    fn drop(&mut self) {
        if self
            .0
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with("wn-"))
        {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
}

#[cfg(target_os = "macos")]
fn test_temporary_root() -> PathBuf {
    PathBuf::from("/tmp")
}

#[cfg(not(target_os = "macos"))]
fn test_temporary_root() -> PathBuf {
    std::env::temp_dir()
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn real_unix_lifecycle_preserves_detached_output_and_cleans_up() {
    let runtime = TestDirectory::new();
    let platform = UnixServerPlatform::from_runtime_directory(runtime.path().to_path_buf())
        .expect("server platform is constructed");
    let transport = UnixClientTransport::from_runtime_directory(runtime.path().to_path_buf())
        .expect("client transport is constructed");
    let project = runtime.path().join("project directory");
    fs::create_dir_all(&project).expect("client project directory is created");
    let project = fs::canonicalize(project).expect("client project directory is canonicalized");
    let (server_result_tx, server_result_rx) = mpsc::sync_channel(1);
    let server = thread::spawn(move || {
        let result = wmux_server::run_with_platform_and_config(
            Box::new(platform),
            wmux_config::WmuxConfig::default(),
        );
        let _ = server_result_tx.send(result);
    });

    let mut attached = connect_and_handshake_at(&transport, &project).await;
    command(&mut attached, "new-session -s native").await;
    let cwd_marker = project.join(".wmux-cwd-observed");
    write_message(
        &mut attached,
        Message::Input(b"printf '%s\\n' \"$PWD\" > .wmux-cwd-observed\n".to_vec()),
    )
    .await;
    let observed_cwd = tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match fs::read_to_string(&cwd_marker) {
                Ok(contents) if !contents.is_empty() => return contents,
                Ok(_) => {}
                Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
                Err(error) => panic!("pane cwd marker could not be read: {error}"),
            }
            tokio::time::sleep(Duration::from_millis(10)).await;
        }
    })
    .await
    .expect("pane writes its cwd marker");
    assert_eq!(Path::new(observed_cwd.trim_end()), project);
    write_message(
        &mut attached,
        Message::Input(b"printf 'WMUX_%s' TYPED\n".to_vec()),
    )
    .await;
    wait_for_output(&mut attached, b"WMUX_TYPED").await;

    drop(attached);
    let mut attached = connect_and_handshake(&transport).await;
    command(&mut attached, "attach-session -t native").await;

    command(&mut attached, "split-window").await;
    write_message(
        &mut attached,
        Message::Resize {
            cols: 100,
            rows: 30,
        },
    )
    .await;
    write_message(
        &mut attached,
        Message::Input(b"set -- $(stty size); printf 'WMUX_SIZE_%sx%s' \"$1\" \"$2\"\n".to_vec()),
    )
    .await;
    // Reserve one client row for status, then divide the remaining pane area
    // around the one-row separator: (30 - 1 - 1) / 2.
    wait_for_output(&mut attached, b"WMUX_SIZE_14x100").await;

    // Assemble PID markers in the shell so they exist only in command output.
    // A literal marker in echoed input can share cells with the rendered result,
    // allowing a correct client diff to omit an unchanged marker prefix.
    write_message(
        &mut attached,
        Message::Input(
            b"sh -c 'trap \"printf \\\"WMUX_BACKGROUND\\\\n\\\"; exit 0\" USR1; printf \"WMUX_%s_PID_%s\\n\" BACKGROUND \"$$\"; while :; do sleep 60 & wait; done' &\n"
                .to_vec(),
        ),
    )
    .await;
    let background_pid = wait_for_pid_marker(&mut attached, b"WMUX_BACKGROUND_PID_").await;
    assert!(process_is_running(background_pid));

    write_message(&mut attached, Message::Detach).await;
    wait_for_command_ok(&mut attached).await;
    drop(attached);

    assert_eq!(unsafe { libc::kill(background_pid, libc::SIGUSR1) }, 0);
    wait_for_process_exit(background_pid).await;

    let mut reattached = connect_and_handshake(&transport).await;
    command_and_wait_for_output(
        &mut reattached,
        "attach-session -t native",
        b"WMUX_BACKGROUND",
    )
    .await;

    write_message(
        &mut reattached,
        Message::Input(
            b"sh -c '(sleep 60) & child=$!; printf \"WMUX_%s_PID_%s\\n\" CHILD \"$child\"; wait \"$child\"'\n"
                .to_vec(),
        ),
    )
    .await;
    // Client output is a screen diff, so an unchanged `WMUX_` prefix need not
    // be resent. The suffix is assembled only by command output and remains a
    // unique, contiguous changed run.
    let child_pid = wait_for_pid_marker(&mut reattached, b"CHILD_PID_").await;
    assert!(process_is_running(child_pid));

    let mut controller = connect_and_handshake(&transport).await;
    command(&mut controller, "kill-session -t native").await;
    wait_for_shutdown(&mut reattached).await;
    wait_for_process_exit(child_pid).await;
    command(&mut controller, "kill-server").await;
    drop(controller);

    let server_result = server_result_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("server exits after kill-server");
    server_result.expect("server exits cleanly");
    server.join().expect("server thread joins");
    assert!(!runtime.path().join("wmux.sock").exists());
    assert!(!runtime.path().join("wmux.lock").exists());

    run_restart(runtime.path()).await;
}

async fn run_restart(runtime: &Path) {
    let platform = UnixServerPlatform::from_runtime_directory(runtime.to_path_buf())
        .expect("Unix endpoint is reusable after shutdown");
    let transport = UnixClientTransport::from_runtime_directory(runtime.to_path_buf())
        .expect("matching restart transport is constructed");
    let (server_result_tx, server_result_rx) = mpsc::sync_channel(1);
    let server = thread::spawn(move || {
        let result = wmux_server::run_with_platform_and_config(
            Box::new(platform),
            wmux_config::WmuxConfig::default(),
        );
        let _ = server_result_tx.send(result);
    });

    let mut client = connect_and_handshake(&transport).await;
    command(&mut client, "new-session -d -s restarted").await;
    command(&mut client, "kill-server").await;
    drop(client);

    server_result_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("restarted server exits")
        .expect("restarted server exits cleanly");
    server.join().expect("restarted server thread joins");
    assert!(!runtime.join("wmux.sock").exists());
    assert!(!runtime.join("wmux.lock").exists());
}

async fn connect_and_handshake(transport: &UnixClientTransport) -> BoxedIpcStream {
    let current_dir = std::env::current_dir().unwrap();
    connect_and_handshake_at(transport, &current_dir).await
}

async fn connect_and_handshake_at(
    transport: &UnixClientTransport,
    current_dir: &Path,
) -> BoxedIpcStream {
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut stream = loop {
        match transport.connect().await {
            Ok(stream) => break stream,
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
            Err(error) => panic!("client could not connect: {error}"),
        }
    };
    write_message(
        &mut stream,
        Message::Hello {
            version: VERSION,
            pid: process::id(),
            capabilities: TerminalCapabilities::default(),
            current_dir: current_dir.to_string_lossy().into_owned(),
        },
    )
    .await;
    assert!(matches!(
        read_message(&mut stream).await,
        Some(Message::HelloOk {
            version: VERSION,
            ..
        })
    ));
    stream
}

async fn command(stream: &mut BoxedIpcStream, command: &str) {
    write_message(stream, Message::Command(command.to_string())).await;
    wait_for_command_ok(stream).await;
}

async fn wait_for_command_ok(stream: &mut BoxedIpcStream) {
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match read_message(stream).await {
                Some(Message::CommandOk(_)) => return,
                Some(Message::CommandErr(error)) => panic!("command failed: {error}"),
                Some(Message::Output { .. } | Message::Clipboard(_)) => {}
                Some(message) => panic!("unexpected command response: {message:?}"),
                None => panic!("server closed before command completion"),
            }
        }
    })
    .await
    .expect("command completes");
}

async fn command_and_wait_for_output(stream: &mut BoxedIpcStream, command: &str, marker: &[u8]) {
    write_message(stream, Message::Command(command.to_string())).await;
    let mut command_complete = false;
    let mut marker_received = false;
    let mut output = Vec::new();
    let outcome = tokio::time::timeout(Duration::from_secs(5), async {
        while !command_complete || !marker_received {
            match read_message(stream).await {
                Some(Message::CommandOk(_)) => command_complete = true,
                Some(Message::Output { bytes, .. }) => {
                    output.extend_from_slice(&bytes);
                    marker_received = output.windows(marker.len()).any(|window| window == marker);
                }
                Some(Message::CommandErr(error)) => panic!("command failed: {error}"),
                Some(Message::Clipboard(_)) => {}
                Some(message) => panic!("unexpected command response: {message:?}"),
                None => panic!("server closed before command output arrived"),
            }
        }
    })
    .await;
    if outcome.is_err() {
        panic!(
            "command completed={command_complete}, expected pane output {:?}; received {:?}",
            String::from_utf8_lossy(marker),
            String::from_utf8_lossy(&output),
        );
    }
}

async fn wait_for_output(stream: &mut BoxedIpcStream, marker: &[u8]) {
    let mut output = Vec::new();
    let outcome = tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match read_message(stream).await {
                Some(Message::Output { bytes, .. }) => {
                    output.extend_from_slice(&bytes);
                    if output.windows(marker.len()).any(|window| window == marker) {
                        return;
                    }
                }
                Some(Message::CommandErr(error)) => panic!("server command failed: {error}"),
                Some(Message::CommandOk(_) | Message::Clipboard(_)) => {}
                Some(message) => panic!("unexpected output message: {message:?}"),
                None => panic!("server closed before expected pane output"),
            }
        }
    })
    .await;
    if outcome.is_err() {
        panic!(
            "expected pane output {:?} arrives; received {:?}",
            String::from_utf8_lossy(marker),
            String::from_utf8_lossy(&output),
        )
    }
}

async fn wait_for_pid_marker(stream: &mut BoxedIpcStream, marker: &[u8]) -> libc::pid_t {
    let mut output = Vec::new();
    tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match read_message(stream).await {
                Some(Message::Output { bytes, .. }) => {
                    output.extend_from_slice(&bytes);
                    if let Some(pid) = parse_pid_marker(&output, marker) {
                        return pid;
                    }
                }
                Some(Message::CommandOk(_) | Message::Clipboard(_)) => {}
                Some(Message::CommandErr(error)) => panic!("server command failed: {error}"),
                Some(message) => panic!("unexpected PID response: {message:?}"),
                None => panic!("server closed before child PID was reported"),
            }
        }
    })
    .await
    .unwrap_or_else(|_| {
        panic!(
            "child PID marker timed out; output was {:?}",
            String::from_utf8_lossy(&output)
        )
    })
}

fn parse_pid_marker(output: &[u8], marker: &[u8]) -> Option<libc::pid_t> {
    output
        .windows(marker.len())
        .enumerate()
        .filter(|(_, window)| *window == marker)
        .find_map(|(offset, _)| {
            let start = offset + marker.len();
            let digits = output[start..]
                .iter()
                .copied()
                .take_while(u8::is_ascii_digit)
                .collect::<Vec<_>>();
            (!digits.is_empty())
                .then(|| std::str::from_utf8(&digits).ok()?.parse().ok())
                .flatten()
        })
}

#[test]
fn pid_marker_is_resolved_from_a_partial_render_suffix() {
    let echoed_command = br#"printf "WMUX_%s_PID_%s\n" CHILD "$child""#;
    assert_eq!(parse_pid_marker(echoed_command, b"CHILD_PID_"), None);
    assert_eq!(
        parse_pid_marker(b"\x1b[22;6HCHILD_PID_3580\x1b[0m", b"CHILD_PID_"),
        Some(3580)
    );
}

fn process_is_running(pid: libc::pid_t) -> bool {
    if unsafe { libc::kill(pid, 0) } != 0 {
        return false;
    }
    process_has_live_kernel_state(pid)
}

#[cfg(target_os = "linux")]
fn process_has_live_kernel_state(pid: libc::pid_t) -> bool {
    // A minimal Docker container may not reap orphaned children from PID 1.
    // `kill(pid, 0)` still succeeds for those dead zombies, so consult the
    // kernel state before calling one a live process leak.
    match fs::read_to_string(format!("/proc/{pid}/stat")) {
        Ok(state) => {
            state
                .rsplit_once(") ")
                .and_then(|(_, fields)| fields.as_bytes().first().copied())
                != Some(b'Z')
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => false,
        Err(_) => true,
    }
}

#[cfg(target_os = "linux")]
#[test]
fn a_missing_proc_entry_is_not_a_live_process() {
    assert!(!process_has_live_kernel_state(libc::pid_t::MAX));
}

#[cfg(not(target_os = "linux"))]
fn process_has_live_kernel_state(_pid: libc::pid_t) -> bool {
    true
}

async fn wait_for_process_exit(pid: libc::pid_t) {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if !process_is_running(pid) {
            return;
        }
        if Instant::now() >= deadline {
            panic!(
                "descendant process {pid} leaked; kernel state: {}",
                process_debug_state(pid)
            );
        }
        tokio::time::sleep(Duration::from_millis(10)).await;
    }
}

#[cfg(target_os = "linux")]
fn process_debug_state(pid: libc::pid_t) -> String {
    fs::read_to_string(format!("/proc/{pid}/stat"))
        .unwrap_or_else(|error| format!("unavailable: {error}"))
}

#[cfg(not(target_os = "linux"))]
fn process_debug_state(_pid: libc::pid_t) -> String {
    "unavailable on this operating system".to_string()
}

async fn wait_for_shutdown(stream: &mut BoxedIpcStream) {
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match read_message(stream).await {
                Some(Message::Shutdown) | None => return,
                Some(Message::Output { .. } | Message::Clipboard(_) | Message::CommandOk(_)) => {}
                Some(Message::CommandErr(error)) => panic!("shutdown command failed: {error}"),
                Some(message) => panic!("unexpected shutdown message: {message:?}"),
            }
        }
    })
    .await
    .expect("attached client observes session destruction");
}

async fn read_message(stream: &mut BoxedIpcStream) -> Option<Message> {
    let mut header = [0_u8; FRAME_HEADER_LEN];
    match stream.read_exact(&mut header).await {
        Ok(_) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return None,
        Err(error) => panic!("read protocol header: {error}"),
    }
    let (tag, payload_len) = decode_frame_header(&header).expect("protocol header is valid");
    let mut payload = vec![0_u8; payload_len];
    stream
        .read_exact(&mut payload)
        .await
        .expect("protocol payload is complete");
    Some(decode_frame_payload_owned(tag, payload).expect("protocol payload is valid"))
}

async fn write_message(stream: &mut BoxedIpcStream, message: Message) {
    let frame = EncodedFrame::from_message(message);
    stream
        .write_all(frame.header())
        .await
        .expect("protocol header writes");
    stream
        .write_all(frame.payload())
        .await
        .expect("protocol payload writes");
    stream.flush().await.expect("protocol frame flushes");
}
