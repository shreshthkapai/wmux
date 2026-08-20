pub mod conpty;
pub mod console;
pub mod daemon;
pub mod pipe;

#[cfg(test)]
mod daemon_tests {
    use crate::daemon::{
        bootstrap_script, encode_powershell_command, powershell_literal, quote_windows_argument,
        run_bounded_command, spawn_user_daemon, DaemonSpec,
    };
    use std::{
        ffi::OsString,
        path::PathBuf,
        process::{Command, Stdio},
        sync::mpsc,
        thread,
        time::{Duration, Instant},
    };

    #[test]
    fn daemon_command_line_quotes_empty_spaces_quotes_and_backslashes_before_quotes() {
        // Mutation caught: a launcher that hands WMI an incorrectly encoded
        // Windows command line changes the server's argv.
        assert_eq!(quote_windows_argument(OsString::new().as_os_str()), "\"\"");
        assert_eq!(
            quote_windows_argument(OsString::from("two words").as_os_str()),
            "\"two words\""
        );
        assert_eq!(
            quote_windows_argument(OsString::from("a\"b").as_os_str()),
            r#""a\"b""#
        );
        assert_eq!(
            quote_windows_argument(OsString::from(r#"C:\path\"tail"#).as_os_str()),
            r#""C:\path\\\"tail""#
        );
        assert_eq!(
            quote_windows_argument(OsString::from("a\"\\\\").as_os_str()),
            r#""a\"\\\\""#
        );
    }

    #[test]
    fn daemon_powershell_literals_escape_apostrophes() {
        // Mutation caught: an apostrophe in an executable path or working
        // directory terminates the PowerShell literal before WMI sees it.
        assert_eq!(
            powershell_literal("C:\\Users\\O'Brien\\wmux-server.exe"),
            "'C:\\Users\\O''Brien\\wmux-server.exe'"
        );
    }

    #[test]
    fn daemon_bootstrap_uses_wmi_startup_environment_and_checks_return_value() {
        // Mutation caught: the provider launches without the requested startup
        // configuration, drops the environment, or treats WMI failure as a PID.
        let script = bootstrap_script(&DaemonSpec {
            executable: PathBuf::from(r"C:\Program Files\wmux\wmux-server.exe"),
            arguments: vec![OsString::from("new-session")],
            current_dir: PathBuf::from(r"C:\Users\O'Brien\work"),
        });
        assert!(script.contains("Win32_ProcessStartup"));
        assert!(script.contains("EnvironmentVariables"));
        assert!(script.contains("Invoke-CimMethod"));
        assert!(script.contains("$result.ReturnValue -ne 0"));
        assert!(script.contains("134219264"));
        assert!(script.contains("C:\\Users\\O''Brien\\work"));

        let bytes = decode_base64_for_test(&encode_powershell_command(&script));
        assert_eq!(bytes.len() % 2, 0);
        let code_units = bytes
            .chunks_exact(2)
            .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
            .collect::<Vec<_>>();
        assert_eq!(String::from_utf16(&code_units).unwrap(), script);
    }

    #[test]
    fn daemon_bootstrap_timeout_kills_and_reaps_a_stalled_powershell() {
        // Mutation caught: a stalled CIM bootstrap makes the caller wait
        // forever, or leaves a timed-out bootstrap process behind.
        let mut command = Command::new(windows_powershell());
        command
            .args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command"])
            .arg("[Console]::Out.Write('x' * 1048576); Start-Sleep -Seconds 30")
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let started = Instant::now();
        let error = run_bounded_command(&mut command, Duration::from_millis(300)).unwrap_err();

        assert_eq!(error.kind(), std::io::ErrorKind::TimedOut);
        assert!(error.to_string().contains("timed out"));
        assert!(started.elapsed() < Duration::from_secs(3));
    }

    #[test]
    fn daemon_bootstrap_does_not_wait_for_descendant_held_output_handles() {
        // Mutation caught: output collection waits for EOF after the bootstrap
        // exits, even though a descendant still owns the inherited write ends.
        let pid_path = std::env::temp_dir().join(format!(
            "wmux-daemon-descendant-{}-{}.pid",
            std::process::id(),
            Instant::now().elapsed().as_nanos()
        ));
        let descendant = windows_powershell();
        let script = format!(
            "$psi = [Diagnostics.ProcessStartInfo]::new(); \
             $psi.FileName = {}; \
             $psi.Arguments = '-NoLogo -NoProfile -NonInteractive -Command \"Start-Sleep -Seconds 30\"'; \
             $psi.UseShellExecute = $false; \
             $child = [Diagnostics.Process]::Start($psi); \
             [IO.File]::WriteAllText({}, [string]$child.Id)",
            powershell_literal(&descendant.display().to_string()),
            powershell_literal(&pid_path.display().to_string()),
        );
        let mut command = Command::new(windows_powershell());
        command
            .args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command"])
            .arg(script)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());
        let (result_tx, result_rx) = mpsc::channel();
        let worker = thread::spawn(move || {
            let _ = result_tx.send(run_bounded_command(&mut command, Duration::from_secs(1)));
        });

        let mut descendant = ProcessCleanup::new(wait_for_pid(&pid_path));
        let result = result_rx.recv_timeout(Duration::from_millis(750));
        descendant.terminate_and_wait().unwrap();
        worker.join().unwrap();
        let _ = std::fs::remove_file(&pid_path);

        assert!(
            result.is_ok(),
            "bootstrap waited for a descendant-held output handle: {result:?}"
        );
    }

    #[test]
    fn daemon_spawn_leaves_the_wmi_created_process_alive_until_cleaned_up() {
        // Mutation caught: replacing WMI with an ordinary child process ties
        // daemon lifetime to the transient PowerShell bootstrap.
        let spec = DaemonSpec {
            executable: windows_powershell(),
            arguments: vec![
                OsString::from("-NoLogo"),
                OsString::from("-NoProfile"),
                OsString::from("-NonInteractive"),
                OsString::from("-Command"),
                OsString::from("Start-Sleep -Seconds 30"),
            ],
            current_dir: std::env::current_dir().unwrap(),
        };
        let pid = spawn_user_daemon(&spec).unwrap();
        assert_ne!(pid, 0);
        let mut process = ProcessCleanup::new(pid);
        assert!(
            process.is_active(),
            "WMI-created process {pid} exited with the bootstrap"
        );
        process.terminate_and_wait().unwrap();
    }

    fn windows_powershell() -> PathBuf {
        let system_root = std::env::var_os("SystemRoot").expect("SystemRoot is set on Windows");
        PathBuf::from(system_root)
            .join("System32")
            .join("WindowsPowerShell")
            .join("v1.0")
            .join("powershell.exe")
    }

    fn wait_for_pid(path: &std::path::Path) -> u32 {
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            if let Ok(pid) = std::fs::read_to_string(path).and_then(|text| {
                text.trim()
                    .parse()
                    .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error))
            }) {
                return pid;
            }
            assert!(Instant::now() < deadline, "descendant PID was not written");
            thread::sleep(Duration::from_millis(10));
        }
    }

    struct ProcessCleanup {
        pid: u32,
        handle: Option<windows_sys::Win32::Foundation::HANDLE>,
    }

    impl ProcessCleanup {
        fn new(pid: u32) -> Self {
            Self { pid, handle: None }
        }

        fn open_handle(&mut self) -> std::io::Result<windows_sys::Win32::Foundation::HANDLE> {
            use windows_sys::Win32::{
                Foundation::INVALID_HANDLE_VALUE,
                System::Threading::{
                    OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE,
                },
            };

            let handle = unsafe {
                OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | 0x0010_0000,
                    0,
                    self.pid,
                )
            };
            if handle.is_null() || handle == INVALID_HANDLE_VALUE {
                Err(std::io::Error::last_os_error())
            } else {
                self.handle = Some(handle);
                Ok(handle)
            }
        }

        fn is_active(&mut self) -> bool {
            use windows_sys::Win32::{
                Foundation::STILL_ACTIVE, System::Threading::GetExitCodeProcess,
            };

            let handle = self.open_handle().unwrap();
            let mut exit_code = 0_u32;
            assert_ne!(
                unsafe { GetExitCodeProcess(handle, &mut exit_code) },
                0,
                "failed to query WMI-created process: {}",
                std::io::Error::last_os_error()
            );
            exit_code == STILL_ACTIVE as u32
        }

        fn terminate_and_wait(&mut self) -> std::io::Result<()> {
            use windows_sys::Win32::{
                Foundation::{CloseHandle, WAIT_OBJECT_0},
                System::Threading::{TerminateProcess, WaitForSingleObject},
            };

            let handle = match self.handle {
                Some(handle) => handle,
                None => self.open_handle()?,
            };
            if unsafe { TerminateProcess(handle, 1) } == 0 {
                return Err(std::io::Error::last_os_error());
            }
            if unsafe { WaitForSingleObject(handle, 2_000) } != WAIT_OBJECT_0 {
                return Err(std::io::Error::new(
                    std::io::ErrorKind::TimedOut,
                    "timed out waiting for WMI-created process cleanup",
                ));
            }
            let _ = unsafe { CloseHandle(handle) };
            self.handle = None;
            Ok(())
        }
    }

    impl Drop for ProcessCleanup {
        fn drop(&mut self) {
            use windows_sys::Win32::{
                Foundation::CloseHandle,
                System::Threading::{TerminateProcess, WaitForSingleObject},
            };

            let handle = self.handle.take().or_else(|| self.open_handle().ok());
            if let Some(handle) = handle {
                let _ = unsafe { TerminateProcess(handle, 1) };
                let _ = unsafe { WaitForSingleObject(handle, 2_000) };
                let _ = unsafe { CloseHandle(handle) };
            }
        }
    }

    fn decode_base64_for_test(input: &str) -> Vec<u8> {
        input
            .as_bytes()
            .chunks_exact(4)
            .flat_map(|chunk| {
                let value = |byte| match byte {
                    b'A'..=b'Z' => byte - b'A',
                    b'a'..=b'z' => byte - b'a' + 26,
                    b'0'..=b'9' => byte - b'0' + 52,
                    b'+' => 62,
                    b'/' => 63,
                    b'=' => 0,
                    _ => panic!("invalid base64 input"),
                };
                let values = [
                    value(chunk[0]),
                    value(chunk[1]),
                    value(chunk[2]),
                    value(chunk[3]),
                ];
                let first = (values[0] << 2) | (values[1] >> 4);
                let second = ((values[1] & 0b1111) << 4) | (values[2] >> 2);
                let third = ((values[2] & 0b11) << 6) | values[3];
                let mut decoded = vec![first];
                if chunk[2] != b'=' {
                    decoded.push(second);
                }
                if chunk[3] != b'=' {
                    decoded.push(third);
                }
                decoded
            })
            .collect()
    }
}
