use std::{
    fs::{self, File, OpenOptions},
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
use wmux_cli::{ConfigAction, Invocation, StartupPolicy};
use wmux_config::{config_path, WmuxConfig};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, read_message, write_message, EncodedFrame,
    Message, TerminalCapabilities, FRAME_HEADER_LEN, VERSION,
};
use wmux_windows::{
    console,
    console::ConsoleInput,
    pipe::{connect, connect_async, is_running, Endpoint, NamedPipeClient},
};

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
        Invocation::Server(invocation) => {
            if invocation.startup == StartupPolicy::StartIfMissing {
                ensure_server()?;
            }
            if invocation.attached {
                RuntimeBuilder::new_current_thread()
                    .enable_all()
                    .build()?
                    .block_on(attached_command(invocation.argv.join(" ")))
            } else {
                send_command(invocation.argv.join(" "))
            }
        }
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

fn ensure_server() -> io::Result<()> {
    let endpoint = Endpoint::current_user();
    if is_running(&endpoint) {
        return Ok(());
    }
    spawn_server()?;
    thread::sleep(Duration::from_millis(200));
    Ok(())
}

fn spawn_server() -> io::Result<()> {
    let path = std::env::current_exe()?.with_file_name("wmux-server.exe");
    process::Command::new(path)
        .stdin(process::Stdio::null())
        .stdout(process::Stdio::from(log_file("wmux-clean-server.out.log")?))
        .stderr(process::Stdio::from(log_file("wmux-clean-server.err.log")?))
        .spawn()?;
    Ok(())
}

fn send_command(command: String) -> io::Result<()> {
    let mut pipe = connect_handshake()?;
    write_message(&mut pipe, &Message::Command(command))?;
    print_response(&mut pipe)
}

fn print_response(pipe: &mut File) -> io::Result<()> {
    match read_message(pipe)? {
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

async fn attached_command(command: String) -> io::Result<()> {
    let capabilities = terminal_capabilities();
    let pipe = connect_async_handshake(capabilities).await?;
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

fn connect_handshake() -> io::Result<File> {
    let endpoint = Endpoint::current_user();
    let mut pipe = connect_retry(&endpoint)?;
    write_message(
        &mut pipe,
        &Message::Hello {
            version: VERSION,
            pid: process::id(),
            capabilities: terminal_capabilities(),
        },
    )?;
    match read_message(&mut pipe)? {
        Some(Message::HelloOk { version, .. }) if version == VERSION => Ok(pipe),
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

async fn connect_async_handshake(
    capabilities: TerminalCapabilities,
) -> io::Result<NamedPipeClient> {
    let endpoint = Endpoint::current_user();
    let mut pipe = connect_async_retry(&endpoint).await?;
    write_async_message(
        &mut pipe,
        Message::Hello {
            version: VERSION,
            pid: process::id(),
            capabilities,
        },
    )
    .await?;
    match read_async_message(&mut pipe).await? {
        Some(Message::HelloOk { version, .. }) if version == VERSION => Ok(pipe),
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

async fn connect_async_retry(endpoint: &Endpoint) -> io::Result<NamedPipeClient> {
    let mut last = None;
    for _ in 0..20 {
        match connect_async(endpoint) {
            Ok(pipe) => return Ok(pipe),
            Err(error) => {
                last = Some(error);
                tokio::time::sleep(Duration::from_millis(50)).await;
            }
        }
    }
    Err(last.unwrap_or_else(|| io::Error::other("connect failed")))
}

fn connect_retry(endpoint: &Endpoint) -> io::Result<File> {
    let mut last = None;
    for _ in 0..20 {
        match connect(endpoint) {
            Ok(pipe) => return Ok(pipe),
            Err(error) => {
                last = Some(error);
                thread::sleep(Duration::from_millis(50));
            }
        }
    }
    Err(last.unwrap_or_else(|| io::Error::other("connect failed")))
}

fn log_file(name: &str) -> io::Result<File> {
    OpenOptions::new()
        .create(true)
        .append(true)
        .open(std::env::temp_dir().join(name))
}

#[cfg(test)]
mod tests {
    use super::{
        classify_attached_inbound, prefix_command_sequence, read_inbound_messages,
        write_async_message, AttachedInbound,
    };
    use wmux_protocol::Message;

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
