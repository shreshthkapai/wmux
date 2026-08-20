use mux_cli::{parse_args, CliCommand};
use mux_platform_windows::{
    console::reset_console_modes,
    console::{
        flush_console_input, has_console_input, query_terminal_size, read_console_input,
        write_console_output, ConsoleModeGuard,
    },
    named_pipe::{connect, has_pending_bytes, is_server_running, NamedPipeEndpoint},
};
use mux_protocol::{
    read_message, write_message, CommandRequest, DetachRequest, ProtocolMessage, PROTOCOL_VERSION,
};
use std::{
    fs::OpenOptions,
    io,
    io::Write,
    process, thread,
    time::{Duration, Instant},
};

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        process::exit(1);
    }
}

fn run() -> io::Result<()> {
    let command = parse_args(std::env::args().skip(1));

    match command {
        CliCommand::ServerStart => start_server(),
        CliCommand::ServerStatus => match send_command("server status") {
            Ok(()) => Ok(()),
            Err(error) if is_not_running_error(&error) => {
                println!("no server running");
                Ok(())
            }
            Err(error) => Err(error),
        },
        CliCommand::ServerStop => send_command("server stop"),
        CliCommand::ResetTerminal => {
            reset_console_modes()?;
            println!("terminal reset");
            Ok(())
        }
        CliCommand::ListClients => send_command("list-clients"),
        CliCommand::NewSession { name } => {
            ensure_server_started()?;
            run_attached_command(format_command_with_target("new-session", "-s", name))
        }
        CliCommand::AttachSession { target } => {
            run_attached_command(format_command_with_target("attach-session", "-t", target))
        }
        CliCommand::Raw(args) => send_command(&args.join(" ")),
    }
}

fn start_server() -> io::Result<()> {
    let endpoint = NamedPipeEndpoint::default_for_current_user();
    if is_server_running(&endpoint) {
        println!("server already running");
        return Ok(());
    }

    spawn_server_process()?;
    println!("server starting");
    Ok(())
}

fn ensure_server_started() -> io::Result<()> {
    let endpoint = NamedPipeEndpoint::default_for_current_user();
    if is_server_running(&endpoint) {
        return Ok(());
    }

    spawn_server_process()?;
    thread::sleep(Duration::from_millis(200));
    Ok(())
}

fn spawn_server_process() -> io::Result<()> {
    let server_path = std::env::current_exe()?.with_file_name("wmux-server.exe");
    if !server_path.exists() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!(
                "wmux-server.exe not found next to client at {}",
                server_path.display()
            ),
        ));
    }

    std::process::Command::new(server_path)
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::from(open_log_file(
            "wmux-server.out.log",
        )?))
        .stderr(std::process::Stdio::from(open_log_file(
            "wmux-server.err.log",
        )?))
        .spawn()?;
    Ok(())
}

fn send_command(command: &str) -> io::Result<()> {
    let mut pipe = connect_and_handshake()?;

    write_message(
        &mut pipe,
        2,
        &ProtocolMessage::Command(CommandRequest {
            command: command.to_string(),
        }),
    )?;

    match read_message(&mut pipe)? {
        Some((_, ProtocolMessage::CommandResponse(response))) => {
            println!("{}", response.message);
            if response.success {
                Ok(())
            } else {
                Err(io::Error::other("server command failed"))
            }
        }
        Some((_, other)) => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("unexpected command response: {other:?}"),
        )),
        None => Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "server closed connection before command response",
        )),
    }
}

fn run_attached_command(command: String) -> io::Result<()> {
    debug_log(format!("attach_start command={command:?}"));
    let mut pipe = connect_and_handshake()?;
    let terminal = ConsoleModeGuard::enter()?;
    let screen = AlternateScreenGuard::enter()?;
    let _ = flush_console_input();

    write_message(
        &mut pipe,
        2,
        &ProtocolMessage::Command(CommandRequest { command }),
    )?;
    debug_log("attach_command_sent");

    wait_for_attach_response(&mut pipe)?;
    debug_log("attach_response_ok");

    let mut last_size = query_terminal_size()
        .unwrap_or_else(|_| mux_platform::terminal::TerminalSize::cells(80, 24));
    send_resize(&mut pipe, last_size)?;
    warm_attach_display(&mut pipe)?;
    let _ = flush_console_input();

    let mut input_buffer = [0_u8; 1024];
    let mut prefix = false;
    let mut last_resize_poll = Instant::now();
    let mut stop = false;

    while !stop {
        let mut did_work = false;
        match drain_attached_output(&mut pipe)? {
            DrainResult::Drained => did_work = true,
            DrainResult::Exit => break,
            DrainResult::Idle => {}
        }

        if has_console_input()? {
            did_work = true;
            let n = read_console_input(&mut input_buffer)?;
            debug_log(format!("attach_input bytes={n}"));
            if n > 0 && handle_attached_input(&mut pipe, &input_buffer[..n], &mut prefix)? {
                debug_log("attach_detach_requested");
                stop = true;
            }
        }

        if last_resize_poll.elapsed() >= Duration::from_millis(250) {
            last_resize_poll = Instant::now();
            if let Ok(size) = query_terminal_size() {
                if size != last_size {
                    last_size = size;
                    send_resize(&mut pipe, size)?;
                }
            }
        }

        if !did_work {
            thread::sleep(Duration::from_millis(5));
        }
    }

    drop(screen);
    drop(terminal);
    reset_console_modes()?;
    debug_log("attach_terminal_restored");
    Ok(())
}

#[derive(Debug)]
struct AlternateScreenGuard {
    active: bool,
}

impl AlternateScreenGuard {
    fn enter() -> io::Result<Self> {
        write_console_output(b"\x1b[?1049h\x1b[?2004l\x1b[?25h\x1b[H\x1b[2J")?;
        Ok(Self { active: true })
    }
}

impl Drop for AlternateScreenGuard {
    fn drop(&mut self) {
        if self.active {
            let _ = write_console_output(b"\x1b[0m\x1b[?2004l\x1b[?25h\x1b[?1049l");
            self.active = false;
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum DrainResult {
    Idle,
    Drained,
    Exit,
}

fn warm_attach_display(pipe: &mut std::fs::File) -> io::Result<()> {
    let started = Instant::now();
    let mut last_output = Instant::now();
    while started.elapsed() < Duration::from_millis(750) {
        match drain_attached_output(pipe)? {
            DrainResult::Drained => {
                last_output = Instant::now();
            }
            DrainResult::Exit => return Ok(()),
            DrainResult::Idle => {
                if last_output.elapsed() >= Duration::from_millis(75) {
                    return Ok(());
                }
                thread::sleep(Duration::from_millis(5));
            }
        }
    }
    Ok(())
}

fn drain_attached_output(pipe: &mut std::fs::File) -> io::Result<DrainResult> {
    let mut drained = false;
    loop {
        match has_pending_bytes(pipe) {
            Ok(true) => {}
            Ok(false) => {
                return Ok(if drained {
                    DrainResult::Drained
                } else {
                    DrainResult::Idle
                });
            }
            Err(error)
                if error.kind() == io::ErrorKind::BrokenPipe
                    || error.raw_os_error() == Some(109) =>
            {
                return Ok(DrainResult::Exit);
            }
            Err(error) => return Err(error),
        }

        drained = true;
        match read_message(pipe)? {
            Some((_, ProtocolMessage::Output { bytes })) => {
                debug_log(format!("attach_output bytes={}", bytes.len()));
                write_console_output(&bytes)?;
            }
            Some((_, ProtocolMessage::Exit { message, .. })) => {
                debug_log(format!("attach_exit message={message:?}"));
                if let Some(message) = message {
                    write_console_output(format!("\r\n{message}\r\n").as_bytes())?;
                }
                return Ok(DrainResult::Exit);
            }
            Some((_, ProtocolMessage::CommandResponse(response))) => {
                if !response.success {
                    return Err(io::Error::other(response.message));
                }
            }
            Some((_, _)) => {}
            None => return Ok(DrainResult::Exit),
        }
    }
}

fn wait_for_attach_response(pipe: &mut std::fs::File) -> io::Result<()> {
    loop {
        match read_message(pipe)? {
            Some((_, ProtocolMessage::CommandResponse(response))) => {
                if response.success {
                    debug_log("attach_command_response_success");
                    return Ok(());
                }
                debug_log(format!(
                    "attach_command_response_error message={:?}",
                    response.message
                ));
                return Err(io::Error::other(response.message));
            }
            Some((_, ProtocolMessage::Output { bytes })) => {
                debug_log(format!("attach_early_output bytes={}", bytes.len()));
                write_console_output(&bytes)?;
            }
            Some((_, ProtocolMessage::Exit { message, .. })) => {
                return Err(io::Error::other(
                    message.unwrap_or_else(|| "server ended attach".to_string()),
                ));
            }
            Some((_, _)) => {}
            None => {
                return Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "server closed connection before attach response",
                ));
            }
        }
    }
}

fn handle_attached_input(
    pipe: &mut std::fs::File,
    bytes: &[u8],
    prefix: &mut bool,
) -> io::Result<bool> {
    let mut pending = Vec::with_capacity(bytes.len());
    for byte in bytes {
        if *prefix {
            if *byte == b'd' || *byte == b'D' || *byte == 0x04 {
                debug_log("detach_sequence_matched");
                write_message(
                    pipe,
                    0,
                    &ProtocolMessage::Detach(DetachRequest {
                        reason: Some("detached".to_string()),
                    }),
                )?;
                return Ok(true);
            }
            pending.push(0x02);
            pending.push(*byte);
            *prefix = false;
        } else if *byte == 0x1d {
            debug_log("detach_direct_matched");
            write_message(
                pipe,
                0,
                &ProtocolMessage::Detach(DetachRequest {
                    reason: Some("detached".to_string()),
                }),
            )?;
            return Ok(true);
        } else if *byte == 0x02 {
            debug_log("prefix_key_seen");
            *prefix = true;
        } else {
            pending.push(*byte);
        }
    }

    if !pending.is_empty() {
        write_message(pipe, 0, &ProtocolMessage::Input { bytes: pending })?;
    }
    Ok(false)
}

fn open_log_file(name: &str) -> io::Result<std::fs::File> {
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(std::env::temp_dir().join(name))
}

fn debug_log(message: impl AsRef<str>) {
    if let Ok(mut file) = open_log_file("wmux-client.log") {
        let _ = writeln!(file, "pid={} {}", process::id(), message.as_ref());
    }
}

fn send_resize(
    pipe: &mut std::fs::File,
    size: mux_platform::terminal::TerminalSize,
) -> io::Result<()> {
    write_message(
        pipe,
        0,
        &ProtocolMessage::Resize {
            cols: size.columns,
            rows: size.rows,
            xpixel: size.xpixel,
            ypixel: size.ypixel,
        },
    )
}

fn connect_and_handshake() -> io::Result<std::fs::File> {
    let endpoint = NamedPipeEndpoint::default_for_current_user();
    let mut pipe = connect_with_retry(&endpoint).map_err(|error| {
        io::Error::new(
            error.kind(),
            format!(
                "could not connect to wmux server at {}: {error}",
                endpoint.pipe_name()
            ),
        )
    })?;

    write_message(
        &mut pipe,
        1,
        &ProtocolMessage::Hello {
            version: PROTOCOL_VERSION,
            client_pid: process::id(),
        },
    )?;

    match read_message(&mut pipe)? {
        Some((
            _,
            ProtocolMessage::HelloOk {
                version,
                server_pid,
            },
        )) if version == PROTOCOL_VERSION => {
            let _ = server_pid;
            Ok(pipe)
        }
        Some((_, ProtocolMessage::HelloError { message, .. })) => {
            Err(io::Error::new(io::ErrorKind::InvalidData, message))
        }
        Some((_, other)) => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("unexpected handshake response: {other:?}"),
        )),
        None => Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "server closed connection during handshake",
        )),
    }
}

fn connect_with_retry(endpoint: &NamedPipeEndpoint) -> io::Result<std::fs::File> {
    let mut last_error = None;
    for _ in 0..20 {
        match connect(endpoint) {
            Ok(pipe) => return Ok(pipe),
            Err(error) => {
                last_error = Some(error);
                thread::sleep(Duration::from_millis(50));
            }
        }
    }
    Err(last_error.unwrap_or_else(|| io::Error::other("connection retry failed")))
}

fn is_not_running_error(error: &io::Error) -> bool {
    error.kind() == io::ErrorKind::NotFound
        || error
            .to_string()
            .contains("The system cannot find the file specified")
}

fn format_command_with_target(command: &str, flag: &str, target: Option<String>) -> String {
    match target {
        Some(target) => format!("{command} {flag} {target}"),
        None => command.to_string(),
    }
}
