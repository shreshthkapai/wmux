mod presentation;

use std::{
    fs::{self, OpenOptions},
    future::Future,
    io::{self, BufRead, IoSlice, Write},
    path::Path,
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
use wmux_core::{quote_argument, ControlNotification, ControlRecord};
use wmux_platform::{
    BoxedIpcStream, ClientTransport, DaemonSpec, PlatformError, PlatformErrorKind, PlatformResult,
    TerminalBackend, TerminalInput, TerminalKeyCode, TerminalKeyEvent,
};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    WireKeyCode, WireKeyEvent, WireKeyModifiers, FRAME_HEADER_LEN, MAX_CLIENT_CWD_BYTES, VERSION,
};

use crate::presentation::{PresentationRequest, PresentationWorker};

pub fn run_with_platform(
    transport: Arc<dyn ClientTransport>,
    terminal: Arc<dyn TerminalBackend>,
) -> io::Result<()> {
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
        Invocation::Control => RuntimeBuilder::new_current_thread()
            .enable_all()
            .build()?
            .block_on(run_control_invocation(transport)),
        Invocation::Config(ConfigAction::Path) => show_config_path(),
        Invocation::Config(ConfigAction::Show) => show_config(),
        Invocation::Config(ConfigAction::Effective) => show_effective_config(),
        Invocation::Server(invocation) => RuntimeBuilder::new_current_thread()
            .enable_all()
            .build()?
            .block_on(run_server_invocation(invocation, transport, terminal)),
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
    print!("{}", format_effective_config(&config));
    Ok(())
}

fn format_effective_config(config: &WmuxConfig) -> String {
    let mut output = format!(
        "agent_compat = {}\nagent_ui = {:?}\n",
        config.agent_compat, config.agent_ui
    );
    for (key, value) in config.pane_environment(1) {
        output.push_str(&format!("pane.env.{key} = {value}\n"));
    }
    let ui = config.ui();
    output.push_str(&format!("ui.theme = {}\n", ui.preset_name()));
    output.push_str(&format!(
        "ui.theme_file = {}\n",
        ui.theme_file()
            .map(|path| path.display().to_string())
            .unwrap_or_default()
    ));
    output.push_str(&format!(
        "ui.theme_provider = {}\n",
        ui.theme_provider().unwrap_or_default()
    ));
    output.push_str(&format!(
        "ui.animation = {}\n",
        ui.animation_name().unwrap_or("theme-defined-or-off")
    ));
    if let Some(target) = ui.animation_target() {
        let target = match target {
            wmux_core::AnimationTarget::Borders => "border",
            wmux_core::AnimationTarget::Status => "status",
            wmux_core::AnimationTarget::Both => "both",
        };
        output.push_str(&format!("ui.animation_target = {target}\n"));
    }
    if let Some(fps) = ui.animation_fps() {
        output.push_str(&format!("ui.animation_fps = {fps}\n"));
    }
    if let Some(playback) = ui.animation_playback() {
        let playback = match playback {
            wmux_core::Playback::Once => "once",
            wmux_core::Playback::Loop => "loop",
        };
        output.push_str(&format!("ui.animation_playback = {playback}\n"));
    }
    output
}

fn server_spec() -> io::Result<DaemonSpec> {
    let executable = std::env::current_exe()?
        .with_file_name(format!("wmux-server{}", std::env::consts::EXE_SUFFIX));
    Ok(DaemonSpec {
        executable,
        arguments: Vec::new(),
        current_dir: std::env::current_dir()?,
    })
}

async fn run_server_invocation(
    invocation: ServerInvocation,
    transport: Arc<dyn ClientTransport>,
    terminal: Arc<dyn TerminalBackend>,
) -> io::Result<()> {
    let capabilities = terminal_capabilities();
    let pipe = connect_for_invocation(&invocation, capabilities, transport).await?;
    let command = encode_command_argv(&invocation.argv);
    if invocation.attached {
        attached_command(pipe, command, capabilities, terminal).await
    } else {
        send_command(pipe, command).await
    }
}

fn encode_command_argv(argv: &[String]) -> String {
    argv.iter()
        .map(|argument| quote_argument(argument))
        .collect::<Vec<_>>()
        .join(" ")
}

async fn send_command(mut pipe: BoxedIpcStream, command: String) -> io::Result<()> {
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

async fn run_control_invocation(transport: Arc<dyn ClientTransport>) -> io::Result<()> {
    let invocation = ServerInvocation {
        argv: Vec::new(),
        attached: false,
        startup: StartupPolicy::StartIfMissing,
    };
    let pipe =
        connect_for_invocation(&invocation, TerminalCapabilities::default(), transport).await?;
    let (input_tx, input_rx) = async_mpsc::channel(64);
    thread::Builder::new()
        .name("wmux-control-stdin".to_string())
        .spawn(move || {
            let stdin = io::stdin();
            for line in stdin.lock().lines() {
                if input_tx.blocking_send(line).is_err() {
                    break;
                }
            }
        })?;
    let stdout = io::stdout();
    control_io_loop(pipe, input_rx, &mut stdout.lock()).await
}

async fn control_io_loop(
    mut pipe: BoxedIpcStream,
    mut input: async_mpsc::Receiver<io::Result<String>>,
    output: &mut impl Write,
) -> io::Result<()> {
    write_async_message(&mut pipe, Message::EnterControl).await?;
    match read_async_message(&mut pipe).await? {
        Some(Message::ControlRecord(ControlRecord::Ready)) => {
            write_control_record(output, &ControlRecord::Ready)?;
        }
        Some(other) => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("unexpected control response: {other:?}"),
            ));
        }
        None => {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "server closed",
            ))
        }
    }

    let (mut reader, mut writer) = tokio::io::split(pipe);
    let mut sequence = 1_u64;
    let mut in_flight = 0_usize;
    let mut input_closed = false;
    loop {
        tokio::select! {
            inbound = read_async_message(&mut reader) => match inbound? {
                Some(Message::ControlRecord(record)) => {
                    if matches!(record, ControlRecord::End { .. } | ControlRecord::Error { .. }) {
                        in_flight = in_flight.saturating_sub(1);
                    }
                    write_control_record(output, &record)?;
                    if input_closed && in_flight == 0 { return Ok(()); }
                }
                Some(Message::Shutdown) | None => return Ok(()),
                Some(other) => return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("unexpected control message: {other:?}"),
                )),
            },
            line = input.recv(), if !input_closed => {
                let Some(line) = line else {
                    input_closed = true;
                    if in_flight == 0 { return Ok(()); }
                    continue;
                };
                let command = line?;
                write_async_message(
                    &mut writer,
                    Message::ControlCommand { sequence, command },
                ).await?;
                in_flight += 1;
                sequence = sequence.checked_add(1).ok_or_else(|| {
                    io::Error::other("control sequence space exhausted")
                })?;
            }
        }
    }
}

fn write_control_record(output: &mut impl Write, record: &ControlRecord) -> io::Result<()> {
    output.write_all(format_control_record(record).as_bytes())?;
    output.write_all(b"\n")?;
    output.flush()
}

fn format_control_record(record: &ControlRecord) -> String {
    match record {
        ControlRecord::Ready => "%ready".to_string(),
        ControlRecord::Begin { sequence } => format!("%begin {sequence}"),
        ControlRecord::Output { pane, bytes } => {
            format!("%output %{} {}", pane.raw(), escape_control_bytes(bytes))
        }
        ControlRecord::Notification(notification) => format_control_notification(notification),
        ControlRecord::End { sequence, output } if output.is_empty() => format!("%end {sequence}"),
        ControlRecord::End { sequence, output } => {
            format!(
                "%end {sequence} {}",
                escape_control_bytes(output.as_bytes())
            )
        }
        ControlRecord::Error { sequence, message } => {
            format!(
                "%error {sequence} {}",
                escape_control_bytes(message.as_bytes())
            )
        }
        ControlRecord::Pause { pane: Some(pane) } => format!("%pause %{}", pane.raw()),
        ControlRecord::Pause { pane: None } => "%pause".to_string(),
    }
}

fn format_control_notification(notification: &ControlNotification) -> String {
    match notification {
        ControlNotification::ClientAttached { client } => {
            format!("%notification client-attached @{}", client.raw())
        }
        ControlNotification::ClientDetached { client } => {
            format!("%notification client-detached @{}", client.raw())
        }
        ControlNotification::SessionCreated { session } => {
            format!("%notification session-created ${}", session.raw())
        }
        ControlNotification::SessionClosed { session } => {
            format!("%notification session-closed ${}", session.raw())
        }
        ControlNotification::WindowCreated { window } => {
            format!("%notification window-created @{}", window.raw())
        }
        ControlNotification::WindowClosed { window } => {
            format!("%notification window-closed @{}", window.raw())
        }
        ControlNotification::PaneCreated { pane } => {
            format!("%notification pane-created %{}", pane.raw())
        }
        ControlNotification::PaneClosed { pane } => {
            format!("%notification pane-closed %{}", pane.raw())
        }
        ControlNotification::BufferChanged { name } => {
            format_control_named_notification("buffer-changed", name.as_deref())
        }
        ControlNotification::BufferDeleted { name } => {
            format_control_named_notification("buffer-deleted", name.as_deref())
        }
        ControlNotification::JobFinished { job, exit_code } => match exit_code {
            Some(exit_code) => format!("%notification job-finished #{} {exit_code}", job.raw()),
            None => format!("%notification job-finished #{} unknown", job.raw()),
        },
    }
}

fn format_control_named_notification(event: &str, name: Option<&str>) -> String {
    match name {
        Some(name) => format!(
            "%notification {event} {}",
            escape_control_bytes(name.as_bytes())
        ),
        None => format!("%notification {event}"),
    }
}

fn escape_control_bytes(bytes: &[u8]) -> String {
    let mut escaped = String::with_capacity(bytes.len());
    for byte in bytes {
        if (0x20..=0x7e).contains(byte) && *byte != b'\\' {
            escaped.push(char::from(*byte));
        } else {
            escaped.push('\\');
            escaped.push(char::from(b'0' + ((byte >> 6) & 0x07)));
            escaped.push(char::from(b'0' + ((byte >> 3) & 0x07)));
            escaped.push(char::from(b'0' + (byte & 0x07)));
        }
    }
    escaped
}

async fn attached_command(
    pipe: BoxedIpcStream,
    command: String,
    capabilities: TerminalCapabilities,
    terminal: Arc<dyn TerminalBackend>,
) -> io::Result<()> {
    let _mode = terminal.enter().map_err(PlatformError::into_io)?;
    terminal
        .write_output(b"\x1b[?1049h\x1b[?2004h\x1b[>9u\x1b[?1002h\x1b[?1006h\x1b[?25h\x1b[H\x1b[2J")
        .map_err(PlatformError::into_io)?;

    let result = attached_inner(pipe, command, capabilities, Arc::clone(&terminal)).await;

    let _ = terminal
        .write_output(b"\x1b[0m\x1b[?1006l\x1b[?1002l\x1b[<u\x1b[?2004l\x1b[?25h\x1b[?1049l");
    result
}

async fn attached_inner(
    mut pipe: BoxedIpcStream,
    command: String,
    capabilities: TerminalCapabilities,
    terminal: Arc<dyn TerminalBackend>,
) -> io::Result<()> {
    let initial_size = terminal
        .size()
        .unwrap_or(wmux_platform::TerminalSize::new(80, 24));
    write_async_message(
        &mut pipe,
        Message::Resize {
            cols: initial_size.cols,
            rows: initial_size.rows,
        },
    )
    .await?;
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

    let done = Arc::new(AtomicBool::new(false));

    let (input_tx, input_rx) = async_mpsc::channel(1024);
    let input_done = Arc::clone(&done);
    let input_terminal = Arc::clone(&terminal);
    thread::spawn(move || {
        input_reader(input_terminal, input_tx, input_done);
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
        terminal,
    )
    .await;
    inbound_task.abort();
    result
}

fn input_reader(
    terminal: Arc<dyn TerminalBackend>,
    tx: async_mpsc::Sender<io::Result<TerminalInput>>,
    done: Arc<AtomicBool>,
) {
    while !done.load(Ordering::SeqCst) {
        match terminal.read_input() {
            Ok(Some(input)) => {
                if tx.blocking_send(Ok(input)).is_err() {
                    return;
                }
            }
            Ok(None) => {}
            Err(error) => {
                trace_client(format_args!("input_worker_error error={error}"));
                let _ = tx.blocking_send(Err(error.into_io()));
                return;
            }
        }
    }
}

struct InputStopGuard(Arc<AtomicBool>);

impl Drop for InputStopGuard {
    fn drop(&mut self) {
        self.0.store(true, Ordering::SeqCst);
    }
}

async fn attach_io_loop(
    writer: &mut (impl AsyncWrite + Unpin),
    mut inbound_rx: async_mpsc::Receiver<io::Result<Option<Message>>>,
    mut input_rx: async_mpsc::Receiver<io::Result<TerminalInput>>,
    done: Arc<AtomicBool>,
    mut last_size: wmux_platform::TerminalSize,
    capabilities: TerminalCapabilities,
    terminal: Arc<dyn TerminalBackend>,
) -> io::Result<()> {
    let _input_stop = InputStopGuard(Arc::clone(&done));
    let (presentation_tx, mut presentation_rx) = async_mpsc::channel(1);
    let presentation = PresentationWorker::spawn(Arc::clone(&terminal), presentation_tx)?;
    let mut presentation_in_flight = None;
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
                    AttachedInbound::Output { sequence, bytes } => {
                        if presentation_in_flight.is_some() {
                            return Err(io::Error::new(
                                io::ErrorKind::InvalidData,
                                "server sent a render before the previous presentation completed",
                            ));
                        }
                        presentation.present(PresentationRequest {
                            sequence,
                            bytes,
                            synchronized_output: capabilities
                                .contains(TerminalCapabilities::SYNCHRONIZED_OUTPUT),
                        })?;
                        presentation_in_flight = Some(sequence);
                    }
                    AttachedInbound::Clipboard(bytes) => {
                        let text = String::from_utf8_lossy(&bytes);
                        terminal.write_clipboard_text(&text).map_err(PlatformError::into_io)?;
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
            completion = presentation_rx.recv() => {
                let completion = completion.ok_or_else(|| io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "terminal presentation worker stopped unexpectedly",
                ))?;
                if presentation_in_flight != Some(completion.sequence) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "terminal presentation completion sequence mismatch",
                    ));
                }
                completion.result.map_err(PlatformError::into_io)?;
                presentation_in_flight = None;
                send_async_message(
                    &mut *writer,
                    Message::OutputAck {
                        sequence: completion.sequence,
                    },
                ).await?;
            }
            input = input_rx.recv() => {
                let input = input.ok_or_else(|| io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "terminal input worker stopped unexpectedly",
                ))??;
                match input {
                TerminalInput::Key(event) => {
                    send_key(writer, wire_key_event(event)).await?;
                }
                TerminalInput::Paste(text) => {
                    send_paste(writer, paste_bytes(&text)).await?;
                }
                TerminalInput::Mouse(event) => {
                    send_async_message(writer, Message::Mouse(event)).await?;
                }
                TerminalInput::Resize(size) => {
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
                if let Ok(size) = terminal.size() {
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

fn wire_key_event(event: TerminalKeyEvent) -> WireKeyEvent {
    let code = match event.code {
        TerminalKeyCode::Char(value) => WireKeyCode::Char(value),
        TerminalKeyCode::Left => WireKeyCode::Left,
        TerminalKeyCode::Right => WireKeyCode::Right,
        TerminalKeyCode::Up => WireKeyCode::Up,
        TerminalKeyCode::Down => WireKeyCode::Down,
        TerminalKeyCode::Home => WireKeyCode::Home,
        TerminalKeyCode::End => WireKeyCode::End,
        TerminalKeyCode::PageUp => WireKeyCode::PageUp,
        TerminalKeyCode::PageDown => WireKeyCode::PageDown,
        TerminalKeyCode::Backspace => WireKeyCode::Backspace,
        TerminalKeyCode::Delete => WireKeyCode::Delete,
        TerminalKeyCode::Insert => WireKeyCode::Insert,
        TerminalKeyCode::Enter => WireKeyCode::Enter,
        TerminalKeyCode::Tab => WireKeyCode::Tab,
        TerminalKeyCode::BackTab => WireKeyCode::BackTab,
        TerminalKeyCode::Escape => WireKeyCode::Escape,
        TerminalKeyCode::Function(value) => WireKeyCode::Function(value),
    };
    WireKeyEvent {
        code,
        modifiers: WireKeyModifiers::from_bits(event.modifiers.bits())
            .expect("platform and protocol key modifier masks must match"),
        raw: event.raw,
    }
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
    Output { sequence: u64, bytes: Vec<u8> },
    Clipboard(Vec<u8>),
    CommandComplete,
    CommandError(String),
    Exit,
}

fn classify_attached_inbound(message: Option<Message>) -> AttachedInbound {
    match message {
        Some(Message::Output { sequence, bytes }) => AttachedInbound::Output { sequence, bytes },
        Some(Message::Clipboard(bytes)) => AttachedInbound::Clipboard(bytes),
        Some(Message::CommandOk(_)) => AttachedInbound::CommandComplete,
        Some(Message::CommandErr(message)) => AttachedInbound::CommandError(message),
        Some(Message::Shutdown) | None => AttachedInbound::Exit,
        Some(_) => AttachedInbound::CommandComplete,
    }
}

async fn send_key(writer: &mut (impl AsyncWrite + Unpin), event: WireKeyEvent) -> io::Result<()> {
    if trace_enabled() {
        trace_client(format_args!(
            "key bytes={} hex={} text={:?}",
            event.raw.len(),
            hex(&event.raw),
            text(&event.raw)
        ));
    }
    send_async_message(writer, Message::Key(event)).await
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
        Message::Output { .. } => "output",
        Message::OutputAck { .. } => "output-ack",
        Message::Clipboard(_) => "clipboard",
        Message::EnterControl => "enter-control",
        Message::ControlCommand { .. } => "control-command",
        Message::ControlRecord(_) => "control-record",
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
    let path = std::env::temp_dir().join("wmux-client.trace.log");
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
    transport: Arc<dyn ClientTransport>,
) -> io::Result<BoxedIpcStream> {
    let endpoint_name = transport.endpoint().display().to_string();
    let current_dir = client_current_dir().map_err(PlatformError::into_io)?;
    connect_with_startup_policy(
        invocation.startup,
        &endpoint_name,
        || connect_async_handshake_once(transport.as_ref(), capabilities, current_dir.clone()),
        || {
            let spec = server_spec()
                .map_err(|error| PlatformError::from_io("build server daemon spec", error))?;
            transport.spawn_server(&spec)
        },
        tokio::time::sleep,
    )
    .await
    .map_err(PlatformError::into_io)
}

async fn connect_async_handshake_once(
    transport: &dyn ClientTransport,
    capabilities: TerminalCapabilities,
    current_dir: String,
) -> PlatformResult<BoxedIpcStream> {
    let mut pipe = transport.connect().await?;
    write_async_message(
        &mut pipe,
        Message::Hello {
            version: VERSION,
            pid: process::id(),
            capabilities,
            current_dir,
        },
    )
    .await
    .map_err(|error| PlatformError::from_io("write client handshake", error))?;
    let response = read_async_message(&mut pipe)
        .await
        .map_err(|error| handshake_read_error(transport.endpoint().display(), error))?;
    match response {
        Some(Message::HelloOk { version, .. }) if version == VERSION => Ok(pipe),
        Some(Message::HelloOk { version, .. }) => Err(PlatformError::new(
            PlatformErrorKind::InvalidData,
            "validate server handshake",
            protocol_error(version),
        )),
        Some(Message::CommandErr(message)) => Err(PlatformError::new(
            PlatformErrorKind::InvalidData,
            "validate server handshake",
            message,
        )),
        Some(other) => Err(PlatformError::new(
            PlatformErrorKind::InvalidData,
            "validate server handshake",
            format!("bad hello response: {other:?}"),
        )),
        None => Err(PlatformError::new(
            PlatformErrorKind::Disconnected,
            "read server handshake",
            "server closed during handshake",
        )),
    }
}

fn client_current_dir() -> PlatformResult<String> {
    let current_dir = std::env::current_dir()
        .map_err(|error| PlatformError::from_io("read client working directory", error))?;
    client_current_dir_text(&current_dir)
}

fn client_current_dir_text(current_dir: &Path) -> PlatformResult<String> {
    if !current_dir.is_absolute() {
        return Err(PlatformError::new(
            PlatformErrorKind::InvalidInput,
            "encode client working directory",
            "working directory is not absolute",
        ));
    }
    let current_dir = current_dir.to_str().ok_or_else(|| {
        PlatformError::new(
            PlatformErrorKind::InvalidInput,
            "encode client working directory",
            "working directory is not valid UTF-8",
        )
    })?;
    if current_dir.len() > MAX_CLIENT_CWD_BYTES {
        return Err(PlatformError::new(
            PlatformErrorKind::InvalidInput,
            "encode client working directory",
            format!("working directory exceeds {MAX_CLIENT_CWD_BYTES} bytes"),
        ));
    }
    Ok(current_dir.to_string())
}

async fn connect_with_startup_policy<T, Connect, ConnectFuture, Spawn, Sleep, SleepFuture>(
    startup: StartupPolicy,
    endpoint_name: &str,
    mut connect: Connect,
    mut spawn: Spawn,
    mut sleep: Sleep,
) -> PlatformResult<T>
where
    Connect: FnMut() -> ConnectFuture,
    ConnectFuture: Future<Output = PlatformResult<T>>,
    Spawn: FnMut() -> PlatformResult<()>,
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
            PlatformError::new(error.kind(), "start wmux server", error.to_string())
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

fn classify_connection_failure(error: &PlatformError) -> ConnectionFailure {
    match error.kind() {
        PlatformErrorKind::NotFound => ConnectionFailure::Absent,
        PlatformErrorKind::Busy | PlatformErrorKind::TimedOut => ConnectionFailure::Retryable,
        _ => ConnectionFailure::Terminal,
    }
}

fn retry_delays() -> [Duration; 20] {
    [Duration::from_millis(50); 20]
}

fn no_server_message(endpoint_name: &str) -> String {
    format!("no wmux server running for this user ({endpoint_name})")
}

fn no_server_error(endpoint_name: &str) -> PlatformError {
    PlatformError::new(
        PlatformErrorKind::NotFound,
        "connect IPC client",
        no_server_message(endpoint_name),
    )
}

fn connection_error(endpoint_name: &str, error: PlatformError) -> PlatformError {
    PlatformError::new(
        error.kind(),
        "connect IPC client",
        format!("could not connect to wmux server ({endpoint_name}): {error}"),
    )
}

fn protocol_error(server_version: u32) -> String {
    format!("wmux protocol mismatch: client protocol {VERSION}, server protocol {server_version}")
}

fn incompatible_server_message(endpoint_name: &str) -> String {
    format!("an incompatible wmux server owns endpoint ({endpoint_name}); stop it before retrying")
}

fn handshake_read_error(endpoint_name: &str, error: io::Error) -> PlatformError {
    if error.kind() == io::ErrorKind::InvalidData && error.to_string() == "bad magic" {
        PlatformError::new(
            PlatformErrorKind::InvalidData,
            "read server handshake",
            incompatible_server_message(endpoint_name),
        )
    } else {
        PlatformError::from_io("read server handshake", error)
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
        attach_io_loop, attached_command, attached_inner, classify_attached_inbound,
        client_current_dir_text, connect_with_startup_policy, control_io_loop, encode_command_argv,
        escape_control_bytes, format_control_record, format_effective_config, handshake_read_error,
        no_server_message, paste_bytes, protocol_error, read_async_message, read_inbound_messages,
        retry_delays, send_key, wire_key_event, write_async_message, AttachedInbound,
    };
    use std::{
        cell::Cell,
        future::ready,
        io,
        sync::{
            atomic::{AtomicBool, AtomicUsize, Ordering},
            Arc, Condvar, Mutex,
        },
        time::Duration,
    };
    use wmux_cli::StartupPolicy;
    use wmux_core::{parse_command_text, Command, ControlRecord, PaneId};
    use wmux_platform::{
        PlatformError, PlatformErrorKind, PlatformResult, TerminalBackend, TerminalInput,
        TerminalKeyCode, TerminalKeyEvent, TerminalKeyModifiers, TerminalModeGuard, TerminalSize,
    };
    use wmux_protocol::{
        Message, TerminalCapabilities, WireKeyCode, WireKeyEvent, WireKeyModifiers,
    };

    struct NoopTerminal;

    #[test]
    fn pasted_leading_ampersand_is_preserved_while_newlines_are_normalized() {
        assert_eq!(paste_bytes("& run\r\nnext\n"), b"& run\rnext\r");
    }

    #[test]
    fn client_working_directory_must_be_absolute() {
        let current_dir = std::env::current_dir().unwrap();
        assert_eq!(
            client_current_dir_text(&current_dir).unwrap(),
            current_dir.to_str().unwrap()
        );
        assert_eq!(
            client_current_dir_text(std::path::Path::new("relative/project"))
                .unwrap_err()
                .kind(),
            PlatformErrorKind::InvalidInput
        );
    }

    #[test]
    fn command_argv_transport_preserves_every_argument_byte() {
        for name in [
            "",
            "two words",
            "semi;colon",
            "#literal",
            "single'quote",
            "double\"quote",
            r"C:\project directory\wmux",
            "line\nbreak",
            "tab\tvalue",
            "lambda-λ",
        ] {
            let wire = encode_command_argv(&["rename-window".to_string(), name.to_string()]);
            let parsed = parse_command_text(&wire).expect("encoded argv remains valid");

            assert!(
                matches!(&parsed[0], Command::RenameWindow { name: actual, .. } if actual == name),
                "argument {name:?} changed in transit as {wire:?}"
            );
        }
    }

    #[test]
    fn effective_config_reports_theme_sources_without_resolving_provider_output() {
        let config = wmux_config::parse_config(
            "ui.theme = neon\nui.theme_provider = trusted-theme\nui.animation = pulse\n",
        )
        .unwrap();

        let output = format_effective_config(&config);

        assert!(output.contains("ui.theme = neon\n"));
        assert!(output.contains("ui.theme_provider = trusted-theme\n"));
        assert!(output.contains("ui.animation = pulse\n"));
        assert!(!output.contains("schema"));
    }

    impl TerminalBackend for NoopTerminal {
        fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
            unreachable!("attach I/O loop does not enter terminal mode")
        }

        fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
            Ok(None)
        }

        fn write_output(&self, _bytes: &[u8]) -> PlatformResult<()> {
            Ok(())
        }

        fn write_render_transaction(
            &self,
            _bytes: &[u8],
            _synchronized_output: bool,
        ) -> PlatformResult<()> {
            Ok(())
        }

        fn write_clipboard_text(&self, _text: &str) -> PlatformResult<()> {
            Ok(())
        }

        fn size(&self) -> PlatformResult<TerminalSize> {
            Ok(TerminalSize::new(80, 24))
        }
    }

    struct CountedGuard(Arc<AtomicUsize>);

    impl Drop for CountedGuard {
        fn drop(&mut self) {
            self.0.fetch_add(1, Ordering::SeqCst);
        }
    }

    struct FailingOutputTerminal {
        guard_drops: Arc<AtomicUsize>,
    }

    struct RecordingTerminal {
        writes: Arc<Mutex<Vec<Vec<u8>>>>,
        size: TerminalSize,
    }

    struct FailingRenderTerminal;

    struct BlockingRenderTerminal {
        started: Arc<AtomicBool>,
        release: Arc<(Mutex<bool>, Condvar)>,
    }

    impl TerminalBackend for RecordingTerminal {
        fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
            Ok(Box::new(()))
        }

        fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
            Ok(None)
        }

        fn write_output(&self, bytes: &[u8]) -> PlatformResult<()> {
            self.writes.lock().unwrap().push(bytes.to_vec());
            Ok(())
        }

        fn write_render_transaction(
            &self,
            bytes: &[u8],
            _synchronized_output: bool,
        ) -> PlatformResult<()> {
            self.writes.lock().unwrap().push(bytes.to_vec());
            Ok(())
        }

        fn write_clipboard_text(&self, _text: &str) -> PlatformResult<()> {
            Ok(())
        }

        fn size(&self) -> PlatformResult<TerminalSize> {
            Ok(self.size)
        }
    }

    impl TerminalBackend for FailingRenderTerminal {
        fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
            Ok(Box::new(()))
        }

        fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
            Ok(None)
        }

        fn write_output(&self, _bytes: &[u8]) -> PlatformResult<()> {
            Ok(())
        }

        fn write_render_transaction(
            &self,
            _bytes: &[u8],
            _synchronized_output: bool,
        ) -> PlatformResult<()> {
            Err(PlatformError::new(
                PlatformErrorKind::Disconnected,
                "present test frame",
                "scripted render failure",
            ))
        }

        fn write_clipboard_text(&self, _text: &str) -> PlatformResult<()> {
            Ok(())
        }

        fn size(&self) -> PlatformResult<TerminalSize> {
            Ok(TerminalSize::new(80, 24))
        }
    }

    impl TerminalBackend for BlockingRenderTerminal {
        fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
            Ok(Box::new(()))
        }

        fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
            Ok(None)
        }

        fn write_output(&self, _bytes: &[u8]) -> PlatformResult<()> {
            Ok(())
        }

        fn write_render_transaction(
            &self,
            _bytes: &[u8],
            _synchronized_output: bool,
        ) -> PlatformResult<()> {
            self.started.store(true, Ordering::SeqCst);
            let (lock, ready) = &*self.release;
            let mut released = lock.lock().unwrap();
            while !*released {
                released = ready.wait(released).unwrap();
            }
            Ok(())
        }

        fn write_clipboard_text(&self, _text: &str) -> PlatformResult<()> {
            Ok(())
        }

        fn size(&self) -> PlatformResult<TerminalSize> {
            Ok(TerminalSize::new(80, 24))
        }
    }

    impl TerminalBackend for FailingOutputTerminal {
        fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
            Ok(Box::new(CountedGuard(Arc::clone(&self.guard_drops))))
        }

        fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
            Ok(None)
        }

        fn write_output(&self, _bytes: &[u8]) -> PlatformResult<()> {
            Err(PlatformError::new(
                PlatformErrorKind::Disconnected,
                "write test terminal",
                "scripted terminal closure",
            ))
        }

        fn write_render_transaction(
            &self,
            _bytes: &[u8],
            _synchronized_output: bool,
        ) -> PlatformResult<()> {
            Ok(())
        }

        fn write_clipboard_text(&self, _text: &str) -> PlatformResult<()> {
            Ok(())
        }

        fn size(&self) -> PlatformResult<TerminalSize> {
            Ok(TerminalSize::new(80, 24))
        }
    }

    #[test]
    fn platform_terminal_key_converts_exactly_to_protocol() {
        let event = wmux_platform::TerminalKeyEvent {
            code: wmux_platform::TerminalKeyCode::Char('λ'),
            modifiers: wmux_platform::TerminalKeyModifiers::new(
                wmux_platform::TerminalKeyModifiers::ALT,
            ),
            raw: vec![0x1b, 0xce, 0xbb],
        };

        assert_eq!(
            wire_key_event(event),
            WireKeyEvent {
                code: WireKeyCode::Char('λ'),
                modifiers: WireKeyModifiers::ALT,
                raw: vec![0x1b, 0xce, 0xbb],
            }
        );
    }

    #[test]
    fn connection_diagnostics_name_the_endpoint_and_protocol_versions() {
        assert_eq!(
            no_server_message(r"\\.\pipe\wmux-S-1-5-21-x"),
            r"no wmux server running for this user (\\.\pipe\wmux-S-1-5-21-x)"
        );
        assert!(protocol_error(wmux_protocol::VERSION - 1)
            .contains(&format!("client protocol {}", wmux_protocol::VERSION)));
        assert!(protocol_error(wmux_protocol::VERSION - 1)
            .contains(&format!("server protocol {}", wmux_protocol::VERSION - 1)));
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
                ready(Err::<(), _>(PlatformError::new(
                    PlatformErrorKind::NotFound,
                    "connect test endpoint",
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
        assert!(error
            .to_string()
            .contains(r"no wmux server running for this user (\\.\pipe\wmux-test)"));
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
                    Err(PlatformError::new(
                        PlatformErrorKind::NotFound,
                        "connect test endpoint",
                        "missing",
                    ))
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
                ready(Err::<(), _>(PlatformError::new(
                    PlatformErrorKind::PermissionDenied,
                    "connect test endpoint",
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

        assert_eq!(error.kind(), PlatformErrorKind::PermissionDenied);
        assert_eq!(attempts.get(), 1);
        assert_eq!(spawns.get(), 0);
    }

    #[tokio::test]
    async fn attached_client_forwards_semantic_keys_without_binding_policy() {
        let expected = WireKeyEvent {
            code: WireKeyCode::Char('b'),
            modifiers: WireKeyModifiers::CONTROL,
            raw: vec![0x02],
        };
        let (mut client, mut server) = tokio::io::duplex(64);

        send_key(&mut client, expected.clone()).await.unwrap();

        assert_eq!(
            read_async_message(&mut server).await.unwrap(),
            Some(Message::Key(expected))
        );
    }

    #[tokio::test]
    async fn attached_client_sends_physical_size_before_attach_command() {
        let expected = TerminalSize::new(137, 43);
        let (client, mut server) = tokio::io::duplex(256);
        let terminal = Arc::new(RecordingTerminal {
            writes: Arc::new(Mutex::new(Vec::new())),
            size: expected,
        });

        let client_task = tokio::spawn(attached_inner(
            Box::new(client),
            "attach-session".to_string(),
            TerminalCapabilities::default(),
            terminal,
        ));
        let first = read_async_message(&mut server).await.unwrap();
        drop(server);
        let _ = client_task.await.unwrap();

        assert_eq!(
            first,
            Some(Message::Resize {
                cols: expected.cols,
                rows: expected.rows,
            })
        );
    }

    #[tokio::test]
    async fn attach_io_error_signals_the_terminal_input_thread_to_stop() {
        let (mut writer, _reader) = tokio::io::duplex(64);
        let (inbound_tx, inbound_rx) = tokio::sync::mpsc::channel(1);
        inbound_tx
            .send(Err(io::Error::other("scripted IPC read failure")))
            .await
            .unwrap();
        let (_input_tx, input_rx) = tokio::sync::mpsc::channel(1);
        let done = Arc::new(AtomicBool::new(false));

        let error = attach_io_loop(
            &mut writer,
            inbound_rx,
            input_rx,
            Arc::clone(&done),
            TerminalSize::new(80, 24),
            TerminalCapabilities::default(),
            Arc::new(NoopTerminal),
        )
        .await
        .unwrap_err();

        assert_eq!(error.kind(), io::ErrorKind::Other);
        assert!(done.load(Ordering::SeqCst));
    }

    #[tokio::test]
    async fn presentation_acknowledgement_follows_the_terminal_write() {
        let (client, mut server) = tokio::io::duplex(512);
        let (reader, mut writer) = tokio::io::split(client);
        drop(reader);
        let (inbound_tx, inbound_rx) = tokio::sync::mpsc::channel(2);
        let (input_tx_guard, input_rx) = tokio::sync::mpsc::channel(1);
        let writes = Arc::new(Mutex::new(Vec::new()));
        let terminal = Arc::new(RecordingTerminal {
            writes: Arc::clone(&writes),
            size: TerminalSize::new(80, 24),
        });
        let done = Arc::new(AtomicBool::new(false));

        inbound_tx
            .send(Ok(Some(Message::Output {
                sequence: 42,
                bytes: b"frame".to_vec(),
            })))
            .await
            .unwrap();
        let attach = tokio::spawn(async move {
            attach_io_loop(
                &mut writer,
                inbound_rx,
                input_rx,
                done,
                TerminalSize::new(80, 24),
                TerminalCapabilities::default(),
                terminal,
            )
            .await
        });
        let acknowledgement =
            tokio::time::timeout(Duration::from_secs(1), read_async_message(&mut server))
                .await
                .unwrap()
                .unwrap();

        assert_eq!(acknowledgement, Some(Message::OutputAck { sequence: 42 }));
        assert_eq!(writes.lock().unwrap().as_slice(), [b"frame".to_vec()]);
        inbound_tx.send(Ok(Some(Message::Shutdown))).await.unwrap();
        assert!(attach.await.unwrap().is_ok());
        drop(input_tx_guard);
    }

    #[tokio::test]
    async fn failed_terminal_presentation_sends_no_acknowledgement() {
        let (client, mut server) = tokio::io::duplex(512);
        let (reader, mut writer) = tokio::io::split(client);
        drop(reader);
        let (inbound_tx, inbound_rx) = tokio::sync::mpsc::channel(1);
        let (input_tx_guard, input_rx) = tokio::sync::mpsc::channel(1);

        inbound_tx
            .send(Ok(Some(Message::Output {
                sequence: 9,
                bytes: b"frame".to_vec(),
            })))
            .await
            .unwrap();
        let attach = tokio::spawn(async move {
            attach_io_loop(
                &mut writer,
                inbound_rx,
                input_rx,
                Arc::new(AtomicBool::new(false)),
                TerminalSize::new(80, 24),
                TerminalCapabilities::default(),
                Arc::new(FailingRenderTerminal),
            )
            .await
        });

        let error = attach.await.unwrap().unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
        assert_eq!(read_async_message(&mut server).await.unwrap(), None);
        drop(input_tx_guard);
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn terminal_presentation_does_not_block_key_or_paste_input() {
        let (client, mut server) = tokio::io::duplex(1024);
        let (reader, mut writer) = tokio::io::split(client);
        drop(reader);
        let (inbound_tx, inbound_rx) = tokio::sync::mpsc::channel(2);
        let (input_tx, input_rx) = tokio::sync::mpsc::channel(2);
        let started = Arc::new(AtomicBool::new(false));
        let release = Arc::new((Mutex::new(false), Condvar::new()));
        let terminal = Arc::new(BlockingRenderTerminal {
            started: Arc::clone(&started),
            release: Arc::clone(&release),
        });

        inbound_tx
            .send(Ok(Some(Message::Output {
                sequence: 11,
                bytes: b"slow frame".to_vec(),
            })))
            .await
            .unwrap();
        let attach = tokio::spawn(async move {
            attach_io_loop(
                &mut writer,
                inbound_rx,
                input_rx,
                Arc::new(AtomicBool::new(false)),
                TerminalSize::new(80, 24),
                TerminalCapabilities::default(),
                terminal,
            )
            .await
        });

        tokio::time::timeout(Duration::from_secs(1), async {
            while !started.load(Ordering::SeqCst) {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        input_tx
            .send(Ok(TerminalInput::Key(TerminalKeyEvent {
                code: TerminalKeyCode::Char('a'),
                modifiers: TerminalKeyModifiers::default(),
                raw: b"a".to_vec(),
            })))
            .await
            .unwrap();
        input_tx
            .send(Ok(TerminalInput::Paste("& run".to_string())))
            .await
            .unwrap();

        assert_eq!(
            tokio::time::timeout(Duration::from_secs(1), read_async_message(&mut server))
                .await
                .unwrap()
                .unwrap(),
            Some(Message::Key(WireKeyEvent {
                code: WireKeyCode::Char('a'),
                modifiers: WireKeyModifiers::NONE,
                raw: b"a".to_vec(),
            }))
        );
        assert_eq!(
            tokio::time::timeout(Duration::from_secs(1), read_async_message(&mut server))
                .await
                .unwrap()
                .unwrap(),
            Some(Message::Paste(b"& run".to_vec()))
        );

        let (lock, ready) = &*release;
        *lock.lock().unwrap() = true;
        ready.notify_one();
        assert_eq!(
            tokio::time::timeout(Duration::from_secs(1), read_async_message(&mut server))
                .await
                .unwrap()
                .unwrap(),
            Some(Message::OutputAck { sequence: 11 })
        );
        inbound_tx.send(Ok(Some(Message::Shutdown))).await.unwrap();
        assert!(attach.await.unwrap().is_ok());
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn a_second_render_before_completion_is_a_protocol_error() {
        let (client, mut server) = tokio::io::duplex(1024);
        let (reader, mut writer) = tokio::io::split(client);
        drop(reader);
        let (inbound_tx, inbound_rx) = tokio::sync::mpsc::channel(1);
        let (input_tx_guard, input_rx) = tokio::sync::mpsc::channel(1);
        let started = Arc::new(AtomicBool::new(false));
        let release = Arc::new((Mutex::new(false), Condvar::new()));
        let terminal = Arc::new(BlockingRenderTerminal {
            started: Arc::clone(&started),
            release: Arc::clone(&release),
        });

        inbound_tx
            .send(Ok(Some(Message::Output {
                sequence: 1,
                bytes: b"first".to_vec(),
            })))
            .await
            .unwrap();
        let attach = tokio::spawn(async move {
            attach_io_loop(
                &mut writer,
                inbound_rx,
                input_rx,
                Arc::new(AtomicBool::new(false)),
                TerminalSize::new(80, 24),
                TerminalCapabilities::default(),
                terminal,
            )
            .await
        });
        tokio::time::timeout(Duration::from_secs(1), async {
            while !started.load(Ordering::SeqCst) {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();

        inbound_tx
            .send(Ok(Some(Message::Output {
                sequence: 2,
                bytes: b"second".to_vec(),
            })))
            .await
            .unwrap();
        tokio::time::timeout(
            Duration::from_secs(1),
            inbound_tx.send(Ok(Some(Message::Shutdown))),
        )
        .await
        .unwrap()
        .unwrap();

        let (lock, ready) = &*release;
        *lock.lock().unwrap() = true;
        ready.notify_one();
        let error = attach.await.unwrap().unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
        assert_eq!(read_async_message(&mut server).await.unwrap(), None);
        drop(input_tx_guard);
    }

    #[tokio::test]
    async fn attached_terminal_error_drops_the_mode_guard_exactly_once() {
        let (client, _server) = tokio::io::duplex(64);
        let guard_drops = Arc::new(AtomicUsize::new(0));
        let terminal = Arc::new(FailingOutputTerminal {
            guard_drops: Arc::clone(&guard_drops),
        });

        let error = attached_command(
            Box::new(client),
            "attach-session".to_string(),
            TerminalCapabilities::default(),
            terminal,
        )
        .await
        .unwrap_err();

        assert_eq!(error.kind(), io::ErrorKind::BrokenPipe);
        assert_eq!(guard_drops.load(Ordering::SeqCst), 1);
    }

    #[tokio::test]
    async fn attached_terminal_preserves_modified_keys_and_avoids_pointer_motion_floods() {
        let (client, mut server) = tokio::io::duplex(256);
        let writes = Arc::new(Mutex::new(Vec::new()));
        let terminal = Arc::new(RecordingTerminal {
            writes: Arc::clone(&writes),
            size: TerminalSize::new(80, 24),
        });
        let server_task = tokio::spawn(async move {
            assert!(matches!(
                read_async_message(&mut server).await.unwrap(),
                Some(Message::Resize { .. })
            ));
            assert!(matches!(
                read_async_message(&mut server).await.unwrap(),
                Some(Message::Command(_))
            ));
            write_async_message(&mut server, Message::CommandErr("stop".to_string()))
                .await
                .unwrap();
        });

        let _ = attached_command(
            Box::new(client),
            "attach-session".to_string(),
            TerminalCapabilities::default(),
            terminal,
        )
        .await;
        server_task.await.unwrap();

        let writes = writes.lock().unwrap();
        assert!(writes[0].windows(5).any(|bytes| bytes == b"\x1b[>9u"));
        assert!(writes[0].windows(8).any(|bytes| bytes == b"\x1b[?1002h"));
        assert!(!writes[0].windows(8).any(|bytes| bytes == b"\x1b[?1003h"));
        assert!(writes[1].windows(4).any(|bytes| bytes == b"\x1b[<u"));
        assert!(writes[1].windows(8).any(|bytes| bytes == b"\x1b[?1002l"));
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
                    Message::Output {
                        sequence: sequence as u64,
                        bytes: (sequence as u64).to_le_bytes().to_vec(),
                    },
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
                Message::Output {
                    sequence: expected as u64,
                    bytes: (expected as u64).to_le_bytes().to_vec(),
                }
            );
        }

        writer_task.await.unwrap();
        reader_task.abort();
    }

    #[test]
    fn control_formatter_octal_escapes_binary_and_flush_delimiters() {
        assert_eq!(escape_control_bytes(b"a\0\\\n\xff"), r"a\000\134\012\377");
        assert_eq!(
            format_control_record(&ControlRecord::Output {
                pane: PaneId::new(7),
                bytes: b"ok\n".to_vec(),
            }),
            r"%output %7 ok\012"
        );
        assert_eq!(
            format_control_record(&ControlRecord::Error {
                sequence: 2,
                message: "bad\ncommand".to_string(),
            }),
            r"%error 2 bad\012command"
        );
    }

    #[tokio::test]
    async fn control_adapter_streams_lines_and_structured_records_without_terminal_mode() {
        let (client, mut server) = tokio::io::duplex(4096);
        let (input_tx, input_rx) = tokio::sync::mpsc::channel(2);
        input_tx
            .send(Ok("list-sessions".to_string()))
            .await
            .unwrap();
        let mut output = Vec::new();
        let client = control_io_loop(Box::new(client), input_rx, &mut output);
        let server = async {
            assert_eq!(
                read_async_message(&mut server).await.unwrap(),
                Some(Message::EnterControl)
            );
            write_async_message(&mut server, Message::ControlRecord(ControlRecord::Ready))
                .await
                .unwrap();
            assert_eq!(
                read_async_message(&mut server).await.unwrap(),
                Some(Message::ControlCommand {
                    sequence: 1,
                    command: "list-sessions".to_string(),
                })
            );
            for record in [
                ControlRecord::Begin { sequence: 1 },
                ControlRecord::End {
                    sequence: 1,
                    output: "work: 1 windows".to_string(),
                },
            ] {
                write_async_message(&mut server, Message::ControlRecord(record))
                    .await
                    .unwrap();
            }
            write_async_message(&mut server, Message::Shutdown)
                .await
                .unwrap();
        };
        let (client_result, ()) = tokio::join!(client, server);
        client_result.unwrap();
        assert_eq!(
            String::from_utf8(output).unwrap(),
            "%ready\n%begin 1\n%end 1 work: 1 windows\n"
        );
        drop(input_tx);
    }
}
