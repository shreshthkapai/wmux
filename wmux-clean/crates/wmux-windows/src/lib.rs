pub mod conpty;
pub mod console;
pub mod daemon;
pub mod pipe;

#[cfg(test)]
mod daemon_tests {
    use crate::daemon::{
        bootstrap_script, powershell_literal, quote_windows_argument, spawn_user_daemon, DaemonSpec,
    };
    use std::{ffi::OsString, path::PathBuf};

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
    }

    #[test]
    fn daemon_spawn_leaves_the_wmi_created_process_alive_until_cleaned_up() {
        // Mutation caught: replacing WMI with an ordinary child process ties
        // daemon lifetime to the transient PowerShell bootstrap.
        let system_root = std::env::var_os("SystemRoot").expect("SystemRoot is set on Windows");
        let powershell = PathBuf::from(system_root)
            .join("System32")
            .join("WindowsPowerShell")
            .join("v1.0")
            .join("powershell.exe");
        let spec = DaemonSpec {
            executable: powershell,
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
        let process = ProcessCleanup::open(pid);
        assert!(
            process.is_active(),
            "WMI-created process {pid} exited with the bootstrap"
        );
    }

    struct ProcessCleanup(windows_sys::Win32::Foundation::HANDLE);

    impl ProcessCleanup {
        fn open(pid: u32) -> Self {
            use windows_sys::Win32::{
                Foundation::INVALID_HANDLE_VALUE,
                System::Threading::{
                    OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE,
                },
            };

            let handle = unsafe {
                OpenProcess(
                    PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                    0,
                    pid,
                )
            };
            assert!(
                !handle.is_null() && handle != INVALID_HANDLE_VALUE,
                "failed to open daemon PID {pid}: {}",
                std::io::Error::last_os_error()
            );
            Self(handle)
        }

        fn is_active(&self) -> bool {
            use windows_sys::Win32::{
                Foundation::STILL_ACTIVE, System::Threading::GetExitCodeProcess,
            };

            let mut exit_code = 0_u32;
            assert_ne!(
                unsafe { GetExitCodeProcess(self.0, &mut exit_code) },
                0,
                "failed to query WMI-created process: {}",
                std::io::Error::last_os_error()
            );
            exit_code == STILL_ACTIVE as u32
        }
    }

    impl Drop for ProcessCleanup {
        fn drop(&mut self) {
            use windows_sys::Win32::{
                Foundation::CloseHandle, System::Threading::TerminateProcess,
            };

            let _ = unsafe { TerminateProcess(self.0, 1) };
            let _ = unsafe { CloseHandle(self.0) };
        }
    }
}
