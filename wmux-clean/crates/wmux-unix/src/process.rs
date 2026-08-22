use nix::pty::{openpty, Winsize};
use std::{
    io,
    os::{
        fd::{AsRawFd, OwnedFd},
        unix::process::CommandExt,
    },
    path::Path,
    process::{Child, Command},
};
use wmux_platform::SpawnPane;

pub(crate) struct SpawnedPane {
    pub(crate) master: OwnedFd,
    pub(crate) child: Child,
    pub(crate) process_group: libc::pid_t,
}

pub(crate) fn spawn_pane(request: &SpawnPane) -> io::Result<SpawnedPane> {
    let pty = openpty(
        Some(&Winsize {
            ws_row: request.size.rows.max(1),
            ws_col: request.size.cols.max(1),
            ws_xpixel: 0,
            ws_ypixel: 0,
        }),
        None,
    )
    .map_err(nix_error)?;
    let master_fd = pty.master.as_raw_fd();
    let slave_fd = pty.slave.as_raw_fd();
    let mut command = if let Some(spec) = &request.command {
        let mut command = Command::new(&spec.program);
        command.args(&spec.args);
        command
    } else {
        Command::new(default_shell())
    };
    if let Some(cwd) = &request.cwd {
        command.current_dir(cwd);
    }
    command.envs(request.environment.iter().map(|(key, value)| (key, value)));
    unsafe {
        command.pre_exec(move || child_terminal_setup(master_fd, slave_fd));
    }
    let child = command.spawn()?;
    drop(pty.slave);
    let process_group = child.id() as libc::pid_t;
    Ok(SpawnedPane {
        master: pty.master,
        child,
        process_group,
    })
}

fn default_shell() -> std::ffi::OsString {
    std::env::var_os("SHELL")
        .filter(|shell| Path::new(shell).is_absolute())
        .unwrap_or_else(|| std::ffi::OsString::from("/bin/sh"))
}

fn child_terminal_setup(master_fd: libc::c_int, slave_fd: libc::c_int) -> io::Result<()> {
    if master_fd > libc::STDERR_FILENO {
        unsafe { libc::close(master_fd) };
    }
    // Match zellij's proven Unix PTY setup: login_tty creates a session,
    // acquires the slave as the controlling terminal, makes this process the
    // foreground process group, duplicates it onto stdio, and closes the
    // original descriptor.
    if unsafe { libc::login_tty(slave_fd) } == -1 {
        return Err(io::Error::last_os_error());
    }
    reset_signal_state()
}

fn reset_signal_state() -> io::Result<()> {
    let mut default_action = unsafe { std::mem::zeroed::<libc::sigaction>() };
    default_action.sa_sigaction = libc::SIG_DFL;
    if unsafe { libc::sigemptyset(&mut default_action.sa_mask) } == -1 {
        return Err(io::Error::last_os_error());
    }
    for signal in [
        libc::SIGPIPE,
        libc::SIGTSTP,
        libc::SIGTTIN,
        libc::SIGTTOU,
        libc::SIGQUIT,
        libc::SIGINT,
        libc::SIGHUP,
        libc::SIGCHLD,
        libc::SIGCONT,
        libc::SIGTERM,
        libc::SIGUSR1,
        libc::SIGUSR2,
        libc::SIGWINCH,
    ] {
        if unsafe { libc::sigaction(signal, &default_action, std::ptr::null_mut()) } == -1 {
            return Err(io::Error::last_os_error());
        }
    }
    let mut empty_mask = std::mem::MaybeUninit::<libc::sigset_t>::uninit();
    if unsafe { libc::sigemptyset(empty_mask.as_mut_ptr()) } == -1 {
        return Err(io::Error::last_os_error());
    }
    if unsafe { libc::sigprocmask(libc::SIG_SETMASK, empty_mask.as_ptr(), std::ptr::null_mut()) }
        == -1
    {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

pub(crate) fn signal_group(process_group: libc::pid_t, signal: libc::c_int) -> io::Result<()> {
    if process_group <= 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "process group must be positive",
        ));
    }
    let result = unsafe { libc::kill(-process_group, signal) };
    if result == 0 {
        return Ok(());
    }
    let error = io::Error::last_os_error();
    if error.raw_os_error() == Some(libc::ESRCH) {
        return Ok(());
    }
    Err(error)
}

pub(crate) fn force_kill_and_reap(child: &mut Child, process_group: libc::pid_t) {
    let _ = signal_group(process_group, libc::SIGKILL);
    let _ = child.wait();
}

fn nix_error(error: nix::errno::Errno) -> io::Error {
    io::Error::from_raw_os_error(error as i32)
}

#[cfg(test)]
mod tests {
    use super::{reset_signal_state, spawn_pane};
    use std::{
        ffi::OsString,
        fs,
        io::Read,
        os::fd::OwnedFd,
        path::{Path, PathBuf},
        process,
        time::{SystemTime, UNIX_EPOCH},
    };
    use wmux_platform::{CommandSpec, PlatformPaneId, SpawnPane, TerminalSize};

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new() -> Self {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("test clock is after Unix epoch")
                .as_nanos();
            let path = std::env::temp_dir()
                .join(format!("wmux-unix-process-test-{}-{nonce}", process::id()));
            fs::create_dir(&path).expect("test directory is created");
            Self(path)
        }

        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            if self.0.file_name().is_some_and(|name| {
                name.to_string_lossy()
                    .starts_with("wmux-unix-process-test-")
            }) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
    }

    fn read_pty_to_end(master: OwnedFd) -> Vec<u8> {
        let mut master = fs::File::from(master);
        let mut output = Vec::new();
        let mut chunk = [0_u8; 1024];
        loop {
            match master.read(&mut chunk) {
                Ok(0) => break,
                Ok(read) => output.extend_from_slice(&chunk[..read]),
                Err(error) if error.raw_os_error() == Some(libc::EIO) => break,
                Err(error) => panic!("read PTY output: {error}"),
            }
        }
        output
    }

    #[test]
    fn custom_command_cwd_and_environment_are_applied() {
        let directory = TestDirectory::new();
        let request = SpawnPane {
            pane: PlatformPaneId::new(1),
            size: TerminalSize::new(80, 24),
            command: Some(CommandSpec {
                program: OsString::from("/bin/sh"),
                args: vec![
                    OsString::from("-c"),
                    OsString::from("printf '%s|%s' \"$PWD\" \"$WMUX_TEST\""),
                ],
            }),
            cwd: Some(directory.path().to_path_buf()),
            environment: vec![(OsString::from("WMUX_TEST"), OsString::from("native"))],
        };

        let mut pane = spawn_pane(&request).expect("PTY child spawns");
        let status = pane.child.wait().expect("PTY child is reaped");
        let output = read_pty_to_end(pane.master);

        assert!(status.success());
        assert_eq!(
            String::from_utf8(output).expect("shell output is UTF-8"),
            format!("{}|native", directory.path().display())
        );
    }

    #[test]
    fn child_signal_dispositions_and_mask_are_reset() {
        let child = unsafe { libc::fork() };
        assert_ne!(child, -1, "test helper forks");
        if child == 0 {
            let mut ignored = unsafe { std::mem::zeroed::<libc::sigaction>() };
            ignored.sa_sigaction = libc::SIG_IGN;
            unsafe { libc::sigemptyset(&mut ignored.sa_mask) };
            if unsafe { libc::sigaction(libc::SIGHUP, &ignored, std::ptr::null_mut()) } == -1 {
                unsafe { libc::_exit(1) };
            }
            let mut blocked = unsafe { std::mem::zeroed::<libc::sigset_t>() };
            unsafe {
                libc::sigemptyset(&mut blocked);
                libc::sigaddset(&mut blocked, libc::SIGHUP);
            }
            if unsafe { libc::sigprocmask(libc::SIG_SETMASK, &blocked, std::ptr::null_mut()) } == -1
            {
                unsafe { libc::_exit(2) };
            }
            if reset_signal_state().is_err() {
                unsafe { libc::_exit(3) };
            }
            let mut action = unsafe { std::mem::zeroed::<libc::sigaction>() };
            if unsafe { libc::sigaction(libc::SIGHUP, std::ptr::null(), &mut action) } == -1 {
                unsafe { libc::_exit(4) };
            }
            let mut current_mask = unsafe { std::mem::zeroed::<libc::sigset_t>() };
            if unsafe { libc::sigprocmask(libc::SIG_SETMASK, std::ptr::null(), &mut current_mask) }
                == -1
            {
                unsafe { libc::_exit(5) };
            }
            let disposition_is_default = action.sa_sigaction == libc::SIG_DFL;
            let signal_is_unblocked =
                unsafe { libc::sigismember(&current_mask, libc::SIGHUP) } == 0;
            unsafe { libc::_exit(i32::from(!(disposition_is_default && signal_is_unblocked))) };
        }

        let mut status = 0;
        assert_eq!(unsafe { libc::waitpid(child, &mut status, 0) }, child);
        assert!(libc::WIFEXITED(status));
        assert_eq!(libc::WEXITSTATUS(status), 0);
    }
}
