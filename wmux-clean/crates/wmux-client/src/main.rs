use std::{
    fs::{self, OpenOptions},
    future::Future,
    io::{self, IoSlice, Write},
    process,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
    thread,
    time::Duration,
};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    runtime::Builder as RuntimeBuilder,
    sync::mpsc as async_mpsc,
    time::MissedTickBehavior,
};
use wmux_cli::{ConfigAction, Invocation, ServerInvocation, StartupPolicy};
use wmux_config::{config_path, WmuxConfig};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    FRAME_HEADER_LEN, VERSION,
};
use wmux_windows::{
    console,
    console::ConsoleInput,
    daemon::{spawn_user_daemon, DaemonSpec},
    pipe::{connect_async, Endpoint, NamedPipeClient},
};

const ERROR_PIPE_BUSY_RAW: i32 = 231;

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        process::exit(1);
    }
}

fn run() -> io::Result<()> {
    let args = std::env::args().skip(1).collect::<Vec<_>>();
    match wmux_cli::parse(&args)
        .map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?
    {
        Invocation::Help => {
            print!("{}", wmux_cli::HELP);
            Ok(())
        }
        Invocation::Version => {
            println!("{}", wmux_cli::version_line());
            Ok(())
        }
        Invocation::Config(ConfigAction::Path) => show_config_path(),
        Invocation::Config(ConfigAction::Show) => show_config(),
        Invocation::Config(ConfigAction::Effective) => show_effective_config(),
        Invocation::Server(invocation) => RuntimeBuilder::new_current_thread()
            .enable_all()
            .build()?
            .block_on(run_server_invocation(invocation)),
    }
}

fn show_config_path() -> io::Result<()> {
    println!("{}", config_path().display());
    Ok(())
}

fn show_config() -> io::Result<()> {
    WmuxConfig::load_or_create()?;
    print!("{}", fs::read_to_string(config_path())?);
    Ok(())
}

fn show_effective_config() -> io::Result<()> {
    let config = WmuxConfig::load_or_create()?;
    println!("agent_compat = {}", config.agent_compat);
    println!("agent_ui = {:?}", config.agent_ui);
    for (key, value) in config.pane_environment(1) {
        println!("pane.env.{key} = {value}");
    }
    Ok(())
}

fn spawn_server() -> io::Result<()> {
    let executable = std::env::current_exe()?.with_file_name("wmux-server.exe");
    let _ = spawn_user_daemon(&DaemonSpec {
        executable,
        arguments: Vec::new(),
        current_dir: std::env::current_dir()?,
    })?;
    Ok(())
}

async fn run_server_invocation(invocation: ServerInvocation) -> io::Result<()> {
    let capabilities = terminal_capabilities();
    let pipe = connect_for_invocation(&invocation, capabilities).await?;
    let command = invocation.argv.join(" ");
    if invocation.attached {
        attached_command(pipe, command, capabilities).await
    } else {
        send_command(pipe, command).await
    }
}

async fn send_command(mut pipe: NamedPipeClient, command: String) -> io::Result<()> {
    write_async_message(&mut pipe, Message::Command(command)).await?;
    match read_async_message(&mut pipe).await? {
        Some(Message::CommandOk(message)) => {
            if !message.is_empty() {
                println!("{message}");
            }
            Ok(())
        }
        Some(Message::CommandErr(message)) => Err(io::Error::other(message)),
        Some(other) => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("unexpected response: {other:?}"),
        )),
        None => Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "server closed",
        )),
    }
}

async fn attached_command(
    pipe: NamedPipeClient,
    command: String,
    capabilities: TerminalCapabilities,
) -> io::Result<()> {
    let _console = console::ConsoleGuard::enter()?;
    console::write_output(b"\x1b[?1049h\x1b[?2004h\x1b[?1003h\x1b[?1006h\x1b[?25h\x1b[H\x1b[2J")?;

    let result = attached_inner(pipe, command, capabilities).await;

    let _ = console::write_output(b"\x1b[0m\x1b[?1006l\x1b[?1003l\x1b[?2004l\x1b[?25h\x1b[?1049l");
    result
}

async fn attached_inner(
    mut pipe: NamedPipeClient,
    command: String,
    capabilities: TerminalCapabilities,
) -> io::Result<()> {
    write_async_message(&mut pipe, Message::Command(command)).await?;
    match read_async_message(&mut pipe).await? {
        Some(Message::CommandOk(_)) => {}
        Some(Message::CommandErr(message)) => return Err(io::Error::other(message)),
        Some(other) => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("unexpected attach response: {other:?}"),
            ))
        }
        None => {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "server closed",
            ))
        }
    }

    let initial_size = console::size().unwrap_or(wmux_platform::TerminalSize::new(80, 24));
    write_async_message(
        &mut pipe,
        Message::Resize {
            cols: initial_size.cols,
            rows: initial_size.rows,
        },
    )
    .await?;
    let done = Arc::new(AtomicBool::new(false));

    let (input_tx, input_rx) = async_mpsc::channel(1024);
    let input_done = Arc::clone(&done);
    thread::spawn(move || {
        input_reader(input_tx, input_done);
    });

    let (reader, mut writer) = tokio::io::split(pipe);
    let (inbound_tx, inbound_rx) = async_mpsc::channel(64);
    let inbound_task = tokio::spawn(read_inbound_messages(reader, inbound_tx));
    let result = attach_io_loop(
        &mut writer,
        inbound_rx,
        input_rx,
        done,
        initial_size,
        capabilities,
    )
    .await;
    inbound_task.abort();
    result
}

fn input_reader(tx: async_mpsc::Sender<io::Result<ConsoleInput>>, done: Arc<AtomicBool>) {
    while !done.load(Ordering::SeqCst) {
        match console::read_input() {
            Ok(Some(input)) => {
                if tx.blocking_send(Ok(input)).is_err() {
                    return;
                }
            }
            Ok(None) => {}
            Err(error) => {
                trace_client(format_args!("input_worker_error error={error}"));
                let _ = tx.blocking_send(Err(error));
                return;
            }
        }
    }
}

async fn attach_io_loop(
    writer: &mut (impl AsyncWrite + Unpin),
    mut inbound_rx: async_mpsc::Receiver<io::Result<Option<Message>>>,
    mut input_rx: async_mpsc::Receiver<io::Result<ConsoleInput>>,
    done: Arc<AtomicBool>,
    mut last_size: wmux_platform::TerminalSize,
    capabilities: TerminalCapabilities,
) -> io::Result<()> {
    let mut prefix = false;
    let mut resize_check = tokio::time::interval(Duration::from_millis(500));
    resize_check.set_missed_tick_behavior(MissedTickBehavior::Skip);

    while !done.load(Ordering::SeqCst) {
        tokio::select! {
            inbound = inbound_rx.recv() => {
                let inbound = inbound.ok_or_else(|| io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "terminal IPC reader stopped unexpectedly",
                ))??;
                match classify_attached_inbound(inbound) {
                    AttachedInbound::Output(bytes) => {
                        console::write_render_transaction(
                            &bytes,
                            capabilities.contains(TerminalCapabilities::SYNCHRONIZED_OUTPUT),
                        )?;
                    }
                    AttachedInbound::Clipboard(bytes) => {
                        let text = String::from_utf8_lossy(&bytes);
                        console::write_clipboard_text(&text)?;
                    }
                    AttachedInbound::CommandComplete => {}
                    AttachedInbound::CommandError(message) => {
                        trace_client(format_args!("attached_command_error message={message:?}"));
                    }
                    AttachedInbound::Exit => {
                        done.store(true, Ordering::SeqCst);
                        return Ok(());
                    }
                }
            }
            input = input_rx.recv() => {
                let input = input.ok_or_else(|| io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "terminal input worker stopped unexpectedly",
                ))??;
                match input {
                ConsoleInput::Key(bytes) => {
                    if handle_key(writer, &bytes, &mut prefix, &done).await? {
                        return Ok(());
                    }
                }
                ConsoleInput::Paste(text) => {
                    send_paste(writer, paste_bytes(&text)).await?;
                    prefix = false;
                }
                ConsoleInput::Mouse(event) => {
                    send_async_message(writer, Message::Mouse(event)).await?;
                    prefix = false;
                }
                ConsoleInput::Resize(size) => {
                    if size != last_size {
                        send_async_message(
                            &mut *writer,
                            Message::Resize {
                                cols: size.cols,
                                rows: size.rows,
                            },
                        ).await?;
                        last_size = size;
                    }
                }
                }
            }
            _ = resize_check.tick() => {
                if let Ok(size) = console::size() {
                    if size != last_size {
                    send_async_message(
                        &mut *writer,
                        Message::Resize {
                            cols: size.cols,
                            rows: size.rows,
                        },
                    ).await?;
                    last_size = size;
                    }
                }
            }
        }
    }
    Ok(())
}

async fn read_inbound_messages(
    mut reader: impl AsyncRead + Unpin,
    sender: async_mpsc::Sender<io::Result<Option<Message>>>,
) {
    loop {
        let result = read_async_message(&mut reader).await;
        let finished = !matches!(result, Ok(Some(_)));
        if sender.send(result).await.is_err() || finished {
            return;
        }
    }
}

#[derive(Debug, Eq, PartialEq)]
enum AttachedInbound {
    Output(Vec<u8>),
    Clipboard(Vec<u8>),
    CommandComplete,
    CommandError(String),
    Exit,
}

fn classify_attached_inbound(message: Option<Message>) -> AttachedInbound {
    match message {
        Some(Message::Output(bytes)) => AttachedInbound::Output(bytes),
        Some(Message::Clipboard(bytes)) => AttachedInbound::Clipboard(bytes),
        Some(Message::CommandOk(_)) => AttachedInbound::CommandComplete,
        Some(Message::CommandErr(message)) => AttachedInbound::CommandError(message),
        Some(Message::Shutdown) | None => AttachedInbound::Exit,
        Some(_) => AttachedInbound::CommandComplete,
    }
}

async fn handle_key(
    writer: &mut (impl AsyncWrite + Unpin),
    bytes: &[u8],
    prefix: &mut bool,
    done: &Arc<AtomicBool>,
) -> io::Result<bool> {
    if trace_enabled() {
        trace_client(format_args!(
            "key bytes={} hex={} text={:?}",
            bytes.len(),
            hex(bytes),
            text(bytes)
        ));
    }

    let mut pending = Vec::with_capacity(bytes.len() + 1);
    let mut index = 0;
    while index < bytes.len() {
        let byte = bytes[index];
        if *prefix {
            if byte == b'd' || byte == b'D' {
                send_async_message(writer, Message::Detach).await?;
                done.store(true, Ordering::SeqCst);
                return Ok(true);
            }
            if let Some((command, consumed)) = prefix_command_sequence(&bytes[index..]) {
                send_async_message(writer, Message::Command(command)).await?;
                *prefix = false;
                index += consumed;
                continue;
            }
            if byte == 0x02 {
                pending.push(0x02);
            } else {
                pending.push(0x02);
                pending.push(byte);
            }
            *prefix = false;
            index += 1;
        } else if byte == 0x02 {
            *prefix = true;
            index += 1;
        } else {
            pending.push(byte);
            index += 1;
        }
    }

    if !pending.is_empty() {
        send_async_message(writer, Message::Key(pending)).await?;
    }
    Ok(false)
}

fn prefix_command_sequence(bytes: &[u8]) -> Option<(String, usize)> {
    let first = *bytes.first()?;
    if first.is_ascii_digit() {
        return Some((format!("select-window -t {}", first as char), 1));
    }
    if bytes.starts_with(b"\x1b[A") {
        return Some(("select-pane -U".to_string(), 3));
    }
    if bytes.starts_with(b"\x1b[B") {
        return Some(("select-pane -D".to_string(), 3));
    }
    if bytes.starts_with(b"\x1b[C") {
        return Some(("select-pane -R".to_string(), 3));
    }
    if bytes.starts_with(b"\x1b[D") {
        return Some(("select-pane -L".to_string(), 3));
    }

    match first {
        b'%' => Some(("split-window -h".to_string(), 1)),
        b'"' => Some(("split-window -v".to_string(), 1)),
        b'c' | b'C' => Some(("new-window".to_string(), 1)),
        b'n' | b'N' => Some(("next-window".to_string(), 1)),
        b'p' | b'P' => Some(("previous-window".to_string(), 1)),
        b'l' | b'L' => Some(("last-window".to_string(), 1)),
        b'z' | b'Z' => Some(("resize-pane -Z".to_string(), 1)),
        b'[' => Some(("copy-mode".to_string(), 1)),
        b'x' | b'X' => Some(("kill-pane".to_string(), 1)),
        b'&' => Some(("kill-window".to_string(), 1)),
        b'o' | b'O' => Some(("select-pane -D".to_string(), 1)),
        b';' => Some(("last-pane".to_string(), 1)),
        _ => None,
    }
}

async fn send_paste(writer: &mut (impl AsyncWrite + Unpin), bytes: Vec<u8>) -> io::Result<()> {
    if trace_enabled() {
        trace_client(format_args!(
            "paste bytes={} hex={} text={:?}",
            bytes.len(),
            hex(&bytes),
            text(&bytes)
        ));
    }
    if bytes.is_empty() {
        return Ok(());
    }
    send_async_message(writer, Message::Paste(bytes)).await
}

async fn send_async_message(
    writer: &mut (impl AsyncWrite + Unpin),
    message: Message,
) -> io::Result<()> {
    let kind = message_kind(&message);
    trace_client(format_args!("send_message kind={kind}"));
    write_async_message(&mut *writer, message).await?;
    trace_client(format_args!("send_message_ok kind={kind}"));
    Ok(())
}

async fn read_async_message(reader: &mut (impl AsyncRead + Unpin)) -> io::Result<Option<Message>> {
    let mut header = [0_u8; FRAME_HEADER_LEN];
    match reader.read_exact(&mut header).await {
        Ok(_) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(error),
    }
    let (tag, payload_len) = decode_frame_header(&header)?;
    let mut payload = vec![0_u8; payload_len];
    reader.read_exact(&mut payload).await?;
    decode_frame_payload_owned(tag, payload).map(Some)
}

async fn write_async_message(
    writer: &mut (impl AsyncWrite + Unpin),
    message: Message,
) -> io::Result<()> {
    let frame = EncodedFrame::from_message(message);
    let mut header_offset = 0;
    let mut payload_offset = 0;
    while header_offset < frame.header().len() || payload_offset < frame.payload().len() {
        let written = if header_offset < frame.header().len() {
            writer
                .write_vectored(&[
                    IoSlice::new(&frame.header()[header_offset..]),
                    IoSlice::new(&frame.payload()[payload_offset..]),
                ])
                .await?
        } else {
            writer.write(&frame.payload()[payload_offset..]).await?
        };
        if written == 0 {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "failed to write IPC frame",
            ));
        }
        let header_remaining = frame.header().len() - header_offset;
        let header_written = written.min(header_remaining);
        header_offset += header_written;
        payload_offset += written - header_written;
    }
    Ok(())
}

fn message_kind(message: &Message) -> &'static str {
    match message {
        Message::Hello { .. } => "hello",
        Message::HelloOk { .. } => "hello-ok",
        Message::Command(_) => "command",
        Message::CommandOk(_) => "command-ok",
        Message::CommandErr(_) => "command-err",
        Message::Input(_) => "input",
        Message::Key(_) => "key",
        Message::Paste(_) => "paste",
        Message::Mouse(_) => "mouse",
        Message::Output(_) => "output",
        Message::Clipboard(_) => "clipboard",
        Message::Resize { .. } => "resize",
        Message::Detach => "detach",
        Message::Shutdown => "shutdown",
    }
}

fn paste_bytes(text: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(text.len());
    let mut chars = text.chars().peekable();
    while let Some(ch) = chars.next() {
        match ch {
            '\r' => {
                if chars.peek() == Some(&'\n') {
                    chars.next();
                }
                out.push(b'\r');
            }
            '\n' => out.push(b'\r'),
            _ => {
                let mut buffer = [0; 4];
                out.extend_from_slice(ch.encode_utf8(&mut buffer).as_bytes());
            }
        }
    }
    out
}

fn trace_client(args: std::fmt::Arguments<'_>) {
    if !trace_enabled() {
        return;
    }
    let path = std::env::temp_dir().join("wmux-clean-client.trace.log");
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(file, "{args}");
    }
}

fn trace_enabled() -> bool {
    std::env::var_os("WMUX_TRACE").is_some_and(|value| value == "1")
}

fn hex(bytes: &[u8]) -> String {
    bytes
        .iter()
        .take(256)
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn text(bytes: &[u8]) -> String {
    bytes
        .iter()
        .take(256)
        .flat_map(|byte| std::ascii::escape_default(*byte))
        .map(char::from)
        .collect()
}

async fn connect_for_invocation(
    invocation: &ServerInvocation,
    capabilities: TerminalCapabilities,
) -> io::Result<NamedPipeClient> {
    let endpoint = Endpoint::current_user()?;
    let endpoint_name = endpoint.pipe_name().to_string();
    connect_with_startup_policy(
        invocation.startup,
        &endpoint_name,
        || connect_async_handshake_once(&endpoint, capabilities),
        spawn_server,
        tokio::time::sleep,
    )
    .await
}

async fn connect_async_handshake_once(
    endpoint: &Endpoint,
    capabilities: TerminalCapabilities,
) -> io::Result<NamedPipeClient> {
    let mut pipe = connect_async(endpoint)?;
    write_async_message(
        &mut pipe,
        Message::Hello {
            version: VERSION,
            pid: process::id(),
            capabilities,
        },
    )
    .await?;
    let response = read_async_message(&mut pipe)
        .await
        .map_err(|error| handshake_read_error(endpoint.pipe_name(), error))?;
    match response {
        Some(Message::HelloOk { version, .. }) if version == VERSION => Ok(pipe),
        Some(Message::HelloOk { version, .. }) => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            protocol_error(version),
        )),
        Some(Message::CommandErr(message)) => Err(io::Error::other(message)),
        Some(other) => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("bad hello response: {other:?}"),
        )),
        None => Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "server closed during handshake",
        )),
    }
}

async fn connect_with_startup_policy<T, Connect, ConnectFuture, Spawn, Sleep, SleepFuture>(
    startup: StartupPolicy,
    endpoint_name: &str,
    mut connect: Connect,
    mut spawn: Spawn,
    mut sleep: Sleep,
) -> io::Result<T>
where
    Connect: FnMut() -> ConnectFuture,
    ConnectFuture: Future<Output = io::Result<T>>,
    Spawn: FnMut() -> io::Result<()>,
    Sleep: FnMut(Duration) -> SleepFuture,
    SleepFuture: Future<Output = ()>,
{
    let first_error = match connect().await {
        Ok(connection) => return Ok(connection),
        Err(error) => error,
    };
    match classify_connection_failure(&first_error) {
        ConnectionFailure::Terminal => return Err(first_error),
        ConnectionFailure::Absent if startup == StartupPolicy::RequireExisting => {
            return Err(no_server_error(endpoint_name));
        }
        ConnectionFailure::Retryable if startup == StartupPolicy::RequireExisting => {
            return Err(connection_error(endpoint_name, first_error));
        }
        ConnectionFailure::Absent => spawn().map_err(|error| {
            io::Error::new(
                error.kind(),
                format!("failed to start wmux server: {error}"),
            )
        })?,
        ConnectionFailure::Retryable => {}
    }

    let mut last_error = first_error;
    for delay in retry_delays() {
        sleep(delay).await;
        match connect().await {
            Ok(connection) => return Ok(connection),
            Err(error) if classify_connection_failure(&error) == ConnectionFailure::Terminal => {
                return Err(error);
            }
            Err(error) => last_error = error,
        }
    }

    if classify_connection_failure(&last_error) == ConnectionFailure::Absent {
        Err(no_server_error(endpoint_name))
    } else {
        Err(connection_error(endpoint_name, last_error))
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ConnectionFailure {
    Absent,
    Retryable,
    Terminal,
}

fn classify_connection_failure(error: &io::Error) -> ConnectionFailure {
    if error.kind() == io::ErrorKind::NotFound || matches!(error.raw_os_error(), Some(2 | 3)) {
        ConnectionFailure::Absent
    } else if matches!(
        error.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::ConnectionRefused
    ) || error.raw_os_error() == Some(ERROR_PIPE_BUSY_RAW)
    {
        ConnectionFailure::Retryable
    } else {
        ConnectionFailure::Terminal
    }
}

fn retry_delays() -> [Duration; 20] {
    [Duration::from_millis(50); 20]
}

fn no_server_message(endpoint_name: &str) -> String {
    format!("no wmux server running for this user ({endpoint_name})")
}

fn no_server_error(endpoint_name: &str) -> io::Error {
    io::Error::new(io::ErrorKind::NotFound, no_server_message(endpoint_name))
}

fn connection_error(endpoint_name: &str, error: io::Error) -> io::Error {
    io::Error::new(
        error.kind(),
        format!("could not connect to wmux server ({endpoint_name}): {error}"),
    )
}

fn protocol_error(server_version: u32) -> String {
    format!("wmux protocol mismatch: client protocol {VERSION}, server protocol {server_version}")
}

fn incompatible_server_message(endpoint_name: &str) -> String {
    format!("an incompatible wmux server owns endpoint ({endpoint_name}); stop it before retrying")
}

fn handshake_read_error(endpoint_name: &str, error: io::Error) -> io::Error {
    if error.kind() == io::ErrorKind::InvalidData && error.to_string() == "bad magic" {
        io::Error::new(
            io::ErrorKind::InvalidData,
            incompatible_server_message(endpoint_name),
        )
    } else {
        error
    }
}

fn terminal_capabilities() -> TerminalCapabilities {
    let mut bits = TerminalCapabilities::SCROLL_REGION;
    let known_sync_terminal = std::env::var_os("WT_SESSION").is_some()
        || std::env::var("TERM_PROGRAM").is_ok_and(|program| {
            matches!(
                program.to_ascii_lowercase().as_str(),
                "alacritty" | "wezterm" | "ghostty" | "iterm.app"
            )
        });
    let sync_override = std::env::var("WMUX_SYNCHRONIZED_OUTPUT").ok();
    if sync_override.as_deref() == Some("1")
        || (sync_override.as_deref() != Some("0") && known_sync_terminal)
    {
        bits |= TerminalCapabilities::SYNCHRONIZED_OUTPUT;
    }
    TerminalCapabilities::new(bits)
}

#[cfg(test)]
mod tests {
    use super::{
        classify_attached_inbound, connect_with_startup_policy, handshake_read_error,
        no_server_message, prefix_command_sequence, protocol_error, read_inbound_messages,
        retry_delays, write_async_message, AttachedInbound,
    };
    use std::{cell::Cell, future::ready, io};
    use wmux_cli::StartupPolicy;
    use wmux_protocol::Message;

    #[test]
    fn connection_diagnostics_name_the_endpoint_and_protocol_versions() {
        assert_eq!(
            no_server_message(r"\\.\pipe\wmux-S-1-5-21-x"),
            r"no wmux server running for this user (\\.\pipe\wmux-S-1-5-21-x)"
        );
        assert!(protocol_error(wmux_protocol::VERSION - 1).contains("client protocol 5"));
        assert!(protocol_error(wmux_protocol::VERSION - 1).contains("server protocol 4"));
        assert_eq!(retry_delays().len(), 20);

        let incompatible = handshake_read_error(
            r"\\.\pipe\wmux-test",
            io::Error::new(io::ErrorKind::InvalidData, "bad magic"),
        );
        assert!(incompatible
            .to_string()
            .contains("incompatible wmux server"));
        assert!(incompatible.to_string().contains("stop it"));
    }

    #[tokio::test]
    async fn connection_require_existing_attempts_once_without_spawning() {
        let attempts = Cell::new(0);
        let spawns = Cell::new(0);
        let error = connect_with_startup_policy(
            StartupPolicy::RequireExisting,
            r"\\.\pipe\wmux-test",
            || {
                attempts.set(attempts.get() + 1);
                ready(Err::<(), _>(io::Error::new(
                    io::ErrorKind::NotFound,
                    "missing",
                )))
            },
            || {
                spawns.set(spawns.get() + 1);
                Ok(())
            },
            |_| ready(()),
        )
        .await
        .unwrap_err();

        assert_eq!(attempts.get(), 1);
        assert_eq!(spawns.get(), 0);
        assert_eq!(
            error.to_string(),
            r"no wmux server running for this user (\\.\pipe\wmux-test)"
        );
    }

    #[tokio::test]
    async fn connection_start_if_missing_spawns_once_then_retries() {
        let attempts = Cell::new(0);
        let spawns = Cell::new(0);
        connect_with_startup_policy(
            StartupPolicy::StartIfMissing,
            r"\\.\pipe\wmux-test",
            || {
                attempts.set(attempts.get() + 1);
                ready(if attempts.get() == 1 {
                    Err(io::Error::new(io::ErrorKind::NotFound, "missing"))
                } else {
                    Ok(())
                })
            },
            || {
                spawns.set(spawns.get() + 1);
                Ok(())
            },
            |_| ready(()),
        )
        .await
        .unwrap();

        assert_eq!(attempts.get(), 2);
        assert_eq!(spawns.get(), 1);
    }

    #[tokio::test]
    async fn connection_security_and_protocol_failures_are_terminal() {
        let attempts = Cell::new(0);
        let spawns = Cell::new(0);
        let error = connect_with_startup_policy(
            StartupPolicy::StartIfMissing,
            r"\\.\pipe\wmux-test",
            || {
                attempts.set(attempts.get() + 1);
                ready(Err::<(), _>(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "endpoint owner mismatch",
                )))
            },
            || {
                spawns.set(spawns.get() + 1);
                Ok(())
            },
            |_| ready(()),
        )
        .await
        .unwrap_err();

        assert_eq!(error.kind(), io::ErrorKind::PermissionDenied);
        assert_eq!(attempts.get(), 1);
        assert_eq!(spawns.get(), 0);
    }

    #[test]
    fn prefix_numbers_select_windows_by_index() {
        assert_eq!(
            prefix_command_sequence(b"0"),
            Some(("select-window -t 0".to_string(), 1))
        );
        assert_eq!(
            prefix_command_sequence(b"9"),
            Some(("select-window -t 9".to_string(), 1))
        );
    }

    #[test]
    fn prefix_arrows_select_panes_by_direction() {
        assert_eq!(
            prefix_command_sequence(b"\x1b[A"),
            Some(("select-pane -U".to_string(), 3))
        );
        assert_eq!(
            prefix_command_sequence(b"\x1b[B"),
            Some(("select-pane -D".to_string(), 3))
        );
        assert_eq!(
            prefix_command_sequence(b"\x1b[C"),
            Some(("select-pane -R".to_string(), 3))
        );
        assert_eq!(
            prefix_command_sequence(b"\x1b[D"),
            Some(("select-pane -L".to_string(), 3))
        );
    }

    #[test]
    fn prefix_left_bracket_enters_tmux_copy_mode() {
        assert_eq!(
            prefix_command_sequence(b"["),
            Some(("copy-mode".to_string(), 1))
        );
    }

    #[test]
    fn attached_command_responses_do_not_end_the_client() {
        assert_eq!(
            classify_attached_inbound(Some(Message::CommandOk(String::new()))),
            AttachedInbound::CommandComplete
        );
        assert_eq!(
            classify_attached_inbound(Some(Message::CommandErr("no pane".to_string()))),
            AttachedInbound::CommandError("no pane".to_string())
        );
        assert_eq!(
            classify_attached_inbound(Some(Message::Shutdown)),
            AttachedInbound::Exit
        );
        assert_eq!(classify_attached_inbound(None), AttachedInbound::Exit);
    }

    #[tokio::test]
    async fn dedicated_reader_preserves_frames_during_sustained_input_pressure() {
        const FRAMES: usize = 20_000;
        let (mut writer, reader) = tokio::io::duplex(37);
        let (inbound_tx, mut inbound_rx) = tokio::sync::mpsc::channel(4);
        let reader_task = tokio::spawn(read_inbound_messages(reader, inbound_tx));
        let writer_task = tokio::spawn(async move {
            for sequence in 0..FRAMES {
                write_async_message(
                    &mut writer,
                    Message::Output((sequence as u64).to_le_bytes().to_vec()),
                )
                .await
                .unwrap();
            }
        });

        for expected in 0..FRAMES {
            for _ in 0..8 {
                tokio::task::yield_now().await;
            }
            let message = inbound_rx.recv().await.unwrap().unwrap().unwrap();
            assert_eq!(
                message,
                Message::Output((expected as u64).to_le_bytes().to_vec())
            );
        }

        writer_task.await.unwrap();
        reader_task.abort();
    }
}
