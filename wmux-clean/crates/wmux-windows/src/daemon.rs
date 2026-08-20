use std::{
    ffi::{OsStr, OsString},
    io::{self, Read},
    os::windows::{ffi::OsStrExt, process::CommandExt},
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Stdio},
    thread,
    time::{Duration, Instant},
};

const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const WMI_CREATE_FLAGS: u32 = 134_219_264;
const MAX_DIAGNOSTIC_BYTES: usize = 4_096;
const MAX_CAPTURED_OUTPUT_BYTES: usize = MAX_DIAGNOSTIC_BYTES * 2;
const BOOTSTRAP_TIMEOUT: Duration = Duration::from_secs(5);
const BOOTSTRAP_POLL_INTERVAL: Duration = Duration::from_millis(10);

/// The server command and working directory that the local WMI provider must
/// launch independently from the disposable client process.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DaemonSpec {
    pub executable: PathBuf,
    pub arguments: Vec<OsString>,
    pub current_dir: PathBuf,
}

/// Quotes one argument according to the Windows command-line parsing rules.
pub fn quote_windows_argument(argument: &OsStr) -> String {
    let argument = String::from_utf16_lossy(&argument.encode_wide().collect::<Vec<_>>());
    if !argument.is_empty()
        && !argument
            .chars()
            .any(|character| character == '"' || character.is_whitespace())
    {
        return argument;
    }

    let mut quoted = String::with_capacity(argument.len() + 2);
    quoted.push('"');
    let mut backslashes = 0;
    for character in argument.chars() {
        match character {
            '\\' => backslashes += 1,
            '"' => {
                quoted.extend(std::iter::repeat_n('\\', backslashes * 2 + 1));
                quoted.push('"');
                backslashes = 0;
            }
            _ => {
                quoted.extend(std::iter::repeat_n('\\', backslashes));
                quoted.push(character);
                backslashes = 0;
            }
        }
    }
    quoted.extend(std::iter::repeat_n('\\', backslashes * 2));
    quoted.push('"');
    quoted
}

/// Starts `spec` through the local user's WMI provider and returns its PID.
///
/// The provider, instead of the terminal-hosted client, owns the process
/// creation relationship. This is necessary because terminal hosts can use
/// kill-on-close Job Objects that deny normal child-process breakaway.
pub fn spawn_user_daemon(spec: &DaemonSpec) -> io::Result<u32> {
    let powershell = windows_powershell_path()?;
    let encoded_command = encode_powershell_command(&bootstrap_script(spec));
    let mut command = Command::new(powershell);
    command
        .args([
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-EncodedCommand",
        ])
        .arg(encoded_command)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .creation_flags(CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP);
    let output = run_bounded_command(&mut command, BOOTSTRAP_TIMEOUT)?;

    if !output.status.success() {
        return Err(io::Error::other(format!(
            "wmux daemon bootstrap failed ({}){}",
            exit_description(output.status.code()),
            output_diagnostic(&output.stderr)
        )));
    }

    let pid_text = String::from_utf8_lossy(&output.stdout);
    let pid = pid_text.trim().parse::<u32>().map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "wmux daemon bootstrap returned an invalid process ID: {}{}",
                bounded_text(&pid_text),
                output_diagnostic(&output.stderr)
            ),
        )
    })?;
    if pid == 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "wmux daemon bootstrap returned process ID zero{}",
                output_diagnostic(&output.stderr)
            ),
        ));
    }
    Ok(pid)
}

fn windows_powershell_path() -> io::Result<PathBuf> {
    let system_root = std::env::var_os("SystemRoot").ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::NotFound,
            "could not resolve Windows PowerShell because SystemRoot is not set",
        )
    })?;
    Ok(PathBuf::from(system_root)
        .join("System32")
        .join("WindowsPowerShell")
        .join("v1.0")
        .join("powershell.exe"))
}

pub(crate) fn bootstrap_script(spec: &DaemonSpec) -> String {
    let command_line = std::iter::once(quote_windows_argument(spec.executable.as_os_str()))
        .chain(
            spec.arguments
                .iter()
                .map(|argument| quote_windows_argument(argument)),
        )
        .collect::<Vec<_>>()
        .join(" ");
    let current_dir = path_text(&spec.current_dir);
    format!(
        r#"$startupClass = Get-CimClass -ClassName Win32_ProcessStartup
$startup = New-CimInstance -CimClass $startupClass -ClientOnly -Property @{{
  ShowWindow = [uint16]0
  CreateFlags = [uint32]{WMI_CREATE_FLAGS}
  EnvironmentVariables = [string[]]@([Environment]::GetEnvironmentVariables().GetEnumerator() |
    ForEach-Object {{ "$($_.Key)=$($_.Value)" }})
}}
$result = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{{
  CommandLine = {}
  CurrentDirectory = {}
  ProcessStartupInformation = $startup
}}
if ($result.ReturnValue -ne 0) {{ throw "Win32_Process.Create failed: $($result.ReturnValue)" }}
[Console]::Out.Write($result.ProcessId)"#,
        powershell_literal(&command_line),
        powershell_literal(&current_dir),
    )
}

pub(crate) fn powershell_literal(value: &str) -> String {
    format!("'{}'", value.replace('\'', "''"))
}

fn path_text(path: &Path) -> String {
    String::from_utf16_lossy(&path.as_os_str().encode_wide().collect::<Vec<_>>())
}

pub(crate) fn encode_powershell_command(script: &str) -> String {
    let utf16le = script
        .encode_utf16()
        .flat_map(u16::to_le_bytes)
        .collect::<Vec<_>>();
    encode_base64(&utf16le)
}

#[derive(Debug)]
pub(crate) struct BootstrapOutput {
    status: ExitStatus,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}

pub(crate) fn run_bounded_command(
    command: &mut Command,
    timeout: Duration,
) -> io::Result<BootstrapOutput> {
    let mut child = command.spawn()?;
    let stdout = child.stdout.take().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::BrokenPipe,
            "wmux daemon bootstrap stdout was not captured",
        )
    })?;
    let stderr = child.stderr.take().ok_or_else(|| {
        io::Error::new(
            io::ErrorKind::BrokenPipe,
            "wmux daemon bootstrap stderr was not captured",
        )
    })?;
    let stdout_reader = thread::spawn(move || drain_limited(stdout));
    let stderr_reader = thread::spawn(move || drain_limited(stderr));
    let started = Instant::now();

    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                return Ok(BootstrapOutput {
                    status,
                    stdout: join_drain(stdout_reader)?,
                    stderr: join_drain(stderr_reader)?,
                });
            }
            Ok(None) if started.elapsed() < timeout => thread::sleep(BOOTSTRAP_POLL_INTERVAL),
            Ok(None) => {
                let output = kill_reap_and_collect(&mut child, stdout_reader, stderr_reader);
                let diagnostic = output
                    .as_ref()
                    .map(|output| output_diagnostic(&output.stderr))
                    .unwrap_or_default();
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    format!(
                        "wmux daemon bootstrap timed out after {} ms and was terminated{}",
                        timeout.as_millis(),
                        diagnostic
                    ),
                ));
            }
            Err(error) => {
                let _ = kill_reap_and_collect(&mut child, stdout_reader, stderr_reader);
                return Err(error);
            }
        }
    }
}

fn kill_reap_and_collect(
    child: &mut Child,
    stdout_reader: thread::JoinHandle<io::Result<Vec<u8>>>,
    stderr_reader: thread::JoinHandle<io::Result<Vec<u8>>>,
) -> io::Result<BootstrapOutput> {
    let _ = child.kill();
    let status = child.wait()?;
    Ok(BootstrapOutput {
        status,
        stdout: join_drain(stdout_reader)?,
        stderr: join_drain(stderr_reader)?,
    })
}

fn drain_limited(mut reader: impl Read) -> io::Result<Vec<u8>> {
    let mut captured = Vec::with_capacity(MAX_CAPTURED_OUTPUT_BYTES);
    let mut buffer = [0_u8; 8_192];
    loop {
        let read = reader.read(&mut buffer)?;
        if read == 0 {
            return Ok(captured);
        }
        let remaining = MAX_CAPTURED_OUTPUT_BYTES.saturating_sub(captured.len());
        captured.extend_from_slice(&buffer[..read.min(remaining)]);
    }
}

fn join_drain(reader: thread::JoinHandle<io::Result<Vec<u8>>>) -> io::Result<Vec<u8>> {
    reader
        .join()
        .map_err(|_| io::Error::other("wmux daemon bootstrap output reader panicked"))?
}

fn encode_base64(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut result = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let first = chunk[0];
        let second = *chunk.get(1).unwrap_or(&0);
        let third = *chunk.get(2).unwrap_or(&0);
        result.push(char::from(TABLE[(first >> 2) as usize]));
        result.push(char::from(
            TABLE[(((first & 0b11) << 4) | (second >> 4)) as usize],
        ));
        result.push(if chunk.len() > 1 {
            char::from(TABLE[(((second & 0b1111) << 2) | (third >> 6)) as usize])
        } else {
            '='
        });
        result.push(if chunk.len() > 2 {
            char::from(TABLE[(third & 0b11_1111) as usize])
        } else {
            '='
        });
    }
    result
}

fn exit_description(code: Option<i32>) -> String {
    match code {
        Some(code) => format!("exit code {code}"),
        None => "terminated without an exit code".to_string(),
    }
}

fn output_diagnostic(stderr: &[u8]) -> String {
    let stderr = bounded_text(&String::from_utf8_lossy(stderr));
    if stderr.is_empty() {
        String::new()
    } else {
        format!(": {stderr}")
    }
}

fn bounded_text(text: &str) -> String {
    let trimmed = text.trim();
    if trimmed.len() <= MAX_DIAGNOSTIC_BYTES {
        return trimmed.to_string();
    }
    let mut end = MAX_DIAGNOSTIC_BYTES;
    while !trimmed.is_char_boundary(end) {
        end -= 1;
    }
    format!("{}...", &trimmed[..end])
}
