use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    fs::{self, File, OpenOptions},
    io::{self, IoSlice, Read, Write},
    path::{Path, PathBuf},
    process,
    sync::{
        mpsc::{self, Receiver, TryRecvError as StdTryRecvError},
        Arc,
    },
    thread,
    time::{Duration, Instant},
};

use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    runtime::Builder as RuntimeBuilder,
    sync::{mpsc as async_mpsc, mpsc::error::TrySendError, oneshot},
    task::JoinSet,
};
use wmux_config::{config_path, WmuxConfig};
use wmux_core::{
    build_window_scene_with_client_overlay, build_window_structure, execute, pane_area_rows,
    parse_command_text, render_damage_from_structure, render_diff_scene_with_capabilities,
    route_key, BareKey, ClientId, ClientInput, Command, CommandEffect, CommandList, CommandQueue,
    CommandSource, ControlNotification, ControlRecord, CopyMode, CopyModeResult, InputMode,
    InputRoute, JobContinuation, JobId, KeyCode, KeyEvent, KeyModifiers, Line, PaneId,
    PaneSceneOverrides, PaneViewport, QueuedCommand, RenderCapabilities, RenderState,
    RetainedPaneFrame, ServerEvent, ServerState, StructuralScene, Style,
};
use wmux_platform::{
    AcceptedConnection, BoxedIpcStream, JobBackend, JobEvent, JobRequest, MouseButton, MouseEvent,
    MouseEventKind, PeerIdentity, PlatformError, PlatformErrorKind, PlatformEvent, PlatformJobId,
    PlatformPaneId, PlatformRequest, PtyBackend, ServerPlatform, SpawnJob, SpawnPane, TerminalSize,
    TerminationMode,
};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    WireKeyCode, WireKeyEvent, FRAME_HEADER_LEN, MAX_FRAME, VERSION,
};

const TRACE_PREVIEW_BYTES: usize = 96;
const SYNC_OUTPUT_TIMEOUT: Duration = Duration::from_secs(1);
const IDLE_FRAME_THRESHOLD: Duration = Duration::from_millis(12);
const MIN_FRAME_DELAY: Duration = Duration::from_millis(4);
const MAX_FRAME_DELAY: Duration = Duration::from_millis(8);
const INPUT_PRIORITY_WINDOW: Duration = Duration::from_millis(50);
const REDRAW_CYCLE_DELAY: Duration = Duration::from_millis(1);
const RESIZE_REPAINT_QUIET: Duration = Duration::from_millis(16);
const RESIZE_REPAINT_MAX: Duration = Duration::from_millis(120);
const CONTROL_EVENTS_PER_TURN: usize = 256;
const COMMANDS_PER_TURN: usize = 64;
const PASTES_PER_TURN: usize = 64;
const PASTE_CHUNK_BYTES: usize = 64 * 1024;
const CLIENT_OUTPUT_MESSAGES: usize = 64;
const CLIENT_OUTPUT_BYTES: usize = 4 * 1024 * 1024;
const CONTROL_OUTPUT_RESERVE_BYTES: usize = 128 * 1024;
const CLIENT_MAX_FRAME_BYTES: usize = FRAME_HEADER_LEN + MAX_FRAME;
const PER_PANE_OUTPUT_BYTES: usize = 64 * 1024;
const PER_PANE_OUTPUT_TIME: Duration = Duration::from_millis(1);
const OUTPUT_ROUND_TIME: Duration = Duration::from_millis(4);
const CONNECTION_DRAIN_TIMEOUT: Duration = Duration::from_secs(5);
const MAX_SOURCE_FILE_BYTES: usize = 1024 * 1024;
const MAX_BUFFER_FILE_BYTES: usize = wmux_core::paste::MAX_PASTE_BUFFER_BYTES;

pub fn run_with_platform(platform: Box<dyn ServerPlatform>) -> io::Result<()> {
    run_with_platform_and_config(platform, load_config())
}

pub fn run_with_platform_and_config(
    platform: Box<dyn ServerPlatform>,
    config: WmuxConfig,
) -> io::Result<()> {
    RuntimeBuilder::new_multi_thread()
        .worker_threads(2)
        .thread_name("wmux-io")
        .enable_all()
        .build()?
        .block_on(run_async(platform, config))
}

async fn run_async(mut platform: Box<dyn ServerPlatform>, config: WmuxConfig) -> io::Result<()> {
    let mut listener = platform.bind().map_err(PlatformError::into_io)?;
    let endpoint_name = listener.endpoint().display().to_string();
    let owner_identity = listener.owner_identity().clone();
    let (owner_tx, owner_rx) = mpsc::channel();
    let notify_owner = owner_tx.clone();
    let notifier = Arc::new(move |_| {
        let _ = notify_owner.send(OwnerMessage::PlatformReady);
    });
    let pty_backend = platform
        .create_pty_backend(notifier)
        .map_err(PlatformError::into_io)?;
    let notify_owner = owner_tx.clone();
    let job_notifier = Arc::new(move |_| {
        let _ = notify_owner.send(OwnerMessage::PlatformReady);
    });
    let job_backend = platform
        .create_job_backend(job_notifier)
        .map_err(PlatformError::into_io)?;
    let (shutdown_tx, mut shutdown_rx) = oneshot::channel();
    let state_owner = thread::Builder::new()
        .name("wmux-state-owner".to_string())
        .spawn(move || {
            ServerOwner::new(config, pty_backend, job_backend, shutdown_tx).run(owner_rx)
        })?;

    eprintln!("wmux clean server listening on {endpoint_name}");
    let mut connections = JoinSet::new();
    let accept_result = 'accept: loop {
        tokio::select! {
            biased;
            shutdown = &mut shutdown_rx => {
                break 'accept shutdown.map_err(|_| io::Error::new(
                    io::ErrorKind::BrokenPipe,
                    "server owner stopped before requesting shutdown",
                ));
            }
            completed = connections.join_next(), if !connections.is_empty() => {
                report_connection_completion(completed);
            }
            accepted = listener.accept() => {
                let accepted = match accepted {
                    Ok(accepted) => accepted,
                    Err(error) => {
                    let error = error.into_io();
                    eprintln!("accept error: {error}");
                    tokio::select! {
                        biased;
                        shutdown = &mut shutdown_rx => {
                            break 'accept shutdown.map_err(|_| io::Error::new(
                                io::ErrorKind::BrokenPipe,
                                "server owner stopped before requesting shutdown",
                            ));
                        }
                        _ = tokio::time::sleep(Duration::from_millis(100)) => {}
                    }
                    continue;
                    }
                };
                let connection_owner = owner_tx.clone();
                let connection_identity = owner_identity.clone();
                connections.spawn(async move {
                    handle_connection(accepted, connection_owner, connection_identity).await
                });
            }
        }
    };

    drop(listener);
    drain_connection_tasks(&mut connections).await;
    let _ = owner_tx.send(OwnerMessage::Stop);
    drop(owner_tx);
    let owner_result = state_owner
        .join()
        .map_err(|_| io::Error::other("server owner thread panicked"));
    accept_result.and(owner_result)
}

fn report_connection_completion(completed: Option<Result<io::Result<()>, tokio::task::JoinError>>) {
    match completed {
        Some(Ok(Err(error))) => eprintln!("client error: {error}"),
        Some(Err(error)) if !error.is_cancelled() => eprintln!("client task error: {error}"),
        _ => {}
    }
}

async fn drain_connection_tasks(connections: &mut JoinSet<io::Result<()>>) {
    let deadline = tokio::time::Instant::now() + CONNECTION_DRAIN_TIMEOUT;
    while !connections.is_empty() {
        let remaining = deadline.saturating_duration_since(tokio::time::Instant::now());
        if remaining.is_zero() {
            break;
        }
        match tokio::time::timeout(remaining, connections.join_next()).await {
            Ok(completed) => report_connection_completion(completed),
            Err(_) => break,
        }
    }
    if !connections.is_empty() {
        connections.abort_all();
        while let Some(completed) = connections.join_next().await {
            report_connection_completion(Some(completed));
        }
    }
}

fn load_config() -> WmuxConfig {
    match WmuxConfig::load_or_create() {
        Ok(config) => config,
        Err(error) => {
            eprintln!(
                "wmux config error at {}: {error}; using defaults",
                config_path().display()
            );
            WmuxConfig::default()
        }
    }
}

enum OwnerMessage {
    Register {
        outbound: async_mpsc::Sender<Outbound>,
        capabilities: TerminalCapabilities,
        reply: oneshot::Sender<ClientId>,
    },
    Event(ServerEvent),
    Commands {
        client: ClientId,
        commands: CommandList,
        source: CommandSource,
    },
    InvalidCommand {
        client: ClientId,
        message: String,
    },
    EnterControl(ClientId),
    ControlCommand {
        client: ClientId,
        sequence: u64,
        command: String,
    },
    Disconnect(ClientId),
    OutboundDrained {
        client: ClientId,
        bytes: usize,
    },
    PlatformReady,
    Stop,
}

enum Outbound {
    Message(Message),
    Shutdown(Message),
}

async fn handle_connection(
    accepted: AcceptedConnection,
    owner_tx: mpsc::Sender<OwnerMessage>,
    owner_identity: PeerIdentity,
) -> io::Result<()> {
    if accepted.peer != owner_identity {
        return Err(io::Error::new(
            io::ErrorKind::PermissionDenied,
            "IPC peer identity does not match endpoint owner",
        ));
    }
    let mut stream = accepted.stream;
    let capabilities = handshake(&mut stream).await?;

    let (outbound_tx, outbound_rx) = async_mpsc::channel(CLIENT_OUTPUT_MESSAGES);
    let (reply_tx, reply_rx) = oneshot::channel();
    owner_tx
        .send(OwnerMessage::Register {
            outbound: outbound_tx,
            capabilities,
            reply: reply_tx,
        })
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server owner stopped"))?;
    let client = reply_rx
        .await
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server owner stopped"))?;

    let result = connection_io_loop(stream, outbound_rx, &owner_tx, client).await;
    let _ = owner_tx.send(OwnerMessage::Disconnect(client));
    result
}

async fn connection_io_loop(
    stream: BoxedIpcStream,
    outbound_rx: async_mpsc::Receiver<Outbound>,
    owner_tx: &mpsc::Sender<OwnerMessage>,
    client: ClientId,
) -> io::Result<()> {
    let (mut reader, mut writer) = tokio::io::split(stream);
    let read_owner = owner_tx.clone();
    let write_owner = owner_tx.clone();
    let read = async move {
        loop {
            match read_async_message(&mut reader).await? {
                Some(message) => {
                    if let Some(owner_message) = protocol_event(client, message) {
                        read_owner.send(owner_message).map_err(|_| {
                            io::Error::new(io::ErrorKind::BrokenPipe, "server owner stopped")
                        })?;
                    }
                }
                None => return Ok(()),
            }
        }
    };
    let write = write_outbound_messages(&mut writer, outbound_rx, &write_owner, client);

    tokio::select! {
        result = read => result,
        result = write => result,
    }
}

async fn write_outbound_messages(
    writer: &mut (impl AsyncWrite + Unpin),
    mut outbound_rx: async_mpsc::Receiver<Outbound>,
    owner_tx: &mpsc::Sender<OwnerMessage>,
    client: ClientId,
) -> io::Result<()> {
    while let Some(outbound) = outbound_rx.recv().await {
        let (message, shutdown) = match outbound {
            Outbound::Message(message) => (message, false),
            Outbound::Shutdown(message) => (message, true),
        };
        let bytes = message.wire_len();
        write_async_message(&mut *writer, message).await?;
        owner_tx
            .send(OwnerMessage::OutboundDrained { client, bytes })
            .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server owner stopped"))?;
        if shutdown {
            return Ok(());
        }
    }
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
    write_vectored_frame(writer, &frame).await
}

async fn write_vectored_frame(
    writer: &mut (impl AsyncWrite + Unpin),
    frame: &EncodedFrame,
) -> io::Result<()> {
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

fn protocol_event(client: ClientId, message: Message) -> Option<OwnerMessage> {
    let event = match message {
        Message::Command(raw) => match parse_command_line(&raw) {
            Ok(commands) => {
                return Some(OwnerMessage::Commands {
                    client,
                    commands,
                    source: CommandSource::ClientRequest,
                });
            }
            Err(error) => {
                return Some(OwnerMessage::InvalidCommand {
                    client,
                    message: error.to_string(),
                });
            }
        },
        Message::EnterControl => return Some(OwnerMessage::EnterControl(client)),
        Message::ControlCommand { sequence, command } => {
            return Some(OwnerMessage::ControlCommand {
                client,
                sequence,
                command,
            });
        }
        Message::Input(bytes) => ServerEvent::ClientInput {
            client,
            input: ClientInput::Bytes(bytes),
        },
        Message::Key(event) => match core_key_event(event) {
            Ok(event) => ServerEvent::ClientKey { client, event },
            Err(message) => return Some(OwnerMessage::InvalidCommand { client, message }),
        },
        Message::Paste(bytes) => ServerEvent::ClientInput {
            client,
            input: ClientInput::Paste(bytes),
        },
        Message::Mouse(event) => ServerEvent::ClientMouse { client, event },
        Message::Resize { cols, rows } => ServerEvent::ClientResize { client, cols, rows },
        Message::Detach => {
            return Some(OwnerMessage::Commands {
                client,
                commands: CommandList::new(vec![Command::DetachClient])
                    .expect("one command is within the list bound"),
                source: CommandSource::ClientRequest,
            });
        }
        Message::Shutdown => {
            return Some(OwnerMessage::Commands {
                client,
                commands: CommandList::new(vec![Command::KillServer])
                    .expect("one command is within the list bound"),
                source: CommandSource::ClientRequest,
            });
        }
        _ => return None,
    };
    Some(OwnerMessage::Event(event))
}

async fn handshake(
    stream: &mut (impl AsyncRead + AsyncWrite + Unpin),
) -> io::Result<TerminalCapabilities> {
    match read_async_message(&mut *stream).await? {
        Some(Message::Hello {
            version,
            capabilities,
            ..
        }) if version == VERSION => {
            write_async_message(
                stream,
                Message::HelloOk {
                    version: VERSION,
                    pid: process::id(),
                    capabilities,
                },
            )
            .await?;
            Ok(capabilities)
        }
        Some(Message::Hello { version, .. }) => {
            write_async_message(
                stream,
                Message::CommandErr(format!(
                    "unsupported protocol version {version}; expected {VERSION}"
                )),
            )
            .await?;
            Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "unsupported protocol version",
            ))
        }
        Some(other) => {
            write_async_message(
                stream,
                Message::CommandErr(format!("expected hello, got {other:?}")),
            )
            .await?;
            Err(io::Error::new(io::ErrorKind::InvalidData, "expected hello"))
        }
        None => Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "client closed before hello",
        )),
    }
}

fn parse_command_line(raw: &str) -> io::Result<CommandList> {
    parse_command_text(raw).map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))
}

fn core_key_event(event: WireKeyEvent) -> Result<KeyEvent, String> {
    let mut modifiers = KeyModifiers::from_bits(event.modifiers.bits())
        .ok_or_else(|| "wire key contains unsupported modifiers".to_string())?;
    let mut bare = match event.code {
        WireKeyCode::Char(character) => BareKey::Char(character),
        WireKeyCode::Left => BareKey::Left,
        WireKeyCode::Right => BareKey::Right,
        WireKeyCode::Up => BareKey::Up,
        WireKeyCode::Down => BareKey::Down,
        WireKeyCode::Home => BareKey::Home,
        WireKeyCode::End => BareKey::End,
        WireKeyCode::PageUp => BareKey::PageUp,
        WireKeyCode::PageDown => BareKey::PageDown,
        WireKeyCode::Backspace => BareKey::Backspace,
        WireKeyCode::Delete => BareKey::Delete,
        WireKeyCode::Insert => BareKey::Insert,
        WireKeyCode::Enter => BareKey::Enter,
        WireKeyCode::Tab => BareKey::Tab,
        WireKeyCode::BackTab => BareKey::BackTab,
        WireKeyCode::Escape => BareKey::Escape,
        WireKeyCode::Function(number) => BareKey::Function(number),
    };
    if matches!(bare, BareKey::Char(_)) && event.raw.len() == 1 && event.raw[0].is_ascii_graphic() {
        // Legacy terminal input identifies a printable binding by the produced glyph. Console
        // APIs may redundantly retain the Shift or Control keys used to produce that glyph.
        bare = BareKey::Char(char::from(event.raw[0]));
        let redundant = KeyModifiers::SHIFT | KeyModifiers::CONTROL;
        modifiers = KeyModifiers::from_bits(modifiers.bits() & !redundant.bits())
            .expect("removing supported modifiers keeps a valid modifier set");
    }
    let code = KeyCode::try_new(bare, modifiers).map_err(|error| error.to_string())?;
    Ok(KeyEvent::new(code, event.raw))
}

struct Runtime {
    state: ServerState,
    queue: CommandQueue,
    config: WmuxConfig,
    platform_panes: BTreeMap<PaneId, PlatformPane>,
    output_ring: VecDeque<PaneId>,
    sync_started_at: BTreeMap<PaneId, Instant>,
    resize_repaint_holds: BTreeMap<PaneId, ResizeRepaintHold>,
    history_growth: Vec<(PaneId, u64)>,
    published_output: Vec<(PaneId, Vec<u8>)>,
    pty_backend: Box<dyn PtyBackend>,
    job_backend: Box<dyn JobBackend>,
    #[cfg(test)]
    test_platform: TestPtyHandle,
    #[cfg(test)]
    test_jobs: TestJobHandle,
    #[cfg(test)]
    test_inputs: Vec<(PaneId, Vec<u8>)>,
}

#[cfg(test)]
#[derive(Default)]
struct TestPtyState {
    requests: Vec<PlatformRequest>,
    events: BTreeMap<PlatformPaneId, VecDeque<PlatformEvent>>,
    notifier: Option<wmux_platform::PlatformNotifier>,
}

#[cfg(test)]
#[derive(Clone, Default)]
struct TestPtyHandle(Arc<std::sync::Mutex<TestPtyState>>);

#[cfg(test)]
impl TestPtyHandle {
    fn emit(&self, pane: PlatformPaneId, event: PlatformEvent) {
        let notifier = {
            let mut state = self.0.lock().unwrap();
            state.events.entry(pane).or_default().push_back(event);
            state.notifier.clone()
        };
        if let Some(notifier) = notifier {
            notifier(pane);
        }
    }

    fn requests(&self) -> Vec<PlatformRequest> {
        self.0.lock().unwrap().requests.clone()
    }

    fn set_notifier(&self, notifier: wmux_platform::PlatformNotifier) {
        self.0.lock().unwrap().notifier = Some(notifier);
    }
}

#[cfg(test)]
struct TestPtyBackend {
    state: TestPtyHandle,
}

#[cfg(test)]
#[derive(Default)]
struct TestJobState {
    requests: Vec<JobRequest>,
    events: BTreeMap<PlatformJobId, VecDeque<JobEvent>>,
    notifier: Option<wmux_platform::JobNotifier>,
}

#[cfg(test)]
#[derive(Clone, Default)]
struct TestJobHandle(Arc<std::sync::Mutex<TestJobState>>);

#[cfg(test)]
impl TestJobHandle {
    fn emit(&self, job: PlatformJobId, event: JobEvent) {
        let notifier = {
            let mut state = self.0.lock().unwrap();
            state.events.entry(job).or_default().push_back(event);
            state.notifier.clone()
        };
        if let Some(notifier) = notifier {
            notifier(job);
        }
    }

    fn requests(&self) -> Vec<JobRequest> {
        self.0.lock().unwrap().requests.clone()
    }
    fn set_notifier(&self, notifier: wmux_platform::JobNotifier) {
        self.0.lock().unwrap().notifier = Some(notifier);
    }
}

#[cfg(test)]
struct TestJobBackend {
    state: TestJobHandle,
}

#[cfg(test)]
impl TestJobBackend {
    fn pair() -> (Self, TestJobHandle) {
        let state = TestJobHandle::default();
        (
            Self {
                state: state.clone(),
            },
            state,
        )
    }
}

#[cfg(test)]
impl JobBackend for TestJobBackend {
    fn submit(&mut self, request: JobRequest) -> wmux_platform::PlatformResult<()> {
        self.state.0.lock().unwrap().requests.push(request);
        Ok(())
    }

    fn try_next_event(
        &mut self,
        job: PlatformJobId,
    ) -> wmux_platform::PlatformResult<Option<JobEvent>> {
        Ok(self
            .state
            .0
            .lock()
            .unwrap()
            .events
            .get_mut(&job)
            .and_then(VecDeque::pop_front))
    }
}

#[cfg(test)]
impl TestPtyBackend {
    fn pair() -> (Self, TestPtyHandle) {
        let state = TestPtyHandle::default();
        (
            Self {
                state: state.clone(),
            },
            state,
        )
    }
}

#[cfg(test)]
impl PtyBackend for TestPtyBackend {
    fn submit(&mut self, request: PlatformRequest) -> wmux_platform::PlatformResult<()> {
        self.state.0.lock().unwrap().requests.push(request);
        Ok(())
    }

    fn try_next_event(
        &mut self,
        pane: PlatformPaneId,
    ) -> wmux_platform::PlatformResult<Option<PlatformEvent>> {
        Ok(self
            .state
            .0
            .lock()
            .unwrap()
            .events
            .get_mut(&pane)
            .and_then(VecDeque::pop_front))
    }
}

#[derive(Clone, Copy, Default)]
struct OutputResult {
    changed: bool,
    publishable: bool,
    synchronized_commit: bool,
}

impl Runtime {
    fn with_backends(
        config: WmuxConfig,
        pty_backend: Box<dyn PtyBackend>,
        job_backend: Box<dyn JobBackend>,
    ) -> Self {
        Self {
            state: ServerState::new(),
            queue: CommandQueue::default(),
            config,
            platform_panes: BTreeMap::new(),
            output_ring: VecDeque::new(),
            sync_started_at: BTreeMap::new(),
            resize_repaint_holds: BTreeMap::new(),
            history_growth: Vec::new(),
            published_output: Vec::new(),
            pty_backend,
            job_backend,
            #[cfg(test)]
            test_platform: TestPtyHandle::default(),
            #[cfg(test)]
            test_jobs: TestJobHandle::default(),
            #[cfg(test)]
            test_inputs: Vec::new(),
        }
    }

    #[cfg(test)]
    fn with_config(config: WmuxConfig) -> Self {
        let (backend, handle) = TestPtyBackend::pair();
        let (job_backend, job_handle) = TestJobBackend::pair();
        let mut runtime = Self::with_backends(config, Box::new(backend), Box::new(job_backend));
        runtime.test_platform = handle;
        runtime.test_jobs = job_handle;
        runtime
    }

    #[cfg(test)]
    fn add_test_platform_pane(&mut self, pane: PaneId, size: TerminalSize) {
        self.platform_panes.insert(pane, PlatformPane::new(size));
        if !self.output_ring.contains(&pane) {
            self.output_ring.push_back(pane);
        }
    }

    #[cfg(test)]
    fn emit_test_platform_event(&self, pane: PaneId, event: PlatformEvent) {
        self.test_platform
            .emit(PlatformPaneId::new(pane.raw()), event);
    }

    #[cfg(test)]
    fn test_platform_requests(&self) -> Vec<PlatformRequest> {
        self.test_platform.requests()
    }

    fn ensure_platform_pane(&mut self, pane_id: PaneId, size: TerminalSize) -> io::Result<()> {
        if self.platform_panes.contains_key(&pane_id) {
            return Ok(());
        }
        let environment = self
            .config
            .pane_environment(pane_id.raw())
            .into_iter()
            .map(|(key, value)| (key.into(), value.into()))
            .collect();
        self.pty_backend
            .submit(PlatformRequest::SpawnPane(SpawnPane {
                pane: PlatformPaneId::new(pane_id.raw()),
                size,
                command: None,
                cwd: None,
                environment,
            }))
            .map_err(PlatformError::into_io)?;
        self.platform_panes.insert(pane_id, PlatformPane::new(size));
        self.output_ring.push_back(pane_id);
        Ok(())
    }

    fn ensure_platform_panes_for_client(&mut self, client: ClientId) -> io::Result<()> {
        let pane_sizes = self.pane_sizes_for_client(client);
        for (pane, size) in &pane_sizes {
            self.ensure_platform_pane(*pane, *size)?;
        }
        Ok(())
    }

    fn apply_pending_pane_resizes(&mut self) -> io::Result<usize> {
        let resizes = self.state.take_pending_pane_resizes();
        let mut applied = 0;
        for resize in resizes {
            let size = TerminalSize::new(resize.new.cols, resize.new.rows);
            if self.resize_platform_pane(resize.pane, size)? {
                applied += 1;
            }
        }
        Ok(applied)
    }

    fn pane_sizes_for_client(&self, client: ClientId) -> BTreeMap<PaneId, TerminalSize> {
        let Some((_, window_id, _)) = self.state.active_window_and_pane_for_client(client) else {
            return BTreeMap::new();
        };
        let Some(window) = self.state.window(window_id) else {
            return BTreeMap::new();
        };
        window
            .panes
            .iter()
            .filter_map(|pane| {
                let rect = self.state.pane(*pane)?.rect;
                Some((*pane, TerminalSize::new(rect.cols, rect.rows)))
            })
            .collect()
    }

    fn resize_client_window(&mut self, client: ClientId, size: TerminalSize) {
        let Some((_, window, _)) = self.state.active_window_and_pane_for_client(client) else {
            return;
        };
        self.state
            .resize_window(window, size.cols, pane_area_rows(size.rows));
    }

    fn resize_platform_pane(&mut self, pane_id: PaneId, size: TerminalSize) -> io::Result<bool> {
        if self
            .platform_panes
            .get(&pane_id)
            .is_none_or(|platform| platform.size == size)
        {
            return Ok(false);
        }
        let Some(retained_frame) = self
            .state
            .pane(pane_id)
            .map(|pane| RetainedPaneFrame::capture(pane_id, &pane.screen))
        else {
            return Ok(false);
        };
        let Some(platform) = self.platform_panes.get_mut(&pane_id) else {
            return Ok(false);
        };
        self.pty_backend
            .submit(PlatformRequest::ResizePane {
                pane: PlatformPaneId::new(pane_id.raw()),
                size,
            })
            .map_err(PlatformError::into_io)?;
        platform.size = size;
        let now = Instant::now();
        self.resize_repaint_holds.insert(
            pane_id,
            ResizeRepaintHold {
                quiet_until: now + RESIZE_REPAINT_QUIET,
                max_until: now + RESIZE_REPAINT_MAX,
                retained_frame,
            },
        );
        Ok(true)
    }

    fn cleanup_platform_panes(&mut self) {
        let removed = self
            .platform_panes
            .keys()
            .filter(|pane| !self.state.panes.contains_key(pane))
            .copied()
            .collect::<Vec<_>>();
        for pane in removed {
            let Some(platform) = self.platform_panes.get_mut(&pane) else {
                continue;
            };
            if platform.termination_requested {
                continue;
            }
            if let Err(error) = self.pty_backend.submit(PlatformRequest::TerminatePane {
                pane: PlatformPaneId::new(pane.raw()),
                mode: TerminationMode::Force,
            }) {
                eprintln!("platform pane termination error: {error}");
            }
            platform.termination_requested = true;
        }
        self.sync_started_at
            .retain(|pane, _| self.state.panes.contains_key(pane));
        self.resize_repaint_holds
            .retain(|pane, _| self.state.panes.contains_key(pane));
    }

    fn shutdown_platform_panes(&mut self) {
        for pane in self.platform_panes.keys().copied().collect::<Vec<_>>() {
            let _ = self.pty_backend.submit(PlatformRequest::TerminatePane {
                pane: PlatformPaneId::new(pane.raw()),
                mode: TerminationMode::Force,
            });
        }
        self.platform_panes.clear();
        self.output_ring.clear();
        self.sync_started_at.clear();
        self.resize_repaint_holds.clear();
        self.history_growth.clear();
    }

    fn write_pane_input(&mut self, pane_id: PaneId, input: ClientInput) -> io::Result<()> {
        if let Some(pane) = self.state.pane_mut(pane_id) {
            pane.screen.set_synchronized_output(false);
        }
        self.sync_started_at.remove(&pane_id);
        self.resize_repaint_holds.remove(&pane_id);

        let bracketed = self
            .state
            .pane(pane_id)
            .is_some_and(|pane| pane.screen.bracketed_paste());
        let bytes = input.into_pty_bytes(bracketed);
        trace_bytes("pane_input", pane_id, &bytes);
        #[cfg(test)]
        self.test_inputs.push((pane_id, bytes.clone()));
        if self.platform_panes.contains_key(&pane_id) {
            self.pty_backend
                .submit(PlatformRequest::WritePane {
                    pane: PlatformPaneId::new(pane_id.raw()),
                    bytes,
                })
                .map_err(PlatformError::into_io)?;
        }
        Ok(())
    }

    fn apply_pty_output(&mut self, pane_id: PaneId, bytes: &[u8]) -> OutputResult {
        if bytes.is_empty() {
            return OutputResult::default();
        }
        trace_bytes("conpty_output_raw", pane_id, bytes);
        let Some(pane) = self.state.pane_mut(pane_id) else {
            return OutputResult::default();
        };
        let was_synchronized = pane.screen.synchronized_output();
        let history_before = pane.screen.history_added();
        let synchronized_epoch = pane.screen.synchronized_output_epoch();
        let generation = pane.terminal.feed(&mut pane.screen, bytes);
        let is_synchronized = pane.screen.synchronized_output();
        if is_synchronized && !was_synchronized {
            self.sync_started_at.insert(pane_id, Instant::now());
        } else if !is_synchronized && was_synchronized {
            self.sync_started_at.remove(&pane_id);
        }
        let synchronized_commit = pane.screen.synchronized_output_epoch() != synchronized_epoch;
        let history_added = pane.screen.history_added().wrapping_sub(history_before);
        if history_added > 0 {
            self.history_growth.push((pane_id, history_added));
        }
        if synchronized_commit {
            self.release_resize_repaint_hold(pane_id);
        } else {
            self.extend_resize_repaint_hold(pane_id);
        }
        let held = self.resize_repaint_holds.contains_key(&pane_id);
        OutputResult {
            changed: generation.is_some(),
            publishable: generation.is_some() && !is_synchronized && !held,
            synchronized_commit,
        }
    }

    fn take_history_growth(&mut self) -> Vec<(PaneId, u64)> {
        std::mem::take(&mut self.history_growth)
    }

    fn apply_pty_exit(&mut self, pane_id: PaneId, exit_code: Option<u32>) -> bool {
        let Some(pane) = self.state.pane_mut(pane_id) else {
            return false;
        };
        if pane.dead {
            return false;
        }
        pane.dead = true;
        let status = match exit_code {
            Some(code) => format!("\r\n[wmux pane exited code={code}]\r\n"),
            None => "\r\n[wmux pane exited]\r\n".to_string(),
        };
        pane.terminal.feed(&mut pane.screen, status.as_bytes());
        true
    }

    fn extend_resize_repaint_hold(&mut self, pane_id: PaneId) {
        let Some(hold) = self.resize_repaint_holds.get_mut(&pane_id) else {
            return;
        };
        hold.quiet_until = (Instant::now() + RESIZE_REPAINT_QUIET).min(hold.max_until);
    }

    fn release_resize_repaint_hold(&mut self, pane_id: PaneId) -> bool {
        if self.resize_repaint_holds.remove(&pane_id).is_none() {
            return false;
        }
        if let Some(pane) = self.state.pane_mut(pane_id) {
            pane.screen.mark_full_damage();
        }
        true
    }

    fn expire_synchronized_output(&mut self, now: Instant) -> bool {
        let expired = self
            .sync_started_at
            .iter()
            .filter_map(|(pane, started)| {
                (now.duration_since(*started) >= SYNC_OUTPUT_TIMEOUT).then_some(*pane)
            })
            .collect::<Vec<_>>();
        for pane_id in &expired {
            self.sync_started_at.remove(pane_id);
            if let Some(pane) = self.state.pane_mut(*pane_id) {
                pane.screen.set_synchronized_output(false);
            }
        }
        !expired.is_empty()
    }

    fn expire_resize_repaint_holds(&mut self, now: Instant) -> bool {
        let expired = self
            .resize_repaint_holds
            .iter()
            .filter_map(|(pane, hold)| {
                let synchronized = self
                    .state
                    .pane(*pane)
                    .is_some_and(|pane| pane.screen.synchronized_output());
                ((!synchronized && now >= hold.quiet_until) || now >= hold.max_until)
                    .then_some(*pane)
            })
            .collect::<Vec<_>>();
        for pane_id in &expired {
            self.release_resize_repaint_hold(*pane_id);
        }
        !expired.is_empty()
    }

    fn process_output_round(&mut self, budget: OutputBudget) -> OutputResult {
        let round_started = Instant::now();
        let turns = self.output_ring.len();
        let mut result = OutputResult::default();
        for _ in 0..turns {
            if round_started.elapsed() >= budget.round_time {
                break;
            }
            let Some(pane_id) = self.output_ring.pop_front() else {
                break;
            };
            if !self.platform_panes.contains_key(&pane_id) {
                continue;
            }
            let collected = collect_pane_events(
                &mut *self.pty_backend,
                PlatformPaneId::new(pane_id.raw()),
                budget.per_pane_bytes,
                budget.per_pane_time,
            );
            let (still_polling, completed) = {
                let platform = self
                    .platform_panes
                    .get_mut(&pane_id)
                    .expect("platform pane still exists");
                if collected.closed {
                    platform.closed = true;
                }
                let first_exit = collected.exit_code.is_some() && !platform.exited;
                if first_exit {
                    platform.exited = true;
                }
                let completed = platform.closed;
                (!completed, completed)
            };
            if let Some(error) = &collected.error {
                eprintln!("platform pane event error: {error}");
            }
            if still_polling {
                self.output_ring.push_back(pane_id);
            }
            let applied = self.apply_pty_output(pane_id, &collected.bytes);
            if !collected.bytes.is_empty() {
                self.published_output.push((pane_id, collected.bytes));
            }
            result.changed |= applied.changed;
            result.publishable |= applied.publishable;
            result.synchronized_commit |= applied.synchronized_commit;
            let exit_to_apply = match collected.exit_code {
                Some(Some(exit_code)) => Some(Some(exit_code)),
                Some(None) if collected.closed => Some(None),
                _ => None,
            };
            if let Some(exit_code) = exit_to_apply {
                if self.apply_pty_exit(pane_id, exit_code) {
                    result.changed = true;
                }
            }
            if completed {
                self.platform_panes.remove(&pane_id);
                self.sync_started_at.remove(&pane_id);
                let deferred_final_frame = self.resize_repaint_holds.remove(&pane_id).is_some();
                if deferred_final_frame && result.changed {
                    result.publishable = true;
                }
            }
        }
        result
    }

    fn take_published_output(&mut self) -> Vec<(PaneId, Vec<u8>)> {
        std::mem::take(&mut self.published_output)
    }
}

struct PlatformPane {
    exited: bool,
    closed: bool,
    termination_requested: bool,
    size: TerminalSize,
}

impl PlatformPane {
    fn new(size: TerminalSize) -> Self {
        Self {
            exited: false,
            closed: false,
            termination_requested: false,
            size,
        }
    }
}

#[derive(Clone, Copy)]
struct OutputBudget {
    per_pane_bytes: usize,
    per_pane_time: Duration,
    round_time: Duration,
}

impl Default for OutputBudget {
    fn default() -> Self {
        Self {
            per_pane_bytes: PER_PANE_OUTPUT_BYTES,
            per_pane_time: PER_PANE_OUTPUT_TIME,
            round_time: OUTPUT_ROUND_TIME,
        }
    }
}

#[derive(Default)]
struct CollectedOutput {
    bytes: Vec<u8>,
    exit_code: Option<Option<u32>>,
    closed: bool,
    error: Option<PlatformError>,
}

fn collect_pane_events(
    backend: &mut dyn PtyBackend,
    pane: PlatformPaneId,
    byte_budget: usize,
    time_budget: Duration,
) -> CollectedOutput {
    let started = Instant::now();
    let mut collected = CollectedOutput::default();
    while collected.bytes.len() < byte_budget && started.elapsed() < time_budget {
        match backend.try_next_event(pane) {
            Ok(Some(PlatformEvent::PtyOutput {
                pane: event_pane,
                bytes,
            })) if event_pane == pane => collected.bytes.extend_from_slice(&bytes),
            Ok(Some(PlatformEvent::PtyExited {
                pane: event_pane,
                exit_code,
            })) if event_pane == pane => {
                if collected.exit_code.is_none() || exit_code.is_some() {
                    collected.exit_code = Some(exit_code);
                }
            }
            Ok(Some(PlatformEvent::PtyClosed { pane: event_pane })) if event_pane == pane => {
                collected.closed = true;
                if collected.exit_code.is_none() {
                    collected.exit_code = Some(None);
                }
                break;
            }
            Ok(Some(PlatformEvent::BackendError {
                pane: event_pane,
                error,
            })) if event_pane == pane => collected.error = Some(error),
            Ok(Some(_)) => {
                collected.error = Some(PlatformError::new(
                    PlatformErrorKind::InvalidData,
                    "drain pane events",
                    "backend returned an event for the wrong pane",
                ));
                break;
            }
            Ok(None) => break,
            Err(error) => {
                collected.error = Some(error);
                break;
            }
        }
    }
    collected
}

struct ResizeRepaintHold {
    quiet_until: Instant,
    max_until: Instant,
    retained_frame: RetainedPaneFrame,
}

#[derive(Clone, Copy)]
enum RenderCause {
    Output,
    Structural,
    SynchronizedCommit,
}

struct FrameScheduler {
    deadline: Option<Instant>,
    last_publish: Option<Instant>,
    burst_frames: u8,
    input_priority_until: Option<Instant>,
}

impl FrameScheduler {
    fn new() -> Self {
        Self {
            deadline: None,
            last_publish: None,
            burst_frames: 0,
            input_priority_until: None,
        }
    }

    fn note_input(&mut self, now: Instant) {
        self.input_priority_until = Some(now + INPUT_PRIORITY_WINDOW);
    }

    fn request(&mut self, now: Instant, cause: RenderCause) {
        let input_priority = self.input_priority_until.is_some_and(|until| now <= until);
        let idle = self
            .last_publish
            .is_none_or(|last| now.saturating_duration_since(last) >= IDLE_FRAME_THRESHOLD);
        let deadline = if matches!(cause, RenderCause::Structural) {
            now
        } else if input_priority || matches!(cause, RenderCause::SynchronizedCommit) {
            now + REDRAW_CYCLE_DELAY
        } else if idle {
            now
        } else {
            let extra = u64::from((self.burst_frames / 4).min(4));
            now + (MIN_FRAME_DELAY + Duration::from_millis(extra)).min(MAX_FRAME_DELAY)
        };
        self.deadline = Some(
            self.deadline
                .map_or(deadline, |pending| pending.min(deadline)),
        );
    }

    fn published(&mut self, now: Instant) {
        if self
            .last_publish
            .is_some_and(|last| now.saturating_duration_since(last) < IDLE_FRAME_THRESHOLD)
        {
            self.burst_frames = self.burst_frames.saturating_add(1);
        } else {
            self.burst_frames = 0;
        }
        self.last_publish = Some(now);
        self.deadline = None;
        if self.input_priority_until.is_some_and(|until| now > until) {
            self.input_priority_until = None;
        }
    }
}

struct ClientView {
    outbound: async_mpsc::Sender<Outbound>,
    queued_bytes: usize,
    size: TerminalSize,
    render_state: RenderState,
    attached: bool,
    blocked: bool,
    full_render: bool,
    scheduler: FrameScheduler,
    consumed_generations: BTreeMap<PaneId, u64>,
    structure: Option<StructuralScene>,
    capabilities: RenderCapabilities,
    scroll_offsets: BTreeMap<PaneId, usize>,
    copy_mode: Option<CopyMode>,
    control: Option<ControlClient>,
}

struct ControlClient {
    next_sequence: u64,
    paused: bool,
    pause_sent: bool,
    subscribed_output: bool,
}

impl ClientView {
    fn new(outbound: async_mpsc::Sender<Outbound>, capabilities: TerminalCapabilities) -> Self {
        let size = TerminalSize::new(80, 24);
        Self {
            outbound,
            queued_bytes: 0,
            size,
            render_state: RenderState::new(size.cols, size.rows),
            attached: false,
            blocked: false,
            full_render: true,
            scheduler: FrameScheduler::new(),
            consumed_generations: BTreeMap::new(),
            structure: None,
            capabilities: RenderCapabilities {
                scroll_region: capabilities.contains(TerminalCapabilities::SCROLL_REGION),
            },
            scroll_offsets: BTreeMap::new(),
            copy_mode: None,
            control: None,
        }
    }

    fn can_enqueue(&self, bytes: usize) -> bool {
        bytes <= CLIENT_MAX_FRAME_BYTES
            && (self
                .queued_bytes
                .checked_add(bytes)
                .is_some_and(|queued| queued <= CLIENT_OUTPUT_BYTES)
                || self.queued_bytes == 0)
    }

    fn try_enqueue(&mut self, outbound: Outbound) -> Result<(), TrySendError<Outbound>> {
        let bytes = outbound.wire_len();
        if !self.can_enqueue(bytes) {
            return Err(TrySendError::Full(outbound));
        }
        self.outbound.try_send(outbound)?;
        self.queued_bytes += bytes;
        Ok(())
    }

    fn drained(&mut self, bytes: usize) {
        self.queued_bytes = self.queued_bytes.saturating_sub(bytes);
    }

    fn request_render(&mut self, now: Instant, cause: RenderCause) {
        if !self.blocked {
            self.scheduler.request(now, cause);
        }
    }

    fn request_immediate_render(&mut self, now: Instant) {
        self.scheduler.request(now, RenderCause::Structural);
    }
}

impl Outbound {
    fn wire_len(&self) -> usize {
        match self {
            Self::Message(message) | Self::Shutdown(message) => message.wire_len(),
        }
    }
}

struct ServerOwner {
    runtime: Runtime,
    clients: BTreeMap<ClientId, ClientView>,
    pending_pastes: VecDeque<PendingPaste>,
    started_at: Instant,
    shutdown: ShutdownState,
    shutdown_tx: Option<oneshot::Sender<()>>,
}

struct PendingPaste {
    pane: PaneId,
    bytes: Arc<[u8]>,
    offset: usize,
    prefix_sent: bool,
    suffix_sent: bool,
    bracketed: bool,
}

impl PendingPaste {
    fn next_chunk(&mut self) -> (Vec<u8>, bool) {
        let remaining = self.bytes.len().saturating_sub(self.offset);
        let payload_len = remaining.min(PASTE_CHUNK_BYTES);
        let mut chunk = Vec::with_capacity(payload_len + 12);
        if self.bracketed && !self.prefix_sent {
            chunk.extend_from_slice(b"\x1b[200~");
            self.prefix_sent = true;
        }
        let end = self.offset + payload_len;
        chunk.extend_from_slice(&self.bytes[self.offset..end]);
        self.offset = end;
        if self.bracketed && self.offset == self.bytes.len() && !self.suffix_sent {
            chunk.extend_from_slice(b"\x1b[201~");
            self.suffix_sent = true;
        }
        let complete = self.offset == self.bytes.len() && (!self.bracketed || self.suffix_sent);
        (chunk, complete)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ShutdownState {
    Running,
    Draining,
    StopRequested,
}

impl ServerOwner {
    fn new(
        config: WmuxConfig,
        pty_backend: Box<dyn PtyBackend>,
        job_backend: Box<dyn JobBackend>,
        shutdown_tx: oneshot::Sender<()>,
    ) -> Self {
        let mut owner = Self {
            runtime: Runtime::with_backends(config, pty_backend, job_backend),
            clients: BTreeMap::new(),
            pending_pastes: VecDeque::new(),
            started_at: Instant::now(),
            shutdown: ShutdownState::Running,
            shutdown_tx: Some(shutdown_tx),
        };
        owner.enqueue_startup_config();
        owner
    }

    #[cfg(test)]
    fn new_test(config: WmuxConfig) -> Self {
        let mut owner = Self {
            runtime: Runtime::with_config(config),
            clients: BTreeMap::new(),
            pending_pastes: VecDeque::new(),
            started_at: Instant::now(),
            shutdown: ShutdownState::Running,
            shutdown_tx: None,
        };
        owner.enqueue_startup_config();
        owner
    }

    fn enqueue_startup_config(&mut self) {
        let source = self.runtime.config.command_source().text();
        if source.trim().is_empty() {
            return;
        }
        match parse_command_text(source) {
            Ok(commands) => {
                if let Err(error) =
                    self.runtime
                        .queue
                        .push_list(ClientId::new(0), commands, CommandSource::Config)
                {
                    eprintln!("wmux startup config queue error: {error}");
                }
            }
            Err(error) => eprintln!(
                "wmux config command error at {}: {error}",
                config_path().display()
            ),
        }
    }

    fn run(mut self, owner_rx: Receiver<OwnerMessage>) {
        'owner: loop {
            let mut did_work = false;
            for _ in 0..CONTROL_EVENTS_PER_TURN {
                match owner_rx.try_recv() {
                    Ok(message) => {
                        did_work = true;
                        self.handle_owner_message(message);
                    }
                    Err(StdTryRecvError::Empty) => break,
                    Err(StdTryRecvError::Disconnected) => break 'owner,
                }
            }

            if self.should_stop() {
                break;
            }
            if self.is_shutting_down() {
                match owner_rx.recv() {
                    Ok(message) => self.handle_owner_message(message),
                    Err(_) => break,
                }
                continue;
            }

            if self.process_command_queue(COMMANDS_PER_TURN) {
                did_work = true;
            }
            if self.process_job_events(CONTROL_EVENTS_PER_TURN) {
                did_work = true;
            }
            if self.process_pending_pastes() {
                did_work = true;
            }

            let output = self.runtime.process_output_round(OutputBudget::default());
            for (pane, bytes) in self.runtime.take_published_output() {
                self.publish_control_output(pane, &bytes);
            }
            self.anchor_scrolled_views();
            if output.changed {
                if output.publishable {
                    self.request_all_attached_renders(if output.synchronized_commit {
                        RenderCause::SynchronizedCommit
                    } else {
                        RenderCause::Output
                    });
                }
                did_work = true;
            }
            let now = Instant::now();
            if self.runtime.expire_synchronized_output(now)
                || self.runtime.expire_resize_repaint_holds(now)
            {
                self.request_all_attached_renders(RenderCause::SynchronizedCommit);
                did_work = true;
            }
            if self.render_due_clients(now) {
                did_work = true;
            }

            if !did_work {
                if let Some(deadline) = self.next_deadline() {
                    match owner_rx.recv_timeout(deadline.saturating_duration_since(Instant::now()))
                    {
                        Ok(message) => self.handle_owner_message(message),
                        Err(mpsc::RecvTimeoutError::Timeout) => {}
                        Err(mpsc::RecvTimeoutError::Disconnected) => break 'owner,
                    }
                } else {
                    match owner_rx.recv() {
                        Ok(message) => self.handle_owner_message(message),
                        Err(_) => break 'owner,
                    }
                }
            }
        }
        self.runtime.shutdown_platform_panes();
    }

    fn is_shutting_down(&self) -> bool {
        self.shutdown != ShutdownState::Running
    }

    fn should_stop(&self) -> bool {
        self.shutdown == ShutdownState::StopRequested
            || (self.shutdown == ShutdownState::Draining && self.clients.is_empty())
    }

    fn next_deadline(&self) -> Option<Instant> {
        let render = self
            .clients
            .values()
            .filter(|view| !view.blocked)
            .filter_map(|view| view.scheduler.deadline)
            .min();
        let synchronized_output = self
            .runtime
            .sync_started_at
            .values()
            .map(|started| *started + SYNC_OUTPUT_TIMEOUT)
            .min();
        let resize = self
            .runtime
            .resize_repaint_holds
            .iter()
            .map(|(pane, hold)| {
                if self
                    .runtime
                    .state
                    .pane(*pane)
                    .is_some_and(|pane| pane.screen.synchronized_output())
                {
                    hold.max_until
                } else {
                    hold.quiet_until
                }
            })
            .min();
        [render, synchronized_output, resize]
            .into_iter()
            .flatten()
            .min()
    }

    fn handle_owner_message(&mut self, message: OwnerMessage) {
        match message {
            OwnerMessage::Register {
                outbound,
                capabilities,
                reply,
            } => {
                if self.is_shutting_down() {
                    return;
                }
                let client = self.runtime.state.add_client();
                self.clients
                    .insert(client, ClientView::new(outbound, capabilities));
                let _ = reply.send(client);
            }
            OwnerMessage::Event(event) => {
                if self.is_shutting_down() {
                    return;
                }
                if let Err(error) = self.handle_event(event) {
                    eprintln!("server event error: {error}");
                }
            }
            OwnerMessage::Commands {
                client,
                commands,
                source,
            } => {
                if !self.is_shutting_down() {
                    self.enqueue_command_list(client, commands, source);
                }
            }
            OwnerMessage::InvalidCommand { client, message } => {
                if !self.is_shutting_down() {
                    self.send_critical(client, Message::CommandErr(message));
                }
            }
            OwnerMessage::EnterControl(client) => self.enter_control(client),
            OwnerMessage::ControlCommand {
                client,
                sequence,
                command,
            } => self.handle_control_command(client, sequence, command),
            OwnerMessage::Disconnect(client) => self.disconnect_client(client, true),
            OwnerMessage::OutboundDrained { client, bytes } => {
                if let Some(view) = self.clients.get_mut(&client) {
                    view.drained(bytes);
                    if view.queued_bytes == 0 {
                        view.blocked = false;
                        view.request_immediate_render(Instant::now());
                    }
                }
            }
            OwnerMessage::PlatformReady => {}
            OwnerMessage::Stop => self.shutdown = ShutdownState::StopRequested,
        }
    }

    fn enter_control(&mut self, client: ClientId) {
        let Some(view) = self.clients.get_mut(&client) else {
            return;
        };
        if view.control.is_some() {
            self.send_control_record(
                client,
                ControlRecord::Error {
                    sequence: 0,
                    message: "client is already in control mode".to_string(),
                },
            );
            return;
        }
        view.control = Some(ControlClient {
            next_sequence: 1,
            paused: false,
            pause_sent: false,
            subscribed_output: true,
        });
        self.send_control_record(client, ControlRecord::Ready);
    }

    fn handle_control_command(&mut self, client: ClientId, sequence: u64, command: String) {
        let expected = self
            .clients
            .get(&client)
            .and_then(|view| view.control.as_ref())
            .map(|control| control.next_sequence);
        let Some(expected) = expected else {
            self.send_critical(
                client,
                Message::CommandErr(
                    "enter control mode before sending control commands".to_string(),
                ),
            );
            return;
        };
        self.send_control_record(client, ControlRecord::Begin { sequence });
        if sequence != expected {
            self.send_control_record(
                client,
                ControlRecord::Error {
                    sequence,
                    message: format!(
                        "out-of-order control sequence {sequence}; expected {expected}"
                    ),
                },
            );
            return;
        }
        let Some(next_sequence) = sequence.checked_add(1) else {
            self.send_control_record(
                client,
                ControlRecord::Error {
                    sequence,
                    message: "control sequence space exhausted".to_string(),
                },
            );
            return;
        };
        if let Some(control) = self
            .clients
            .get_mut(&client)
            .and_then(|view| view.control.as_mut())
        {
            control.next_sequence = next_sequence;
        }
        let commands = match parse_command_text(&command) {
            Ok(commands) => commands,
            Err(error) => {
                self.send_control_record(
                    client,
                    ControlRecord::Error {
                        sequence,
                        message: error.to_string(),
                    },
                );
                return;
            }
        };
        if let Err(error) =
            self.runtime
                .queue
                .push_list(client, commands, CommandSource::Control { sequence })
        {
            self.send_control_record(
                client,
                ControlRecord::Error {
                    sequence,
                    message: error.to_string(),
                },
            );
        }
    }

    fn handle_event(&mut self, event: ServerEvent) -> io::Result<()> {
        match event {
            ServerEvent::PtyOutput { pane, bytes } => {
                self.publish_control_output(pane, &bytes);
                let output = self.runtime.apply_pty_output(pane, &bytes);
                self.anchor_scrolled_views();
                if output.publishable {
                    let cause = if output.synchronized_commit {
                        RenderCause::SynchronizedCommit
                    } else {
                        RenderCause::Output
                    };
                    self.request_all_attached_renders(cause);
                }
            }
            ServerEvent::PtyExited { pane, exit_code } => {
                if self.runtime.apply_pty_exit(pane, exit_code) {
                    self.request_all_attached_renders(RenderCause::Structural);
                }
            }
            ServerEvent::ClientInput { client, input } => {
                if let Some(view) = self.clients.get_mut(&client) {
                    view.scheduler.note_input(Instant::now());
                }
                if self
                    .clients
                    .get(&client)
                    .is_some_and(|view| view.copy_mode.is_some())
                {
                    if let ClientInput::Bytes(bytes) = input {
                        self.handle_copy_mode_key(client, bytes);
                    }
                } else if let Some(pane) = self.active_pane_for_client(client) {
                    self.runtime.write_pane_input(pane, input)?;
                }
            }
            ServerEvent::ClientKey { client, event } => {
                let now_ms = self
                    .started_at
                    .elapsed()
                    .as_millis()
                    .min(u128::from(u64::MAX)) as u64;
                self.handle_client_key_at(client, event, now_ms)?;
            }
            ServerEvent::ClientMouse { client, event } => {
                self.handle_client_mouse(client, event)?;
            }
            ServerEvent::ClientResize { client, cols, rows } => {
                let size = TerminalSize::new(cols, rows);
                if let Some(view) = self.clients.get_mut(&client) {
                    view.size = size;
                }
                self.commit_layout_transaction(client, size)?;
                if let Some(view) = self.clients.get_mut(&client) {
                    view.request_immediate_render(Instant::now());
                }
            }
            ServerEvent::ClientWritable { client } => {
                if let Some(view) = self.clients.get_mut(&client) {
                    if view.blocked && view.queued_bytes == 0 {
                        view.blocked = false;
                        view.request_immediate_render(Instant::now());
                    }
                }
            }
            ServerEvent::Command { client, command } => self.enqueue_command_list(
                client,
                CommandList::new(vec![command]).expect("one command is within the list bound"),
                CommandSource::ClientRequest,
            ),
            ServerEvent::Timer { .. } => {}
        }
        Ok(())
    }

    fn enqueue_command_list(
        &mut self,
        client: ClientId,
        commands: CommandList,
        source: CommandSource,
    ) {
        if let Err(error) = self.runtime.queue.push_list(client, commands, source) {
            if matches!(source, CommandSource::ClientRequest) {
                self.send_critical(client, Message::CommandErr(error.to_string()));
            }
        }
    }

    fn handle_client_key_at(
        &mut self,
        client: ClientId,
        event: KeyEvent,
        now_ms: u64,
    ) -> io::Result<()> {
        if let Some(view) = self.clients.get_mut(&client) {
            view.scheduler.note_input(Instant::now());
        }
        let mode = if self
            .clients
            .get(&client)
            .is_some_and(|view| view.copy_mode.is_some())
        {
            InputMode::CopyMode
        } else {
            InputMode::Normal
        };
        let prompt_before = self
            .runtime
            .state
            .clients
            .get(&client)
            .and_then(client_prompt);
        let had_confirmation = self
            .runtime
            .state
            .clients
            .get(&client)
            .is_some_and(|client| client.confirmation.is_some());
        let route = route_key(&mut self.runtime.state, client, mode, event, now_ms);
        let prompt_after = self
            .runtime
            .state
            .clients
            .get(&client)
            .and_then(client_prompt);
        if prompt_before != prompt_after {
            if let Some(view) = self.clients.get_mut(&client) {
                if prompt_before.is_some() && prompt_after.is_none() {
                    view.full_render = true;
                }
                if had_confirmation
                    && self
                        .runtime
                        .state
                        .clients
                        .get(&client)
                        .is_some_and(|client| client.confirmation.is_none())
                {
                    view.full_render = true;
                    view.render_state.invalidate();
                }
                view.request_immediate_render(Instant::now());
            }
        }
        match route {
            InputRoute::PaneBytes(bytes) => {
                if let Some(pane) = self.active_pane_for_client(client) {
                    self.runtime
                        .write_pane_input(pane, ClientInput::Bytes(bytes))?;
                }
            }
            InputRoute::Commands(commands) => {
                self.enqueue_command_list(client, commands, CommandSource::KeyBinding);
            }
            InputRoute::CopyModeKey(event) => {
                self.handle_copy_mode_key(client, event.raw);
            }
            InputRoute::Consumed => {}
        }
        Ok(())
    }

    #[cfg(test)]
    fn handle_wire_key_at(
        &mut self,
        client: ClientId,
        event: WireKeyEvent,
        now_ms: u64,
    ) -> io::Result<()> {
        let event = core_key_event(event).map_err(io::Error::other)?;
        self.handle_client_key_at(client, event, now_ms)
    }

    fn process_command_queue(&mut self, budget: usize) -> bool {
        let mut processed = false;
        for _ in 0..budget {
            let Some(queued) = self.runtime.queue.pop() else {
                break;
            };
            processed = true;
            let outcome = execute(&mut self.runtime.state, &queued);
            let mut terminal_response_sent = false;
            let mut effect_error = None;

            if outcome.ok {
                for effect in outcome.effects {
                    match self.apply_command_effect(&queued, effect, &outcome.message) {
                        Ok(sent) => terminal_response_sent |= sent,
                        Err(error) => {
                            effect_error = Some(error.to_string());
                            break;
                        }
                    }
                }
            }

            let result = match effect_error {
                Some(error) => Err(error),
                None if outcome.ok => Ok(outcome.message),
                None => Err(outcome.message),
            };
            if self.runtime.state.jobs.owns_sequence(queued.sequence) {
                continue;
            }
            let completion = self.runtime.queue.finish(queued, result);
            if !terminal_response_sent {
                if let Some(completion) = completion {
                    self.send_command_completion(completion);
                }
            }
            if self.is_shutting_down() {
                break;
            }
        }
        processed
    }

    fn process_job_events(&mut self, budget: usize) -> bool {
        let jobs = self.runtime.state.jobs.ids().collect::<Vec<_>>();
        let mut processed = false;
        let mut remaining = budget;
        for job in jobs {
            while remaining > 0 {
                let event = match self
                    .runtime
                    .job_backend
                    .try_next_event(PlatformJobId::new(job.raw()))
                {
                    Ok(event) => event,
                    Err(error) => {
                        eprintln!("shell job backend error for {}: {error}", job.raw());
                        break;
                    }
                };
                let Some(event) = event else { break };
                processed = true;
                remaining -= 1;
                match event {
                    JobEvent::Output { bytes, .. } => {
                        self.runtime.state.jobs.append_output(job, &bytes);
                    }
                    JobEvent::Exited { exit_code, .. } => {
                        self.runtime.state.jobs.mark_exited(job, exit_code);
                    }
                    JobEvent::BackendError { error, .. } => {
                        self.runtime.state.jobs.append_output(
                            job,
                            format!("wmux job backend error: {error}\n").as_bytes(),
                        );
                    }
                    JobEvent::Closed { .. } => self.finish_job(job),
                }
            }
            if remaining == 0 {
                break;
            }
        }
        processed
    }

    fn finish_job(&mut self, job_id: JobId) {
        let Some(job) = self.runtime.state.jobs.finish(job_id) else {
            return;
        };
        let exit_code = job.exit_code();
        let success = exit_code == Some(0);
        let mut output = String::from_utf8_lossy(job.output()).into_owned();
        if job.output_truncated() {
            output.push_str("\n[wmux: job output truncated at 1048576 bytes]");
        }
        self.publish_control_notification(ControlNotification::JobFinished {
            job: job_id,
            exit_code,
        });

        if let Some(owner) = job.owner {
            if let JobContinuation::IfShell { if_true, if_false } = &job.continuation {
                let branch = if success {
                    Some(if_true)
                } else {
                    if_false.as_ref()
                };
                if let Some(branch) = branch {
                    if let Err(error) = self
                        .runtime
                        .queue
                        .insert_after_active(&owner, branch.clone())
                    {
                        eprintln!("wmux if-shell continuation error: {error}");
                    }
                }
            }
            if let Err(error) = self.insert_hook_notification(
                &owner,
                wmux_core::HookEvent::JobFinished,
                wmux_core::OptionTarget::Server,
            ) {
                eprintln!("wmux job-finished hook error: {error}");
            }
            let result = match job.continuation {
                JobContinuation::RunShell if success => Ok(output),
                JobContinuation::RunShell => Err(job_failure(exit_code, &output)),
                JobContinuation::IfShell { .. } => Ok(String::new()),
            };
            if let Some(completion) = self.runtime.queue.finish(owner, result) {
                self.send_command_completion(completion);
            }
            return;
        }

        if let JobContinuation::IfShell { if_true, if_false } = job.continuation {
            let branch = if success { Some(if_true) } else { if_false };
            if let Some(branch) = branch {
                if let Err(error) = self.runtime.queue.push_list(job.client, branch, job.source) {
                    eprintln!("wmux background if-shell continuation error: {error}");
                }
            }
        } else if !success {
            eprintln!("{}", job_failure(exit_code, &output));
        }
        self.enqueue_hook_notification(
            job.source,
            wmux_core::HookEvent::JobFinished,
            wmux_core::OptionTarget::Server,
        );
    }

    fn enqueue_hook_notification(
        &mut self,
        source: CommandSource,
        event: wmux_core::HookEvent,
        target: wmux_core::OptionTarget,
    ) {
        let depth = match source {
            CommandSource::Hook { depth } if depth >= wmux_core::MAX_HOOK_DEPTH => return,
            CommandSource::Hook { depth } => depth + 1,
            _ => 1,
        };
        let path = self.runtime.state.option_path(target);
        for commands in self.runtime.state.hooks.resolve(&path, event).to_vec() {
            if let Err(error) = self.runtime.queue.push_list(
                ClientId::new(0),
                commands,
                CommandSource::Hook { depth },
            ) {
                eprintln!("wmux hook queue error: {error}");
            }
        }
    }

    fn process_pending_pastes(&mut self) -> bool {
        let candidates = self.pending_pastes.len().min(PASTES_PER_TURN);
        let mut processed = false;
        let mut serviced_panes = BTreeSet::new();
        let mut next = VecDeque::with_capacity(self.pending_pastes.len());
        for _ in 0..candidates {
            let Some(mut paste) = self.pending_pastes.pop_front() else {
                break;
            };
            if !serviced_panes.insert(paste.pane) {
                next.push_back(paste);
                continue;
            }
            processed = true;
            if self
                .runtime
                .state
                .pane(paste.pane)
                .is_none_or(|pane| pane.dead)
            {
                continue;
            }
            let (chunk, complete) = paste.next_chunk();
            if !chunk.is_empty() {
                if let Err(error) = self
                    .runtime
                    .write_pane_input(paste.pane, ClientInput::Bytes(chunk))
                {
                    eprintln!(
                        "pending paste write error for pane {}: {error}",
                        paste.pane.raw()
                    );
                    continue;
                }
            }
            if !complete {
                next.push_back(paste);
            }
        }
        next.append(&mut self.pending_pastes);
        self.pending_pastes = next;
        processed
    }

    fn apply_command_effect(
        &mut self,
        queued: &QueuedCommand,
        effect: CommandEffect,
        success_message: &str,
    ) -> io::Result<bool> {
        match effect {
            CommandEffect::EnsurePane { pane } => {
                let size = self.pane_size(pane).unwrap_or(TerminalSize::new(80, 24));
                self.runtime.ensure_platform_pane(pane, size)?;
                self.runtime.apply_pending_pane_resizes()?;
                Ok(false)
            }
            CommandEffect::PaneInput { pane, bytes } => {
                self.runtime
                    .write_pane_input(pane, ClientInput::Bytes(bytes))?;
                Ok(false)
            }
            CommandEffect::EnterCopyMode { client } => {
                self.enter_copy_mode(client).map_err(io::Error::other)?;
                Ok(false)
            }
            CommandEffect::RefreshClient { client } => {
                if let Some(control) = self
                    .clients
                    .get_mut(&client)
                    .and_then(|view| view.control.as_mut())
                {
                    control.paused = false;
                    control.pause_sent = false;
                    control.subscribed_output = true;
                    return Ok(false);
                }
                let destroyed = self.client_session_is_destroyed(client);
                if destroyed && matches!(queued.source, CommandSource::ClientRequest) {
                    self.send_critical(client, Message::CommandOk(success_message.to_string()));
                }
                if self
                    .close_clients_with_destroyed_sessions()
                    .contains(&client)
                {
                    return Ok(destroyed);
                }

                let should_attach = self
                    .runtime
                    .state
                    .clients
                    .get(&client)
                    .is_some_and(|client| client.attached_session.is_some());
                let size = self
                    .clients
                    .get(&client)
                    .map_or(TerminalSize::new(80, 24), |view| view.size);
                if let Some(view) = self.clients.get_mut(&client) {
                    if should_attach && !view.attached {
                        view.attached = true;
                    }
                    view.full_render = true;
                    view.render_state.invalidate();
                    view.structure = None;
                }
                if self.clients.get(&client).is_some_and(|view| view.attached) {
                    self.commit_layout_transaction(client, size)?;
                    if let Some(view) = self.clients.get_mut(&client) {
                        view.request_render(Instant::now(), RenderCause::Structural);
                    }
                }
                Ok(false)
            }
            CommandEffect::Confirm {
                client,
                prompt,
                commands,
            } => {
                let mut installed = false;
                if let Some(client) = self.runtime.state.clients.get_mut(&client) {
                    client.prompt = None;
                    client.confirmation = Some(wmux_core::ConfirmationState { prompt, commands });
                    installed = true;
                }
                if installed {
                    if let Some(view) = self.clients.get_mut(&client) {
                        view.full_render = true;
                        view.render_state.invalidate();
                        view.request_immediate_render(Instant::now());
                    }
                }
                Ok(false)
            }
            CommandEffect::Prompt {
                client,
                prompt,
                input,
                template,
            } => {
                let mut installed = false;
                if let Some(client) = self.runtime.state.clients.get_mut(&client) {
                    client.confirmation = None;
                    client.prompt = Some(wmux_core::PromptState::new(prompt, input, template));
                    installed = true;
                }
                if installed {
                    if let Some(view) = self.clients.get_mut(&client) {
                        view.request_immediate_render(Instant::now());
                    }
                }
                Ok(false)
            }
            CommandEffect::DetachClient { client } => {
                if matches!(queued.source, CommandSource::ClientRequest) {
                    self.send_critical(client, Message::CommandOk(success_message.to_string()));
                } else if let CommandSource::Control { sequence } = queued.source {
                    self.send_control_record(
                        client,
                        ControlRecord::End {
                            sequence,
                            output: success_message.to_string(),
                        },
                    );
                }
                self.disconnect_client(client, false);
                Ok(true)
            }
            CommandEffect::Shutdown { requester } => {
                let response = match queued.source {
                    CommandSource::ClientRequest => Message::CommandOk(success_message.to_string()),
                    CommandSource::Control { sequence } => {
                        Message::ControlRecord(ControlRecord::End {
                            sequence,
                            output: success_message.to_string(),
                        })
                    }
                    _ => Message::Shutdown,
                };
                self.begin_shutdown(requester, response);
                Ok(true)
            }
            CommandEffect::SourceFile {
                path,
                depth,
                ancestors,
            } => {
                let commands = load_source_commands(&path, depth, &ancestors)?;
                self.runtime
                    .queue
                    .insert_after_active(queued, commands)
                    .map_err(io::Error::other)?;
                Ok(false)
            }
            CommandEffect::ReadBufferFile { path, name } => {
                let bytes = read_buffer_file(&path)?;
                match name {
                    Some(name) => self
                        .runtime
                        .state
                        .paste_buffers
                        .set_named(name, bytes)
                        .map_err(io::Error::other)?,
                    None => {
                        self.runtime
                            .state
                            .paste_buffers
                            .add_automatic(bytes)
                            .map_err(io::Error::other)?;
                    }
                }
                self.insert_hook_notification(
                    queued,
                    wmux_core::HookEvent::BufferChanged,
                    wmux_core::OptionTarget::Server,
                )?;
                Ok(false)
            }
            CommandEffect::WriteBufferFile {
                path,
                bytes,
                append,
            } => {
                write_buffer_file(&path, &bytes, append)?;
                Ok(false)
            }
            CommandEffect::Clipboard { client, bytes } => {
                self.send_critical(client, Message::Clipboard(bytes.to_vec()));
                Ok(false)
            }
            CommandEffect::PastePane {
                pane,
                bytes,
                bracketed,
            } => {
                let bracketed = bracketed
                    && self
                        .runtime
                        .state
                        .pane(pane)
                        .is_some_and(|pane| pane.screen.bracketed_paste());
                self.pending_pastes.push_back(PendingPaste {
                    pane,
                    bytes,
                    offset: 0,
                    prefix_sent: false,
                    suffix_sent: false,
                    bracketed,
                });
                Ok(false)
            }
            CommandEffect::StartJob {
                command,
                background,
                continuation,
            } => {
                let job = self
                    .runtime
                    .state
                    .jobs
                    .start(command.clone(), background, continuation, queued.clone())
                    .map_err(io::Error::other)?;
                let environment = self
                    .runtime
                    .config
                    .pane_environment(job.raw())
                    .into_iter()
                    .map(|(key, value)| (key.into(), value.into()))
                    .collect();
                let request = JobRequest::Spawn(SpawnJob {
                    job: PlatformJobId::new(job.raw()),
                    command,
                    cwd: std::env::current_dir().ok(),
                    environment,
                });
                if let Err(error) = self.runtime.job_backend.submit(request) {
                    self.runtime.state.jobs.finish(job);
                    return Err(error.into_io());
                }
                Ok(false)
            }
            CommandEffect::Notify { event, target } => {
                self.insert_hook_notification(queued, event, target)?;
                Ok(false)
            }
        }
    }

    fn insert_hook_notification(
        &mut self,
        queued: &QueuedCommand,
        event: wmux_core::HookEvent,
        target: wmux_core::OptionTarget,
    ) -> io::Result<()> {
        if let Some(notification) = control_notification(event, target) {
            self.publish_control_notification(notification);
        }
        let depth = match queued.source {
            CommandSource::Hook { depth } if depth >= wmux_core::MAX_HOOK_DEPTH => return Ok(()),
            CommandSource::Hook { depth } => depth + 1,
            _ => 1,
        };
        let path = self.runtime.state.option_path(target);
        let registrations = self.runtime.state.hooks.resolve(&path, event).to_vec();
        for commands in registrations {
            if event == wmux_core::HookEvent::ClientDetached {
                self.runtime
                    .queue
                    .push_list(ClientId::new(0), commands, CommandSource::Hook { depth })
                    .map_err(io::Error::other)?;
            } else {
                self.runtime
                    .queue
                    .insert_after_active_as(queued, commands, CommandSource::Hook { depth })
                    .map_err(io::Error::other)?;
            }
        }
        Ok(())
    }

    fn send_command_completion(&mut self, completion: wmux_core::CommandCompletion) {
        if let CommandSource::Control { sequence } = completion.source {
            match completion.result {
                Ok(output) if output.len() <= wmux_core::MAX_CONTROL_TEXT_BYTES => {
                    self.send_control_record(
                        completion.client,
                        ControlRecord::End { sequence, output },
                    );
                }
                Ok(_) => self.send_control_record(
                    completion.client,
                    ControlRecord::Error {
                        sequence,
                        message: "control command output exceeds 1048576 bytes".to_string(),
                    },
                ),
                Err(message) => self.send_control_record(
                    completion.client,
                    ControlRecord::Error {
                        sequence,
                        message: bounded_control_text(message),
                    },
                ),
            }
            return;
        }
        if matches!(completion.source, CommandSource::Config) {
            if let Err(message) = completion.result {
                eprintln!("wmux config command error: {message}");
            }
            return;
        }
        if matches!(completion.source, CommandSource::Hook { .. }) {
            if let Err(message) = completion.result {
                eprintln!("wmux hook command error: {message}");
            }
            return;
        }
        match completion.result {
            Ok(message) if completion.requires_reply() => {
                self.send_critical(completion.client, Message::CommandOk(message));
            }
            Ok(_) => {}
            Err(message) => {
                self.send_critical(completion.client, Message::CommandErr(message));
            }
        }
    }

    fn active_pane_for_client(&self, client: ClientId) -> Option<PaneId> {
        self.runtime
            .state
            .active_window_and_pane_for_client(client)
            .map(|(_, _, pane)| pane)
    }

    fn anchor_scrolled_views(&mut self) {
        let growth = self.runtime.take_history_growth();
        if growth.is_empty() {
            return;
        }
        let now = Instant::now();
        for (pane, added) in growth {
            for view in self.clients.values_mut() {
                let Some(offset) = view.scroll_offsets.get_mut(&pane) else {
                    continue;
                };
                *offset = offset.saturating_add(added as usize);
                view.full_render = true;
                view.request_render(now, RenderCause::Output);
            }
            for view in self.clients.values_mut() {
                if let Some(mode) = view.copy_mode.as_mut().filter(|mode| mode.pane == pane) {
                    mode.anchor_output(added as usize);
                    view.full_render = true;
                    view.request_render(now, RenderCause::Output);
                }
            }
        }
    }

    fn enter_copy_mode(&mut self, client: ClientId) -> Result<(), String> {
        let pane_id = self
            .active_pane_for_client(client)
            .ok_or_else(|| "no active pane".to_string())?;
        let (rows, cols, cursor_row, cursor_col) = self
            .runtime
            .state
            .pane(pane_id)
            .map(|pane| {
                let (row, column) = pane.screen.render_cursor();
                (pane.rect.rows.max(1), pane.rect.cols.max(1), row, column)
            })
            .ok_or_else(|| "no active pane".to_string())?;
        let (total, history) = self
            .runtime
            .state
            .pane_mut(pane_id)
            .map(|pane| {
                let (lines, history) = pane.screen.copy_lines(cols);
                (lines.len(), history)
            })
            .ok_or_else(|| "no active pane".to_string())?;
        let mode = CopyMode::new(
            pane_id,
            history.saturating_add(usize::from(cursor_row)),
            cursor_col,
            total,
            rows,
        );
        let view = self
            .clients
            .get_mut(&client)
            .ok_or_else(|| "client not attached".to_string())?;
        view.scroll_offsets.remove(&pane_id);
        view.copy_mode = Some(mode);
        view.full_render = true;
        view.request_immediate_render(Instant::now());
        Ok(())
    }

    fn handle_copy_mode_key(&mut self, client: ClientId, bytes: Vec<u8>) {
        let Some(mut mode) = self
            .clients
            .get_mut(&client)
            .and_then(|view| view.copy_mode.take())
        else {
            return;
        };
        let pane_id = mode.pane;
        let Some((lines, rows)) = self.runtime.state.pane_mut(pane_id).map(|pane| {
            let rows = pane.rect.rows.max(1);
            let (lines, _) = pane.screen.copy_lines(pane.rect.cols.max(1));
            (lines, rows)
        }) else {
            return;
        };
        let result = mode.handle_key(&bytes, &lines, rows);
        let clipboard = match result {
            CopyModeResult::Continue => None,
            CopyModeResult::Cancel => Some(None),
            CopyModeResult::Copy(bytes) => Some(Some(bytes)),
        };
        if let Some(view) = self.clients.get_mut(&client) {
            if clipboard.is_none() {
                view.copy_mode = Some(mode);
            } else {
                view.scroll_offsets.remove(&pane_id);
            }
            view.full_render = true;
            view.request_immediate_render(Instant::now());
        }
        if let Some(Some(bytes)) = clipboard {
            self.store_copy_and_update_clipboard(client, bytes);
        }
    }

    fn store_copy_and_update_clipboard(&mut self, client: ClientId, bytes: Vec<u8>) {
        if let Err(error) = self
            .runtime
            .state
            .paste_buffers
            .add_automatic(bytes.clone())
        {
            eprintln!("copy-mode buffer error: {error}");
        }
        let clipboard_enabled = matches!(
            self.runtime
                .state
                .option(wmux_core::OptionTarget::Client(client), "set-clipboard"),
            Ok(wmux_core::OptionValue::Flag(true))
        );
        if clipboard_enabled {
            self.send_critical(client, Message::Clipboard(bytes));
        }
    }

    fn handle_client_mouse(&mut self, client: ClientId, event: MouseEvent) -> io::Result<()> {
        let Some(view) = self.clients.get(&client) else {
            return Ok(());
        };
        let Some((session, _, _)) = self.runtime.state.active_window_and_pane_for_client(client)
        else {
            return Ok(());
        };
        let Some(structure) =
            build_window_structure(&self.runtime.state, session, view.size.cols, view.size.rows)
        else {
            return Ok(());
        };
        let Some((pane, column, row)) = structure.pane_at(event.column, event.row) else {
            return Ok(());
        };

        if self
            .clients
            .get(&client)
            .and_then(|view| view.copy_mode.as_ref())
            .is_some_and(|mode| mode.pane == pane)
        {
            let copy_lines = self
                .runtime
                .state
                .pane_mut(pane)
                .map(|pane| pane.screen.copy_lines(pane.rect.cols.max(1)).0)
                .unwrap_or_default();
            let total = copy_lines.len();
            let rows = self
                .runtime
                .state
                .pane(pane)
                .map_or(1, |pane| pane.rect.rows.max(1));
            let mut copied = None;
            if let Some(view) = self.clients.get_mut(&client) {
                if let Some(mode) = view.copy_mode.as_mut() {
                    match event.kind {
                        MouseEventKind::ScrollUp => mode.scroll(-5, total, rows),
                        MouseEventKind::ScrollDown => mode.scroll(5, total, rows),
                        MouseEventKind::Down if event.button == MouseButton::Left => {
                            mode.place_cursor(row, column, total, rows, true);
                        }
                        MouseEventKind::Drag if event.button == MouseButton::Left => {
                            mode.place_cursor(row, column, total, rows, true);
                        }
                        MouseEventKind::Up if event.button == MouseButton::Left => {
                            let bytes = mode.selected_text(&copy_lines).into_bytes();
                            if !bytes.is_empty() {
                                copied = Some(bytes);
                            }
                        }
                        _ => return Ok(()),
                    }
                    if copied.is_some() {
                        view.copy_mode = None;
                        view.scroll_offsets.remove(&pane);
                    }
                    view.full_render = true;
                    view.request_immediate_render(Instant::now());
                }
            }
            if let Some(bytes) = copied {
                self.store_copy_and_update_clipboard(client, bytes);
            }
            return Ok(());
        }

        let application_input = self
            .runtime
            .state
            .pane(pane)
            .and_then(|pane| pane.screen.encode_mouse(event, column, row));
        if let Some(bytes) = application_input {
            return self
                .runtime
                .write_pane_input(pane, ClientInput::Bytes(bytes));
        }

        let delta = match event.kind {
            MouseEventKind::ScrollUp => 5_isize,
            MouseEventKind::ScrollDown => -5_isize,
            _ => return Ok(()),
        };
        let cols = self
            .runtime
            .state
            .pane(pane)
            .map_or(1, |pane| pane.rect.cols);
        let max_offset = self
            .runtime
            .state
            .pane_mut(pane)
            .map_or(0, |pane| pane.screen.max_viewport_offset(cols));
        let Some(view) = self.clients.get_mut(&client) else {
            return Ok(());
        };
        let old = view.scroll_offsets.get(&pane).copied().unwrap_or(0);
        let new = if delta > 0 {
            old.saturating_add(delta as usize).min(max_offset)
        } else {
            old.saturating_sub(delta.unsigned_abs())
        };
        if new == old {
            return Ok(());
        }
        if new == 0 {
            view.scroll_offsets.remove(&pane);
        } else {
            view.scroll_offsets.insert(pane, new);
        }
        view.full_render = true;
        view.request_immediate_render(Instant::now());
        Ok(())
    }

    fn pane_size(&self, pane_id: PaneId) -> Option<TerminalSize> {
        let pane = self.runtime.state.pane(pane_id)?;
        Some(TerminalSize::new(pane.screen.cols(), pane.screen.rows()))
    }

    fn commit_layout_transaction(
        &mut self,
        client: ClientId,
        size: TerminalSize,
    ) -> io::Result<()> {
        self.runtime.resize_client_window(client, size);
        self.runtime.cleanup_platform_panes();
        self.runtime.ensure_platform_panes_for_client(client)?;
        self.runtime.apply_pending_pane_resizes()?;
        Ok(())
    }

    fn request_all_attached_renders(&mut self, cause: RenderCause) {
        let now = Instant::now();
        for view in self.clients.values_mut().filter(|view| view.attached) {
            view.request_render(now, cause);
        }
    }

    fn render_due_clients(&mut self, now: Instant) -> bool {
        let due = self
            .clients
            .iter()
            .filter_map(|(client, view)| {
                (view.attached
                    && !view.blocked
                    && view.queued_bytes == 0
                    && view
                        .scheduler
                        .deadline
                        .is_some_and(|deadline| now >= deadline))
                .then_some(*client)
            })
            .collect::<Vec<_>>();
        let mut rendered = false;
        for client in due {
            let Some(mut view) = self.clients.remove(&client) else {
                continue;
            };
            view.scheduler.deadline = None;
            let pane_generations = pane_generations_for_client(&self.runtime, client);
            let Some((session, _, _)) =
                self.runtime.state.active_window_and_pane_for_client(client)
            else {
                self.clients.insert(client, view);
                continue;
            };
            let overlay_active = self
                .runtime
                .state
                .clients
                .get(&client)
                .is_some_and(|client| client.confirmation.is_some() || client.prompt.is_some());
            let Some(structure) = build_window_structure(
                &self.runtime.state,
                session,
                view.size.cols,
                view.size.rows,
            ) else {
                self.clients.insert(client, view);
                continue;
            };
            if view.structure.as_ref() != Some(&structure) {
                view.full_render = true;
            }
            if !view.full_render
                && pane_generations.iter().any(|(pane_id, _)| {
                    let consumed = view.consumed_generations.get(pane_id).copied().unwrap_or(0);
                    self.runtime.state.pane(*pane_id).is_some_and(|pane| {
                        pane.screen
                            .damage_status_since(consumed)
                            .requires_full_redraw
                    })
                })
            {
                view.full_render = true;
                view.render_state.invalidate();
            }
            if !view.full_render
                && view.scroll_offsets.is_empty()
                && view.copy_mode.is_none()
                && !overlay_active
                && pane_generations.iter().all(|(pane, generation)| {
                    view.consumed_generations.get(pane) == Some(generation)
                })
            {
                self.clients.insert(client, view);
                continue;
            }
            let mut candidate = view.render_state.clone();
            let bytes = if view.full_render {
                render_full_for_client(
                    &mut self.runtime,
                    client,
                    view.size,
                    &mut candidate,
                    view.capabilities,
                    &mut view.scroll_offsets,
                    view.copy_mode.as_ref(),
                )
            } else if self.runtime.resize_repaint_holds.is_empty()
                && view.scroll_offsets.is_empty()
                && view.copy_mode.is_none()
                && !overlay_active
            {
                render_damage_from_structure(
                    &self.runtime.state,
                    &structure,
                    &view.consumed_generations,
                    &mut candidate,
                    view.capabilities,
                )
                .unwrap_or_else(|| {
                    render_diff_for_client(
                        &mut self.runtime,
                        client,
                        view.size,
                        &mut candidate,
                        view.capabilities,
                        &mut view.scroll_offsets,
                        view.copy_mode.as_ref(),
                    )
                })
            } else {
                render_diff_for_client(
                    &mut self.runtime,
                    client,
                    view.size,
                    &mut candidate,
                    view.capabilities,
                    &mut view.scroll_offsets,
                    view.copy_mode.as_ref(),
                )
            };
            let delivered = if bytes.is_empty() {
                true
            } else {
                match view.try_enqueue(Outbound::Message(Message::Output(bytes))) {
                    Ok(()) => {
                        rendered = true;
                        true
                    }
                    Err(TrySendError::Full(_)) => {
                        if view.queued_bytes == 0 {
                            self.runtime.state.remove_client(client);
                            continue;
                        }
                        view.blocked = true;
                        false
                    }
                    Err(TrySendError::Closed(_)) => {
                        self.runtime.state.remove_client(client);
                        continue;
                    }
                }
            };
            if delivered {
                view.render_state = candidate;
                view.structure = Some(structure);
                view.full_render = false;
                for (pane, generation) in pane_generations {
                    if self
                        .runtime
                        .state
                        .pane(pane)
                        .is_some_and(|pane| !pane.screen.synchronized_output())
                        && !self.runtime.resize_repaint_holds.contains_key(&pane)
                    {
                        view.consumed_generations.insert(pane, generation);
                    }
                }
                view.scheduler.published(now);
            }
            self.clients.insert(client, view);
        }
        rendered
    }

    fn send_critical(&mut self, client: ClientId, message: Message) {
        let result = self
            .clients
            .get_mut(&client)
            .map(|view| view.try_enqueue(Outbound::Message(message)));
        if matches!(result, Some(Err(_))) {
            self.disconnect_client(client, true);
        }
    }

    fn send_control_record(&mut self, client: ClientId, record: ControlRecord) {
        self.send_critical(client, Message::ControlRecord(record));
    }

    fn publish_control_notification(&mut self, notification: ControlNotification) {
        let clients = self
            .clients
            .iter()
            .filter_map(|(client, view)| view.control.is_some().then_some(*client))
            .collect::<Vec<_>>();
        for client in clients {
            self.send_control_record(client, ControlRecord::Notification(notification.clone()));
        }
    }

    fn publish_control_output(&mut self, pane: PaneId, bytes: &[u8]) {
        let clients = self
            .clients
            .iter()
            .filter_map(|(client, view)| {
                view.control
                    .as_ref()
                    .filter(|control| control.subscribed_output && !control.paused)
                    .and_then(|_| {
                        self.control_client_observes_pane(*client, pane)
                            .then_some(*client)
                    })
            })
            .collect::<Vec<_>>();
        for client in clients {
            for chunk in bytes.chunks(wmux_core::MAX_CONTROL_OUTPUT_BYTES) {
                let message = Message::ControlRecord(ControlRecord::Output {
                    pane,
                    bytes: chunk.to_vec(),
                });
                let should_pause = self.clients.get(&client).is_none_or(|view| {
                    view.outbound.capacity() <= 2
                        || view.queued_bytes.saturating_add(message.wire_len())
                            > CLIENT_OUTPUT_BYTES - CONTROL_OUTPUT_RESERVE_BYTES
                });
                let delivered = !should_pause
                    && self
                        .clients
                        .get_mut(&client)
                        .is_some_and(|view| view.try_enqueue(Outbound::Message(message)).is_ok());
                if !delivered {
                    self.pause_control_output(client, Some(pane));
                    break;
                }
            }
        }
    }

    fn control_client_observes_pane(&self, client: ClientId, pane: PaneId) -> bool {
        let Some(session_id) = self
            .runtime
            .state
            .clients
            .get(&client)
            .and_then(|client| client.attached_session)
        else {
            return false;
        };
        let Some(window) = self.runtime.state.panes.get(&pane).map(|pane| pane.window) else {
            return false;
        };
        self.runtime
            .state
            .sessions
            .get(&session_id)
            .is_some_and(|session| {
                session.winlinks.iter().any(|winlink| {
                    self.runtime
                        .state
                        .winlinks
                        .get(winlink)
                        .is_some_and(|winlink| winlink.window == window)
                })
            })
    }

    fn pause_control_output(&mut self, client: ClientId, pane: Option<PaneId>) {
        let send_pause = self
            .clients
            .get_mut(&client)
            .and_then(|view| view.control.as_mut())
            .is_some_and(|control| {
                control.paused = true;
                if control.pause_sent {
                    false
                } else {
                    control.pause_sent = true;
                    true
                }
            });
        if send_pause {
            if let Some(view) = self.clients.get_mut(&client) {
                let _ = view.try_enqueue(Outbound::Message(Message::ControlRecord(
                    ControlRecord::Pause { pane },
                )));
            }
        }
    }

    fn begin_shutdown(&mut self, requester: ClientId, response: Message) {
        if self.is_shutting_down() {
            return;
        }
        self.shutdown = ShutdownState::Draining;
        self.runtime.shutdown_platform_panes();
        for job in self.runtime.state.jobs.ids().collect::<Vec<_>>() {
            let _ = self.runtime.job_backend.submit(JobRequest::Terminate {
                job: PlatformJobId::new(job.raw()),
            });
        }

        let clients = self.clients.keys().copied().collect::<Vec<_>>();
        let mut failed = Vec::new();
        for client in clients {
            let message = if client == requester {
                response.clone()
            } else {
                Message::Shutdown
            };
            if self
                .clients
                .get_mut(&client)
                .is_none_or(|view| view.try_enqueue(Outbound::Shutdown(message)).is_err())
            {
                failed.push(client);
            }
        }
        for client in failed {
            self.disconnect_client(client, false);
        }
        if let Some(shutdown_tx) = self.shutdown_tx.take() {
            let _ = shutdown_tx.send(());
        }
    }

    fn close_clients_with_destroyed_sessions(&mut self) -> Vec<ClientId> {
        let clients = self
            .clients
            .keys()
            .filter_map(|client_id| {
                self.client_session_is_destroyed(*client_id)
                    .then_some(*client_id)
            })
            .collect::<Vec<_>>();
        if clients.is_empty() {
            return clients;
        }
        self.runtime.cleanup_platform_panes();
        for client_id in &clients {
            if let Some(mut view) = self.clients.remove(client_id) {
                let _ = view.try_enqueue(Outbound::Shutdown(Message::Shutdown));
            }
            self.runtime.queue.remove_client(*client_id);
            self.runtime.state.remove_client(*client_id);
        }
        clients
    }

    fn client_session_is_destroyed(&self, client: ClientId) -> bool {
        self.clients.get(&client).is_some_and(|view| view.attached)
            && self
                .runtime
                .state
                .clients
                .get(&client)
                .is_none_or(|client| client.attached_session.is_none())
    }

    fn disconnect_client(&mut self, client: ClientId, submit_detach: bool) {
        if !self.clients.contains_key(&client) {
            return;
        }
        if submit_detach {
            self.runtime.state.detach_client(client);
        }
        self.clients.remove(&client);
        self.runtime.queue.remove_client(client);
        self.runtime.state.remove_client(client);
    }
}

fn load_source_commands(
    requested: &Path,
    depth: u8,
    ancestors: &[PathBuf],
) -> io::Result<CommandList> {
    let resolved = if requested.is_absolute() {
        requested.to_path_buf()
    } else if let Some(parent) = ancestors.last().and_then(|path| path.parent()) {
        parent.join(requested)
    } else {
        std::env::current_dir()?.join(requested)
    };
    let canonical = fs::canonicalize(&resolved).map_err(|error| {
        io::Error::new(
            error.kind(),
            format!("source-file {}: {error}", resolved.display()),
        )
    })?;
    if ancestors.contains(&canonical) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            format!("source-file cycle at {}", canonical.display()),
        ));
    }
    let mut bytes = Vec::new();
    File::open(&canonical)?
        .take((MAX_SOURCE_FILE_BYTES + 1) as u64)
        .read_to_end(&mut bytes)?;
    if bytes.len() > MAX_SOURCE_FILE_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("source-file {} exceeds 1048576 bytes", canonical.display()),
        ));
    }
    let text = std::str::from_utf8(&bytes).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("source-file {} is not UTF-8: {error}", canonical.display()),
        )
    })?;
    let parsed = parse_command_text(text).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("source-file {}: {error}", canonical.display()),
        )
    })?;
    let mut nested_ancestors = ancestors.to_vec();
    nested_ancestors.push(canonical);
    parsed
        .with_source_context(
            depth.saturating_add(1),
            Arc::<[PathBuf]>::from(nested_ancestors),
        )
        .map_err(io::Error::other)
}

fn job_failure(exit_code: Option<u32>, output: &str) -> String {
    let status = exit_code.map_or_else(
        || "unknown status".to_string(),
        |code| format!("status {code}"),
    );
    let diagnostic = output.trim_end();
    if diagnostic.is_empty() {
        format!("shell command failed with {status}")
    } else {
        format!("shell command failed with {status}: {diagnostic}")
    }
}

fn bounded_control_text(mut text: String) -> String {
    if text.len() <= wmux_core::MAX_CONTROL_TEXT_BYTES {
        return text;
    }
    let mut end = wmux_core::MAX_CONTROL_TEXT_BYTES;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text.truncate(end);
    text
}

fn control_notification(
    event: wmux_core::HookEvent,
    target: wmux_core::OptionTarget,
) -> Option<ControlNotification> {
    use wmux_core::{HookEvent, OptionTarget};
    match (event, target) {
        (HookEvent::ClientAttached, OptionTarget::Client(client)) => {
            Some(ControlNotification::ClientAttached { client })
        }
        (HookEvent::ClientDetached, OptionTarget::Client(client)) => {
            Some(ControlNotification::ClientDetached { client })
        }
        (HookEvent::SessionCreated, OptionTarget::Session(session)) => {
            Some(ControlNotification::SessionCreated { session })
        }
        (HookEvent::SessionClosed, OptionTarget::Session(session)) => {
            Some(ControlNotification::SessionClosed { session })
        }
        (HookEvent::WindowCreated, OptionTarget::Window(window)) => {
            Some(ControlNotification::WindowCreated { window })
        }
        (HookEvent::WindowClosed, OptionTarget::Window(window)) => {
            Some(ControlNotification::WindowClosed { window })
        }
        (HookEvent::PaneCreated, OptionTarget::Pane(pane)) => {
            Some(ControlNotification::PaneCreated { pane })
        }
        (HookEvent::PaneClosed, OptionTarget::Pane(pane)) => {
            Some(ControlNotification::PaneClosed { pane })
        }
        (HookEvent::BufferChanged, _) => Some(ControlNotification::BufferChanged { name: None }),
        (HookEvent::BufferDeleted, _) => Some(ControlNotification::BufferDeleted { name: None }),
        (HookEvent::JobFinished, _) => None,
        _ => None,
    }
}

fn read_buffer_file(path: &Path) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    File::open(path)
        .map_err(|error| path_io_error("load-buffer", path, error))?
        .take((MAX_BUFFER_FILE_BYTES + 1) as u64)
        .read_to_end(&mut bytes)
        .map_err(|error| path_io_error("load-buffer", path, error))?;
    if bytes.len() > MAX_BUFFER_FILE_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!(
                "load-buffer {} exceeds {MAX_BUFFER_FILE_BYTES} bytes",
                path.display()
            ),
        ));
    }
    Ok(bytes)
}

fn write_buffer_file(path: &Path, bytes: &[u8], append: bool) -> io::Result<()> {
    let mut options = OpenOptions::new();
    options.create(true).write(true);
    if append {
        options.append(true);
    } else {
        options.truncate(true);
    }
    let mut file = options
        .open(path)
        .map_err(|error| path_io_error("save-buffer", path, error))?;
    file.write_all(bytes)
        .map_err(|error| path_io_error("save-buffer", path, error))?;
    file.flush()
        .map_err(|error| path_io_error("save-buffer", path, error))
}

fn path_io_error(operation: &str, path: &Path, error: io::Error) -> io::Error {
    io::Error::new(
        error.kind(),
        format!("{operation} {}: {error}", path.display()),
    )
}

#[derive(Clone, Debug, Eq, PartialEq)]
enum ClientPrompt {
    Confirmation(String),
    Editing { text: String, cursor_column: u16 },
}

impl ClientPrompt {
    fn render_overlay(&self) -> wmux_core::ClientOverlay<'_> {
        match self {
            Self::Confirmation(text) => wmux_core::ClientOverlay::Confirmation(text),
            Self::Editing {
                text,
                cursor_column,
            } => wmux_core::ClientOverlay::Editing {
                text,
                cursor_column: *cursor_column,
            },
        }
    }
}

fn client_prompt(client: &wmux_core::Client) -> Option<ClientPrompt> {
    client
        .prompt
        .as_ref()
        .map(|prompt| ClientPrompt::Editing {
            text: prompt.display(),
            cursor_column: prompt.cursor_column(),
        })
        .or_else(|| {
            client
                .confirmation
                .as_ref()
                .map(|confirmation| ClientPrompt::Confirmation(confirmation.prompt.clone()))
        })
}

fn render_full_for_client(
    runtime: &mut Runtime,
    client: ClientId,
    size: TerminalSize,
    render_state: &mut RenderState,
    capabilities: RenderCapabilities,
    scroll_offsets: &mut BTreeMap<PaneId, usize>,
    copy_mode: Option<&CopyMode>,
) -> Vec<u8> {
    let Some((session, _, _)) = runtime.state.active_window_and_pane_for_client(client) else {
        return Vec::new();
    };
    let prompt = runtime.state.clients.get(&client).and_then(client_prompt);
    let (previous_frame_panes, retained_frames) = retained_panes_for_client(runtime, client);
    let viewports = pane_viewports_for_client(runtime, client, scroll_offsets, copy_mode);
    let Some(scene) = build_window_scene_with_client_overlay(
        &runtime.state,
        session,
        size.cols,
        size.rows,
        PaneSceneOverrides {
            previous_frame_panes: &previous_frame_panes,
            retained_frames: &retained_frames,
            viewports: &viewports,
            previous: Some(render_state),
        },
        prompt.as_ref().map(ClientPrompt::render_overlay),
    ) else {
        return Vec::new();
    };
    render_diff_scene_with_capabilities(&scene, render_state, capabilities)
}

fn render_diff_for_client(
    runtime: &mut Runtime,
    client: ClientId,
    size: TerminalSize,
    render_state: &mut RenderState,
    capabilities: RenderCapabilities,
    scroll_offsets: &mut BTreeMap<PaneId, usize>,
    copy_mode: Option<&CopyMode>,
) -> Vec<u8> {
    let Some((session, _, _)) = runtime.state.active_window_and_pane_for_client(client) else {
        return Vec::new();
    };
    let prompt = runtime.state.clients.get(&client).and_then(client_prompt);
    let (previous_frame_panes, retained_frames) = retained_panes_for_client(runtime, client);
    let viewports = pane_viewports_for_client(runtime, client, scroll_offsets, copy_mode);
    let Some(scene) = build_window_scene_with_client_overlay(
        &runtime.state,
        session,
        size.cols,
        size.rows,
        PaneSceneOverrides {
            previous_frame_panes: &previous_frame_panes,
            retained_frames: &retained_frames,
            viewports: &viewports,
            previous: Some(render_state),
        },
        prompt.as_ref().map(ClientPrompt::render_overlay),
    ) else {
        return Vec::new();
    };
    render_diff_scene_with_capabilities(&scene, render_state, capabilities)
}

fn pane_viewports_for_client(
    runtime: &mut Runtime,
    client: ClientId,
    offsets: &mut BTreeMap<PaneId, usize>,
    copy_mode: Option<&CopyMode>,
) -> Vec<PaneViewport> {
    let Some((_, window_id, _)) = runtime.state.active_window_and_pane_for_client(client) else {
        return Vec::new();
    };
    let Some(window) = runtime.state.window(window_id) else {
        return Vec::new();
    };
    let panes = window
        .panes
        .iter()
        .filter_map(|pane| runtime.state.pane(*pane).map(|state| (*pane, state.rect)))
        .collect::<Vec<_>>();
    let mut viewports = Vec::new();
    let mut reset = Vec::new();
    for (pane_id, rect) in panes {
        if let Some(mode) = copy_mode.filter(|mode| mode.pane == pane_id) {
            let Some(pane) = runtime.state.pane_mut(pane_id) else {
                continue;
            };
            let (all_lines, _) = pane.screen.copy_lines(rect.cols);
            let max_offset = all_lines
                .len()
                .saturating_sub(usize::from(rect.rows.max(1)));
            let offset = mode.viewport_offset().min(max_offset);
            let top = all_lines
                .len()
                .saturating_sub(usize::from(rect.rows.max(1)).saturating_add(offset));
            let mut lines = pane.screen.viewport_lines(rect.cols, rect.rows, offset);
            for (line, start, end) in mode.selection_ranges() {
                if let Some(visible) = line.checked_sub(top).and_then(|row| lines.get_mut(row)) {
                    visible.reverse_range(start, end);
                }
            }
            let cursor = if let Some(prompt) = mode.prompt() {
                let mut prompt_line = Line::blank(rect.cols);
                for (column, ch) in prompt.chars().take(usize::from(rect.cols)).enumerate() {
                    prompt_line.set(column as u16, ch, 1, Style::default());
                }
                if let Some(last) = lines.last_mut() {
                    *last = prompt_line;
                }
                Some((
                    rect.rows.saturating_sub(1),
                    prompt
                        .chars()
                        .count()
                        .min(usize::from(rect.cols.saturating_sub(1))) as u16,
                ))
            } else {
                Some((
                    mode.visible_cursor_row(all_lines.len(), rect.rows),
                    mode.cursor().column.min(rect.cols.saturating_sub(1)),
                ))
            };
            viewports.push(PaneViewport {
                pane: pane_id,
                offset,
                lines,
                cursor,
            });
            continue;
        }
        let Some(requested) = offsets.get(&pane_id).copied().filter(|offset| *offset > 0) else {
            continue;
        };
        let Some(pane) = runtime.state.pane_mut(pane_id) else {
            continue;
        };
        let offset = requested.min(pane.screen.max_viewport_offset(rect.cols));
        if offset == 0 {
            reset.push(pane_id);
            continue;
        }
        offsets.insert(pane_id, offset);
        viewports.push(PaneViewport {
            pane: pane_id,
            offset,
            lines: pane.screen.viewport_lines(rect.cols, rect.rows, offset),
            cursor: None,
        });
    }
    for pane in reset {
        offsets.remove(&pane);
    }
    viewports
}

fn pane_generations_for_client(runtime: &Runtime, client: ClientId) -> BTreeMap<PaneId, u64> {
    let Some((_, window_id, _)) = runtime.state.active_window_and_pane_for_client(client) else {
        return BTreeMap::new();
    };
    let Some(window) = runtime.state.window(window_id) else {
        return BTreeMap::new();
    };
    window
        .panes
        .iter()
        .filter_map(|pane_id| {
            runtime
                .state
                .pane(*pane_id)
                .map(|pane| (*pane_id, pane.generation()))
        })
        .collect()
}

fn retained_panes_for_client(
    runtime: &Runtime,
    client: ClientId,
) -> (Vec<PaneId>, Vec<RetainedPaneFrame>) {
    let now = Instant::now();
    let Some((_, window_id, _)) = runtime.state.active_window_and_pane_for_client(client) else {
        return (Vec::new(), Vec::new());
    };
    let Some(window) = runtime.state.window(window_id) else {
        return (Vec::new(), Vec::new());
    };
    let mut previous_frame_panes = Vec::new();
    let mut retained_frames = Vec::new();
    for pane_id in window.panes.iter().copied() {
        if runtime
            .state
            .pane(pane_id)
            .is_some_and(|pane| pane.screen.synchronized_output())
        {
            previous_frame_panes.push(pane_id);
        }
        if let Some(hold) = runtime.resize_repaint_holds.get(&pane_id) {
            if now < hold.max_until {
                retained_frames.push(hold.retained_frame.clone());
            }
        }
    }
    (previous_frame_panes, retained_frames)
}

fn trace_bytes(event: &str, pane: PaneId, bytes: &[u8]) {
    if !trace_enabled() {
        return;
    }
    trace_server(format_args!(
        "{event} pane={} bytes={} hex={} text={:?}",
        pane.raw(),
        bytes.len(),
        hex(bytes),
        text(bytes)
    ));
}

fn trace_server(args: std::fmt::Arguments<'_>) {
    let path = std::env::temp_dir().join("wmux-clean-server.trace.log");
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(file, "{args}");
    }
}

fn trace_enabled() -> bool {
    std::env::var_os("WMUX_TRACE").is_some_and(|value| value == "1")
}

fn hex(bytes: &[u8]) -> String {
    let mut preview = bytes
        .iter()
        .take(TRACE_PREVIEW_BYTES)
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(" ");
    if bytes.len() > TRACE_PREVIEW_BYTES {
        preview.push_str(" ...");
    }
    preview
}

fn text(bytes: &[u8]) -> String {
    let mut preview = bytes
        .iter()
        .take(TRACE_PREVIEW_BYTES)
        .flat_map(|byte| std::ascii::escape_default(*byte))
        .map(char::from)
        .collect::<String>();
    if bytes.len() > TRACE_PREVIEW_BYTES {
        preview.push_str("...");
    }
    preview
}

#[cfg(test)]
mod tests {
    use super::{
        collect_pane_events, read_async_message, render_full_for_client,
        run_with_platform_and_config, write_async_message, write_outbound_messages, ClientView,
        FrameScheduler, Outbound, OutputBudget, RenderCause, Runtime, ServerOwner, TestJobBackend,
        TestJobHandle, TestPtyBackend, TestPtyHandle, COMMANDS_PER_TURN, CONTROL_EVENTS_PER_TURN,
        PER_PANE_OUTPUT_TIME,
    };
    use std::{
        collections::BTreeMap,
        thread,
        time::{Duration, Instant},
    };
    use tokio::sync::mpsc;
    use wmux_config::{parse_config, WmuxConfig};
    use wmux_core::{
        parse_command_text, Command, CommandList, CommandSource, ControlNotification,
        ControlRecord, KeyBinding, KeyCode, KeyTableName, OptionTarget, OptionValue,
        RenderCapabilities, RenderState, ServerEvent, SplitDirection,
    };
    use wmux_platform::{
        AcceptedConnection, Endpoint, JobBackend, JobNotifier, MouseButton, MouseEvent,
        MouseEventKind, MouseModifiers, PeerIdentity, PlatformError, PlatformErrorKind,
        PlatformEvent, PlatformFuture, PlatformNotifier, PlatformPaneId, PlatformRequest,
        PlatformResult, PtyBackend, ServerListener, ServerPlatform, TerminalSize,
    };
    use wmux_protocol::{Message, TerminalCapabilities, VERSION};
    use wmux_protocol::{WireKeyCode, WireKeyEvent, WireKeyModifiers};

    struct MemoryListener {
        endpoint: Endpoint,
        owner: PeerIdentity,
        accepted: mpsc::UnboundedReceiver<AcceptedConnection>,
    }

    impl ServerListener for MemoryListener {
        fn endpoint(&self) -> &Endpoint {
            &self.endpoint
        }

        fn owner_identity(&self) -> &PeerIdentity {
            &self.owner
        }

        fn accept(&mut self) -> PlatformFuture<'_, AcceptedConnection> {
            Box::pin(async move {
                self.accepted.recv().await.ok_or_else(|| {
                    PlatformError::new(
                        PlatformErrorKind::Disconnected,
                        "accept memory client",
                        "memory connector closed",
                    )
                })
            })
        }
    }

    struct MemoryServerPlatform {
        listener: Option<MemoryListener>,
        pty: TestPtyHandle,
        jobs: TestJobHandle,
    }

    impl ServerPlatform for MemoryServerPlatform {
        fn bind(&mut self) -> PlatformResult<Box<dyn ServerListener>> {
            self.listener
                .take()
                .map(|listener| Box::new(listener) as Box<dyn ServerListener>)
                .ok_or_else(|| {
                    PlatformError::new(
                        PlatformErrorKind::AlreadyRunning,
                        "bind memory listener",
                        "listener was already bound",
                    )
                })
        }

        fn create_pty_backend(
            &mut self,
            notifier: PlatformNotifier,
        ) -> PlatformResult<Box<dyn PtyBackend>> {
            self.pty.set_notifier(notifier);
            Ok(Box::new(TestPtyBackend {
                state: self.pty.clone(),
            }))
        }

        fn create_job_backend(
            &mut self,
            notifier: JobNotifier,
        ) -> PlatformResult<Box<dyn JobBackend>> {
            self.jobs.set_notifier(notifier);
            Ok(Box::new(TestJobBackend {
                state: self.jobs.clone(),
            }))
        }
    }

    #[derive(Clone)]
    struct MemoryConnector {
        accepted: mpsc::UnboundedSender<AcceptedConnection>,
        owner: PeerIdentity,
    }

    impl MemoryConnector {
        fn connect(&self) -> tokio::io::DuplexStream {
            self.connect_as(self.owner.clone())
        }

        fn connect_as(&self, peer: PeerIdentity) -> tokio::io::DuplexStream {
            let (client, server) = tokio::io::duplex(4 * 1024 * 1024);
            self.accepted
                .send(AcceptedConnection {
                    stream: Box::new(server),
                    peer,
                })
                .expect("memory server is accepting clients");
            client
        }
    }

    fn memory_platform() -> (MemoryServerPlatform, MemoryConnector, TestPtyHandle) {
        let (accepted_tx, accepted_rx) = mpsc::unbounded_channel();
        let owner = PeerIdentity::from_token(b"memory-owner");
        let pty = TestPtyHandle::default();
        let jobs = TestJobHandle::default();
        (
            MemoryServerPlatform {
                listener: Some(MemoryListener {
                    endpoint: Endpoint::new("memory://wmux-lifecycle"),
                    owner: owner.clone(),
                    accepted: accepted_rx,
                }),
                pty: pty.clone(),
                jobs,
            },
            MemoryConnector {
                accepted: accepted_tx,
                owner,
            },
            pty,
        )
    }

    #[test]
    fn startup_config_commands_use_the_shared_serialized_queue() {
        let config = parse_config(
            "agent_compat = true\nset-option -g buffer-limit 60\nset-option -g @source config\n",
        )
        .unwrap();
        let mut owner = ServerOwner::new_test(config);

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "buffer-limit")
                .unwrap(),
            OptionValue::Number(60)
        );
        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "@source")
                .unwrap(),
            OptionValue::String("config".to_string())
        );
    }

    #[test]
    fn source_file_parses_whole_file_and_inserts_commands_in_place() {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "wmux-source-file-{}-{nonce}.wmux",
            std::process::id()
        ));
        std::fs::write(
            &path,
            "set-option -g @first loaded\nset-option -g @second after\n",
        )
        .unwrap();

        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        owner.enqueue_command_list(
            client,
            parse_command_text(&format!("source-file '{}'", path.display())).unwrap(),
            CommandSource::ClientRequest,
        );
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "@first")
                .unwrap(),
            OptionValue::String("loaded".to_string())
        );
        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "@second")
                .unwrap(),
            OptionValue::String("after".to_string())
        );
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn buffer_file_and_clipboard_effects_preserve_exact_bytes() {
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let input = std::env::temp_dir().join(format!(
            "wmux-load-buffer-{}-{nonce}.bin",
            std::process::id()
        ));
        let output = std::env::temp_dir().join(format!(
            "wmux-save-buffer-{}-{nonce}.bin",
            std::process::id()
        ));
        let expected = [0, 0xff, b'\n', b'\r', b'x'];
        std::fs::write(&input, expected).unwrap();

        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(8);
        owner.clients.insert(
            client,
            ClientView::new(outbound_tx, TerminalCapabilities::default()),
        );
        owner.enqueue_command_list(
            client,
            parse_command_text(&format!(
                "load-buffer -b exact '{}'; save-buffer -b exact '{}'; set-buffer -b clip -w copied",
                input.display(),
                output.display()
            ))
            .unwrap(),
            CommandSource::ClientRequest,
        );

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert_eq!(
            owner
                .runtime
                .state
                .paste_buffers
                .get(Some("exact"))
                .unwrap()
                .data(),
            expected
        );
        assert_eq!(std::fs::read(&output).unwrap(), expected);
        assert!(
            std::iter::from_fn(|| outbound_rx.try_recv().ok()).any(|message| matches!(
                message,
                Outbound::Message(wmux_protocol::Message::Clipboard(bytes)) if bytes == b"copied"
            ))
        );

        let _ = std::fs::remove_file(input);
        let _ = std::fs::remove_file(output);
    }

    #[test]
    fn hooks_run_in_the_active_invocation_before_remaining_parent_commands() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let created = owner.runtime.state.create_session("hooks", 80, 24);
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (tx, mut rx) = mpsc::channel(8);
        owner
            .clients
            .insert(client, ClientView::new(tx, TerminalCapabilities::default()));
        owner.enqueue_command_list(
            client,
            parse_command_text(
                "set-hook -g pane-created 'set-option -g @hook fired'; split-window; show-options -g -v @hook",
            )
            .unwrap(),
            CommandSource::ClientRequest,
        );

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::CommandOk(message))) if message == "fired"
        ));
    }

    #[test]
    fn recursive_hooks_stop_at_the_shared_depth_limit() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let created = owner.runtime.state.create_session("hooks", 80, 24);
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        owner.enqueue_command_list(
            client,
            parse_command_text(
                "set-hook -g buffer-changed 'set-buffer recursive'; set-buffer root",
            )
            .unwrap(),
            CommandSource::KeyBinding,
        );

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.runtime.queue.is_empty());
        assert_eq!(
            owner.runtime.state.paste_buffers.len(),
            usize::from(wmux_core::MAX_HOOK_DEPTH) + 1
        );
    }

    #[test]
    fn foreground_jobs_pause_only_the_originating_invocation_and_resume_in_place() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (tx, mut rx) = mpsc::channel(8);
        owner
            .clients
            .insert(client, ClientView::new(tx, TerminalCapabilities::default()));
        owner.enqueue_command_list(
            client,
            parse_command_text("run-shell 'echo ready'; set-option -g @after resumed").unwrap(),
            CommandSource::ClientRequest,
        );

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner
            .runtime
            .state
            .option(OptionTarget::Server, "@after")
            .is_err());
        let requests = owner.runtime.test_jobs.requests();
        let wmux_platform::JobRequest::Spawn(spawn) = &requests[0] else {
            panic!("job spawns")
        };
        assert_eq!(spawn.command, "echo ready");
        let job = spawn.job;
        owner.runtime.test_jobs.emit(
            job,
            wmux_platform::JobEvent::Output {
                job,
                bytes: b"ready\r\n".to_vec(),
            },
        );
        owner.runtime.test_jobs.emit(
            job,
            wmux_platform::JobEvent::Exited {
                job,
                exit_code: Some(0),
            },
        );
        owner
            .runtime
            .test_jobs
            .emit(job, wmux_platform::JobEvent::Closed { job });

        assert!(owner.process_job_events(CONTROL_EVENTS_PER_TURN));
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "@after")
                .unwrap(),
            OptionValue::String("resumed".to_string())
        );
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::CommandOk(message))) if message == "ready\r\n"
        ));
    }

    #[test]
    fn if_shell_selects_exactly_one_serialized_branch() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        owner.enqueue_command_list(
            client,
            parse_command_text(
                "if-shell false 'set-option -g @branch true' 'set-option -g @branch false'",
            )
            .unwrap(),
            CommandSource::KeyBinding,
        );
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        let wmux_platform::JobRequest::Spawn(spawn) = &owner.runtime.test_jobs.requests()[0] else {
            panic!("job spawns")
        };
        let job = spawn.job;
        owner.runtime.test_jobs.emit(
            job,
            wmux_platform::JobEvent::Exited {
                job,
                exit_code: Some(1),
            },
        );
        owner
            .runtime
            .test_jobs
            .emit(job, wmux_platform::JobEvent::Closed { job });
        assert!(owner.process_job_events(CONTROL_EVENTS_PER_TURN));
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "@branch")
                .unwrap(),
            OptionValue::String("false".to_string())
        );
    }

    #[test]
    fn control_commands_are_monotonic_and_emit_structured_ordered_records() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (tx, mut rx) = mpsc::channel(16);
        owner
            .clients
            .insert(client, ClientView::new(tx, TerminalCapabilities::default()));
        owner.enter_control(client);
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::Ready
            )))
        ));

        owner.handle_control_command(client, 1, "set-buffer -b observed data".to_string());
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::Begin { sequence: 1 }
            )))
        ));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::Notification(ControlNotification::BufferChanged { .. })
            )))
        ));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::End { sequence: 1, .. }
            )))
        ));

        owner.handle_control_command(client, 1, "list-sessions".to_string());
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::Begin { sequence: 1 }
            )))
        ));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(ControlRecord::Error { sequence: 1, message })))
                if message.contains("expected 2")
        ));

        owner.handle_control_command(client, 2, "not-a-command".to_string());
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::Begin { sequence: 2 }
            )))
        ));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(
                ControlRecord::Error { sequence: 2, .. }
            )))
        ));
    }

    #[test]
    fn control_output_is_sliced_filtered_and_pauses_before_replies_are_starved() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let created = owner.runtime.state.create_session("control", 80, 24);
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (tx, mut rx) = mpsc::channel(64);
        owner
            .clients
            .insert(client, ClientView::new(tx, TerminalCapabilities::default()));
        owner.enter_control(client);
        let _ = rx.try_recv();

        owner.publish_control_output(created.pane, &vec![b'x'; 64 * 1024 + 1]);
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(ControlRecord::Output { pane, bytes })))
                if pane == created.pane && bytes.len() == 64 * 1024
        ));
        assert!(matches!(
            rx.try_recv(),
            Ok(Outbound::Message(Message::ControlRecord(ControlRecord::Output { bytes, .. })))
                if bytes == b"x"
        ));

        for _ in 0..64 {
            owner.publish_control_output(created.pane, &[b'y'; 64 * 1024]);
            if owner.clients[&client].control.as_ref().unwrap().paused {
                break;
            }
        }
        assert!(owner.clients[&client].control.as_ref().unwrap().paused);
        owner.handle_control_command(client, 1, "refresh-client".to_string());
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(!owner.clients[&client].control.as_ref().unwrap().paused);
    }

    async fn memory_handshake(connector: &MemoryConnector) -> tokio::io::DuplexStream {
        let mut stream = connector.connect();
        write_async_message(
            &mut stream,
            Message::Hello {
                version: VERSION,
                pid: 42,
                capabilities: TerminalCapabilities::default(),
            },
        )
        .await
        .unwrap();
        assert!(matches!(
            read_async_message(&mut stream).await.unwrap(),
            Some(Message::HelloOk {
                version: VERSION,
                ..
            })
        ));
        stream
    }

    async fn command_ok(stream: &mut tokio::io::DuplexStream, command: &str) -> Vec<u8> {
        write_async_message(stream, Message::Command(command.to_string()))
            .await
            .unwrap();
        let mut output = Vec::new();
        loop {
            let message = tokio::time::timeout(Duration::from_secs(2), read_async_message(stream))
                .await
                .expect("command response timed out")
                .unwrap();
            match message {
                Some(Message::CommandOk(_)) => return output,
                Some(Message::CommandErr(error)) => panic!("command {command:?} failed: {error}"),
                Some(Message::Output(bytes)) => output.extend(bytes),
                Some(other) => panic!("command {command:?} returned {other:?}"),
                None => panic!("server closed while running {command:?}"),
            }
        }
    }

    async fn wait_until(mut condition: impl FnMut() -> bool) {
        tokio::time::timeout(Duration::from_secs(2), async {
            while !condition() {
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        })
        .await
        .expect("observable lifecycle transition timed out");
    }

    async fn wait_for_output(stream: &mut tokio::io::DuplexStream, needle: &[u8]) {
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                match read_async_message(stream).await.unwrap() {
                    Some(Message::Output(bytes))
                        if bytes.windows(needle.len()).any(|w| w == needle) =>
                    {
                        return;
                    }
                    Some(_) => {}
                    None => panic!("server closed before authoritative background output rendered"),
                }
            }
        })
        .await
        .expect("authoritative background output render timed out");
    }

    #[tokio::test]
    async fn mock_platform_drives_complete_real_protocol_lifecycle() {
        let (platform, connector, pty) = memory_platform();
        let server = thread::spawn(move || {
            run_with_platform_and_config(Box::new(platform), WmuxConfig::default())
        });

        let mut wrong_peer = connector.connect_as(PeerIdentity::from_token(b"intruder"));
        assert_eq!(
            tokio::time::timeout(Duration::from_secs(2), read_async_message(&mut wrong_peer))
                .await
                .expect("wrong peer was not rejected before hello")
                .unwrap(),
            None
        );

        let mut client = memory_handshake(&connector).await;
        command_ok(&mut client, "new-session -d -s durable").await;
        wait_until(|| {
            pty.requests()
                .iter()
                .filter(|request| matches!(request, PlatformRequest::SpawnPane(_)))
                .count()
                == 1
        })
        .await;
        let first_pane = pty
            .requests()
            .iter()
            .find_map(|request| match request {
                PlatformRequest::SpawnPane(spawn) => Some(spawn.pane),
                _ => None,
            })
            .unwrap();

        command_ok(&mut client, "attach-session -t durable").await;
        write_async_message(
            &mut client,
            Message::Key(WireKeyEvent {
                code: WireKeyCode::Char('x'),
                modifiers: WireKeyModifiers::NONE,
                raw: b"x".to_vec(),
            }),
        )
        .await
        .unwrap();
        wait_until(|| {
            pty.requests().iter().any(|request| {
                matches!(request, PlatformRequest::WritePane { pane, bytes } if *pane == first_pane && bytes == b"x")
            })
        })
        .await;

        command_ok(&mut client, "split-window -h").await;
        wait_until(|| {
            pty.requests()
                .iter()
                .filter(|request| matches!(request, PlatformRequest::SpawnPane(_)))
                .count()
                == 2
        })
        .await;
        write_async_message(
            &mut client,
            Message::Resize {
                cols: 100,
                rows: 30,
            },
        )
        .await
        .unwrap();
        wait_until(|| {
            pty.requests().iter().any(|request| {
                matches!(request, PlatformRequest::ResizePane { size, .. } if size.rows == 29)
            })
        })
        .await;

        write_async_message(&mut client, Message::Detach)
            .await
            .unwrap();
        loop {
            match tokio::time::timeout(Duration::from_secs(2), read_async_message(&mut client))
                .await
                .expect("detach response timed out")
                .unwrap()
            {
                Some(Message::CommandOk(_)) => break,
                Some(Message::Output(_)) => {}
                other => panic!("unexpected detach response: {other:?}"),
            }
        }

        pty.emit(
            first_pane,
            PlatformEvent::PtyOutput {
                pane: first_pane,
                bytes: b"background-ready".to_vec(),
            },
        );
        let mut reattached = memory_handshake(&connector).await;
        let early_output = command_ok(&mut reattached, "attach-session -t durable").await;
        if !early_output
            .windows(b"background-ready".len())
            .any(|window| window == b"background-ready")
        {
            wait_for_output(&mut reattached, b"background-ready").await;
        }

        command_ok(&mut reattached, "kill-pane").await;
        command_ok(&mut reattached, "kill-session -t durable").await;
        wait_until(|| {
            pty.requests()
                .iter()
                .filter(|request| matches!(request, PlatformRequest::TerminatePane { .. }))
                .count()
                >= 2
        })
        .await;

        let mut shutdown_client = memory_handshake(&connector).await;
        command_ok(&mut shutdown_client, "kill-server").await;
        wait_until(|| server.is_finished()).await;
        server.join().unwrap().unwrap();
    }

    fn attached_owner() -> (ServerOwner, wmux_core::ClientId, wmux_core::PaneId) {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("keys", 80, 24);
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, _outbound_rx) = mpsc::channel(16);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(client, view);
        owner
            .runtime
            .add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        (owner, client, created.pane)
    }

    #[test]
    fn server_runtime_submits_only_semantic_platform_requests() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("platform", 80, 24);
        runtime
            .ensure_platform_pane(created.pane, TerminalSize::new(80, 24))
            .unwrap();
        runtime
            .write_pane_input(
                created.pane,
                wmux_core::ClientInput::Bytes(b"input".to_vec()),
            )
            .unwrap();
        runtime
            .resize_platform_pane(created.pane, TerminalSize::new(79, 24))
            .unwrap();
        runtime.shutdown_platform_panes();

        let requests = runtime.test_platform_requests();
        assert!(matches!(
            requests[0],
            wmux_platform::PlatformRequest::SpawnPane(_)
        ));
        assert!(matches!(
            requests[1],
            wmux_platform::PlatformRequest::WritePane { .. }
        ));
        assert!(matches!(
            requests[2],
            wmux_platform::PlatformRequest::ResizePane { .. }
        ));
        assert!(matches!(
            requests[3],
            wmux_platform::PlatformRequest::TerminatePane { .. }
        ));
    }

    fn wire_char(character: char, modifiers: WireKeyModifiers, raw: &[u8]) -> WireKeyEvent {
        WireKeyEvent {
            code: WireKeyCode::Char(character),
            modifiers,
            raw: raw.to_vec(),
        }
    }

    #[test]
    fn server_owned_key_prefix_executes_without_platform_input() {
        let (mut owner, client, _) = attached_owner();
        assert!(owner.clients[&client]
            .scheduler
            .input_priority_until
            .is_none());

        owner
            .handle_wire_key_at(
                client,
                wire_char('b', WireKeyModifiers::CONTROL, &[0x02]),
                0,
            )
            .unwrap();
        owner
            .handle_wire_key_at(client, wire_char('c', WireKeyModifiers::NONE, b"c"), 1)
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert_eq!(owner.runtime.state.windows.len(), 2);
        assert!(owner.runtime.test_inputs.is_empty());
        assert!(owner.clients[&client]
            .scheduler
            .input_priority_until
            .is_some());
    }

    #[test]
    fn printable_raw_quote_routes_despite_redundant_console_modifiers() {
        let (mut owner, client, _) = attached_owner();

        owner
            .handle_wire_key_at(
                client,
                wire_char('b', WireKeyModifiers::CONTROL, &[0x02]),
                0,
            )
            .unwrap();
        owner
            .handle_wire_key_at(
                client,
                wire_char(
                    '"',
                    WireKeyModifiers::SHIFT | WireKeyModifiers::CONTROL,
                    b"\"",
                ),
                1,
            )
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert_eq!(owner.runtime.state.panes.len(), 2);
        assert!(owner.runtime.test_inputs.is_empty());
    }

    #[test]
    fn destructive_prefix_binding_prompts_rejects_and_only_then_accepts() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("confirm", 80, 24);
        let killed = owner
            .runtime
            .state
            .split_pane(created.window, None, SplitDirection::LeftRight, 80, 24)
            .unwrap();
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let authoritative_last_line = owner.runtime.state.panes[&killed]
            .screen
            .render_line(23)
            .unwrap()
            .clone();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(16);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(client, view);
        for pane in [created.pane, killed] {
            owner
                .runtime
                .add_test_platform_pane(pane, TerminalSize::new(40, 24));
        }

        for (character, modifiers, raw) in [
            ('b', WireKeyModifiers::CONTROL, b"\x02".as_slice()),
            ('x', WireKeyModifiers::NONE, b"x".as_slice()),
        ] {
            owner
                .handle_wire_key_at(client, wire_char(character, modifiers, raw), 0)
                .unwrap();
        }
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.runtime.state.clients[&client].confirmation.is_some());
        assert!(owner.render_due_clients(Instant::now()));
        let prompt_frame = outbound_rx.try_recv().unwrap();
        let Outbound::Message(Message::Output(bytes)) = &prompt_frame else {
            panic!("expected confirmation render");
        };
        assert!(String::from_utf8_lossy(bytes).contains("kill-pane? (y/n)"));
        assert_eq!(
            owner.runtime.state.panes[&killed].screen.render_line(23),
            Some(&authoritative_last_line)
        );
        owner
            .clients
            .get_mut(&client)
            .unwrap()
            .drained(prompt_frame.wire_len());

        owner
            .handle_wire_key_at(client, wire_char('n', WireKeyModifiers::NONE, b"n"), 1)
            .unwrap();
        assert!(owner.runtime.state.clients[&client].confirmation.is_none());
        assert!(owner.runtime.state.panes.contains_key(&killed));
        assert!(owner.clients[&client].full_render);

        for (character, modifiers, raw) in [
            ('b', WireKeyModifiers::CONTROL, b"\x02".as_slice()),
            ('x', WireKeyModifiers::NONE, b"x".as_slice()),
        ] {
            owner
                .handle_wire_key_at(client, wire_char(character, modifiers, raw), 2)
                .unwrap();
        }
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        owner
            .handle_wire_key_at(client, wire_char('y', WireKeyModifiers::NONE, b"y"), 3)
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(!owner.runtime.state.panes.contains_key(&killed));
        assert!(owner.runtime.state.panes.contains_key(&created.pane));
    }

    #[test]
    fn prefix_comma_edits_window_name_in_status_without_reaching_the_pane() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("rename", 80, 24);
        owner
            .runtime
            .state
            .rename_window(created.window, "shell")
            .unwrap();
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(16);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(client, view);
        owner
            .runtime
            .add_test_platform_pane(created.pane, TerminalSize::new(80, 23));

        for (character, modifiers, raw) in [
            ('b', WireKeyModifiers::CONTROL, b"\x02".as_slice()),
            (',', WireKeyModifiers::NONE, b",".as_slice()),
        ] {
            owner
                .handle_wire_key_at(client, wire_char(character, modifiers, raw), 0)
                .unwrap();
        }
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.render_due_clients(Instant::now()));
        let prompt_frame = outbound_rx.try_recv().unwrap();
        let Outbound::Message(Message::Output(bytes)) = &prompt_frame else {
            panic!("expected rename prompt render");
        };
        assert!(String::from_utf8_lossy(bytes).contains("rename-window: shell"));
        owner
            .clients
            .get_mut(&client)
            .unwrap()
            .drained(prompt_frame.wire_len());

        owner
            .handle_wire_key_at(
                client,
                wire_char('u', WireKeyModifiers::CONTROL, b"\x15"),
                1,
            )
            .unwrap();
        for character in ['d', 'e', 'v'] {
            owner
                .handle_wire_key_at(
                    client,
                    wire_char(
                        character,
                        WireKeyModifiers::NONE,
                        character.to_string().as_bytes(),
                    ),
                    2,
                )
                .unwrap();
        }
        owner
            .handle_wire_key_at(
                client,
                WireKeyEvent {
                    code: WireKeyCode::Enter,
                    modifiers: WireKeyModifiers::NONE,
                    raw: b"\r".to_vec(),
                },
                3,
            )
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert_eq!(owner.runtime.state.windows[&created.window].name, "dev");
        assert!(owner.runtime.test_inputs.is_empty());
    }

    #[test]
    fn prefix_dollar_edits_session_name_without_reaching_the_pane() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("work", 80, 24);
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(16);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(client, view);
        owner
            .runtime
            .add_test_platform_pane(created.pane, TerminalSize::new(80, 23));

        for (character, modifiers, raw) in [
            ('b', WireKeyModifiers::CONTROL, b"\x02".as_slice()),
            ('$', WireKeyModifiers::SHIFT, b"$".as_slice()),
        ] {
            owner
                .handle_wire_key_at(client, wire_char(character, modifiers, raw), 0)
                .unwrap();
        }
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.render_due_clients(Instant::now()));
        let prompt_frame = outbound_rx.try_recv().unwrap();
        let Outbound::Message(Message::Output(bytes)) = &prompt_frame else {
            panic!("expected rename prompt render");
        };
        assert!(String::from_utf8_lossy(bytes).contains("rename-session: work"));
        owner
            .clients
            .get_mut(&client)
            .unwrap()
            .drained(prompt_frame.wire_len());

        owner
            .handle_wire_key_at(
                client,
                wire_char('u', WireKeyModifiers::CONTROL, b"\x15"),
                1,
            )
            .unwrap();
        for character in ['m', 'a', 'i', 'n'] {
            owner
                .handle_wire_key_at(
                    client,
                    wire_char(
                        character,
                        WireKeyModifiers::NONE,
                        character.to_string().as_bytes(),
                    ),
                    2,
                )
                .unwrap();
        }
        owner
            .handle_wire_key_at(
                client,
                WireKeyEvent {
                    code: WireKeyCode::Enter,
                    modifiers: WireKeyModifiers::NONE,
                    raw: b"\r".to_vec(),
                },
                3,
            )
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert_eq!(owner.runtime.state.sessions[&created.session].name, "main");
        assert!(owner.runtime.test_inputs.is_empty());
    }

    #[test]
    fn idle_client_publishes_command_prompt_without_pane_damage() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("idle-prompt", 80, 24);
        owner
            .runtime
            .state
            .rename_window(created.window, "shell")
            .unwrap();
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(16);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        view.request_immediate_render(Instant::now());
        owner.clients.insert(client, view);

        assert!(owner.render_due_clients(Instant::now()));
        let baseline = outbound_rx.try_recv().unwrap();
        owner
            .clients
            .get_mut(&client)
            .unwrap()
            .drained(baseline.wire_len());

        owner
            .handle_wire_key_at(
                client,
                wire_char('b', WireKeyModifiers::CONTROL, b"\x02"),
                0,
            )
            .unwrap();
        owner
            .handle_wire_key_at(client, wire_char(',', WireKeyModifiers::NONE, b","), 1)
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert!(owner.render_due_clients(Instant::now()));
        let Outbound::Message(Message::Output(bytes)) = outbound_rx.try_recv().unwrap() else {
            panic!("expected editing prompt frame");
        };
        let frame = String::from_utf8_lossy(&bytes);
        assert!(frame.contains("rename-window:"));
        assert!(frame.contains("shell"));
    }

    #[test]
    fn editing_prompt_uses_the_physical_cursor_at_the_input_position() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("cursor-prompt", 80, 24);
        runtime
            .state
            .rename_window(created.window, "shell")
            .unwrap();
        let client = runtime.state.add_client();
        runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        runtime.state.clients.get_mut(&client).unwrap().prompt = Some(wmux_core::PromptState::new(
            "rename-window: ".to_string(),
            "shell".to_string(),
            "rename-window -- %%".to_string(),
        ));
        let mut render_state = RenderState::new(80, 24);
        let mut scroll_offsets = BTreeMap::new();

        let frame = render_full_for_client(
            &mut runtime,
            client,
            TerminalSize::new(80, 24),
            &mut render_state,
            RenderCapabilities::default(),
            &mut scroll_offsets,
            None,
        );
        let frame = String::from_utf8(frame).unwrap();

        assert!(frame.contains("rename-window: shell"));
        assert!(!frame.contains('▏'));
        assert!(frame.contains("\x1b[24;21H"));
        assert!(frame.ends_with("\x1b[?25h"));
    }

    #[test]
    fn server_owned_key_unbound_utf8_and_paste_reach_the_pane_once() {
        let (mut owner, client, pane) = attached_owner();
        owner
            .handle_wire_key_at(
                client,
                wire_char('λ', WireKeyModifiers::NONE, "λ".as_bytes()),
                0,
            )
            .unwrap();
        owner
            .handle_event(ServerEvent::ClientInput {
                client,
                input: wmux_core::ClientInput::Paste(b"abc".to_vec()),
            })
            .unwrap();

        assert_eq!(
            owner.runtime.test_inputs,
            [(pane, "λ".as_bytes().to_vec()), (pane, b"abc".to_vec())]
        );
    }

    #[test]
    fn server_owned_key_prefix_copy_repeat_and_expiry_are_client_scoped() {
        let (mut owner, first, pane) = attached_owner();
        let second = owner.runtime.state.add_client();
        let session = owner.runtime.state.sessions.keys().next().copied().unwrap();
        owner.runtime.state.attach_client(second, session).unwrap();
        let (outbound_tx, _outbound_rx) = mpsc::channel(16);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(second, view);

        owner
            .handle_wire_key_at(first, wire_char('b', WireKeyModifiers::CONTROL, &[0x02]), 0)
            .unwrap();
        owner
            .handle_wire_key_at(second, wire_char('c', WireKeyModifiers::NONE, b"c"), 1)
            .unwrap();
        assert_eq!(owner.runtime.test_inputs, [(pane, b"c".to_vec())]);
        assert_eq!(
            owner.runtime.state.clients[&first].key_table,
            KeyTableName::PREFIX
        );
        assert_eq!(
            owner.runtime.state.clients[&second].key_table,
            KeyTableName::ROOT
        );

        owner.enter_copy_mode(second).unwrap();
        owner
            .handle_wire_key_at(second, wire_char('q', WireKeyModifiers::NONE, b"q"), 2)
            .unwrap();
        assert!(owner.clients[&second].copy_mode.is_none());
        assert_eq!(owner.runtime.test_inputs.len(), 1);

        owner
            .handle_wire_key_at(first, wire_char('c', WireKeyModifiers::NONE, b"c"), 501)
            .unwrap();
        assert_eq!(
            owner.runtime.test_inputs.last(),
            Some(&(pane, b"c".to_vec()))
        );

        owner
            .runtime
            .state
            .key_tables
            .table_mut(KeyTableName::PREFIX)
            .unwrap()
            .bind(KeyBinding {
                key: KeyCode::parse("Right").unwrap(),
                repeatable: true,
                commands: parse_command_text("resize-pane -R").unwrap(),
            });
        owner
            .handle_wire_key_at(
                first,
                wire_char('b', WireKeyModifiers::CONTROL, &[0x02]),
                600,
            )
            .unwrap();
        owner
            .handle_wire_key_at(
                first,
                WireKeyEvent {
                    code: WireKeyCode::Right,
                    modifiers: WireKeyModifiers::NONE,
                    raw: b"\x1b[C".to_vec(),
                },
                601,
            )
            .unwrap();
        assert!(owner.runtime.state.clients[&first]
            .repeat_deadline_ms
            .is_some());
        assert!(!owner.runtime.queue.is_empty());
    }

    #[test]
    fn server_owned_key_disconnect_drops_pending_binding_commands() {
        let (mut owner, client, _) = attached_owner();
        owner
            .handle_wire_key_at(
                client,
                wire_char('b', WireKeyModifiers::CONTROL, &[0x02]),
                0,
            )
            .unwrap();
        owner
            .handle_wire_key_at(client, wire_char('c', WireKeyModifiers::NONE, b"c"), 1)
            .unwrap();
        assert!(!owner.runtime.queue.is_empty());

        owner.disconnect_client(client, true);

        assert!(owner.runtime.queue.is_empty());
    }

    #[test]
    fn server_owned_key_conversion_moves_raw_bytes_and_rejects_invalid_functions() {
        let mut raw = Vec::with_capacity(32);
        raw.extend_from_slice("λ".as_bytes());
        let pointer = raw.as_ptr();
        let event = super::core_key_event(WireKeyEvent {
            code: WireKeyCode::Char('λ'),
            modifiers: WireKeyModifiers::ALT,
            raw,
        })
        .unwrap();

        assert_eq!(event.raw.as_ptr(), pointer);
        assert!(super::core_key_event(WireKeyEvent {
            code: WireKeyCode::Function(0),
            modifiers: WireKeyModifiers::NONE,
            raw: Vec::new(),
        })
        .is_err());
    }

    #[test]
    fn shutdown_transaction_notifies_clients_and_clears_platform_panes() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let requester = owner.runtime.state.add_client();
        let attached = owner.runtime.state.add_client();
        let (requester_tx, mut requester_rx) = mpsc::channel(4);
        let (attached_tx, mut attached_rx) = mpsc::channel(4);
        owner.clients.insert(
            requester,
            ClientView::new(requester_tx, TerminalCapabilities::default()),
        );
        let mut attached_view = ClientView::new(attached_tx, TerminalCapabilities::default());
        attached_view.attached = true;
        owner.clients.insert(attached, attached_view);
        let created = owner.runtime.state.create_session("shutdown", 80, 24);
        owner
            .runtime
            .add_test_platform_pane(created.pane, TerminalSize::new(80, 24));

        owner
            .handle_event(ServerEvent::Command {
                client: requester,
                command: Command::KillServer,
            })
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert!(owner.is_shutting_down());
        assert!(matches!(
            requester_rx.try_recv(),
            Ok(Outbound::Shutdown(wmux_protocol::Message::CommandOk(_)))
        ));
        assert!(matches!(
            attached_rx.try_recv(),
            Ok(Outbound::Shutdown(wmux_protocol::Message::Shutdown))
        ));
        assert!(owner.runtime.platform_panes.is_empty());
        assert!(owner.runtime.output_ring.is_empty());
    }

    #[test]
    fn command_queue_budget_yields_and_emits_one_list_completion() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(4);
        owner.clients.insert(
            client,
            ClientView::new(outbound_tx, TerminalCapabilities::default()),
        );
        let commands = CommandList::new(vec![Command::ListClients; COMMANDS_PER_TURN + 1]).unwrap();
        owner.enqueue_command_list(client, commands, CommandSource::ClientRequest);

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(!owner.runtime.queue.is_empty());
        assert!(matches!(
            outbound_rx.try_recv(),
            Err(mpsc::error::TryRecvError::Empty)
        ));

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.runtime.queue.is_empty());
        let Outbound::Message(Message::CommandOk(message)) = outbound_rx.try_recv().unwrap() else {
            panic!("expected one command-list completion");
        };
        assert_eq!(message.lines().count(), COMMANDS_PER_TURN + 1);
        assert!(matches!(
            outbound_rx.try_recv(),
            Err(mpsc::error::TryRecvError::Empty)
        ));
    }

    #[test]
    fn command_effects_apply_before_the_next_list_command() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("copy", 80, 24);
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, _outbound_rx) = mpsc::channel(4);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(client, view);
        owner.enqueue_command_list(
            client,
            CommandList::new(vec![Command::CopyMode, Command::ListClients]).unwrap(),
            CommandSource::ClientRequest,
        );

        assert!(owner.process_command_queue(1));
        assert!(owner.clients[&client].copy_mode.is_some());
        assert!(!owner.runtime.queue.is_empty());
    }

    #[test]
    fn refresh_client_invalidates_only_the_requesting_clients_baseline() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("refresh", 80, 24);
        let first = owner.runtime.state.add_client();
        let second = owner.runtime.state.add_client();
        for client in [first, second] {
            owner
                .runtime
                .state
                .attach_client(client, created.session)
                .unwrap();
            let (outbound_tx, _outbound_rx) = mpsc::channel(4);
            let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
            view.attached = true;
            view.full_render = false;
            owner.clients.insert(client, view);
        }
        owner.enqueue_command_list(
            first,
            parse_command_text("refresh-client").unwrap(),
            CommandSource::KeyBinding,
        );

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.clients[&first].full_render);
        assert!(!owner.clients[&second].full_render);
        assert!(owner.clients[&first].scheduler.deadline.is_some());
        assert!(owner.clients[&second].scheduler.deadline.is_none());
    }

    #[test]
    fn protocol_command_text_is_parsed_as_one_atomic_list() {
        let commands = super::parse_command_line(
            "rename-window -n 'alpha beta'; list-windows # trailing comment",
        )
        .unwrap();
        assert_eq!(commands.len(), 2);
        assert!(matches!(
            &commands[0],
            Command::RenameWindow { name, .. } if name == "alpha beta"
        ));
    }

    #[tokio::test]
    async fn shutdown_writer_flushes_reports_drain_and_returns() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (outbound_tx, outbound_rx) = mpsc::channel(2);
        outbound_tx
            .send(Outbound::Shutdown(wmux_protocol::Message::CommandOk(
                "done".to_string(),
            )))
            .await
            .unwrap();
        drop(outbound_tx);
        let (mut writer, mut reader) = tokio::io::duplex(128);
        let (owner_tx, owner_rx) = std::sync::mpsc::channel();

        let writer_task = tokio::spawn(async move {
            write_outbound_messages(&mut writer, outbound_rx, &owner_tx, client).await
        });
        assert_eq!(
            read_async_message(&mut reader).await.unwrap(),
            Some(wmux_protocol::Message::CommandOk("done".to_string()))
        );
        writer_task.await.unwrap().unwrap();
        assert!(matches!(
            owner_rx.recv_timeout(Duration::from_secs(1)),
            Ok(super::OwnerMessage::OutboundDrained { client: drained, .. }) if drained == client
        ));
    }

    #[test]
    fn frame_scheduler_is_immediate_after_idle_and_coalesces_bursts() {
        let mut scheduler = FrameScheduler::new();
        let now = Instant::now();
        scheduler.request(now, RenderCause::Output);
        assert_eq!(scheduler.deadline, Some(now));

        scheduler.published(now);
        let sustained = now + Duration::from_millis(1);
        scheduler.request(sustained, RenderCause::Output);
        let delay = scheduler.deadline.unwrap() - sustained;
        assert!(delay >= super::MIN_FRAME_DELAY);
        assert!(delay <= super::MAX_FRAME_DELAY);

        scheduler.deadline = None;
        scheduler.note_input(sustained);
        scheduler.request(sustained, RenderCause::Output);
        assert_eq!(
            scheduler.deadline,
            Some(sustained + super::REDRAW_CYCLE_DELAY)
        );
    }

    fn mouse(kind: MouseEventKind, column: u16, row: u16) -> MouseEvent {
        MouseEvent {
            kind,
            button: MouseButton::None,
            modifiers: MouseModifiers::default(),
            column,
            row,
        }
    }

    #[test]
    fn wheel_scrollback_is_server_owned_and_independent_per_client() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("scroll", 20, 3);
        owner
            .runtime
            .apply_pty_output(created.pane, b"one\r\ntwo\r\nthree\r\nfour\r\nfive\r\nsix");
        owner.runtime.take_history_growth();

        let first = owner.runtime.state.add_client();
        let second = owner.runtime.state.add_client();
        for client in [first, second] {
            owner
                .runtime
                .state
                .attach_client(client, created.session)
                .unwrap();
            let (tx, _rx) = mpsc::channel(8);
            let mut view = ClientView::new(tx, TerminalCapabilities::default());
            view.attached = true;
            view.size = TerminalSize::new(20, 3);
            owner.clients.insert(client, view);
        }

        owner
            .handle_event(ServerEvent::ClientMouse {
                client: first,
                event: mouse(MouseEventKind::ScrollUp, 1, 1),
            })
            .unwrap();
        let initial = owner.clients[&first].scroll_offsets[&created.pane];
        assert!(initial > 0);
        assert!(owner.clients[&second].scroll_offsets.is_empty());

        owner.runtime.apply_pty_output(created.pane, b"\r\nseven");
        owner.anchor_scrolled_views();
        assert!(owner.clients[&first].scroll_offsets[&created.pane] > initial);

        owner
            .handle_event(ServerEvent::ClientMouse {
                client: first,
                event: mouse(MouseEventKind::ScrollDown, 1, 1),
            })
            .unwrap();
        assert!(
            owner.clients[&first]
                .scroll_offsets
                .get(&created.pane)
                .copied()
                .unwrap_or(0)
                < initial + 1
        );
    }

    #[test]
    fn application_mouse_mode_takes_precedence_over_scrollback() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("mouse", 20, 3);
        owner
            .runtime
            .apply_pty_output(created.pane, b"\x1b[?1000h\x1b[?1006h");
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (tx, _rx) = mpsc::channel(8);
        let mut view = ClientView::new(tx, TerminalCapabilities::default());
        view.attached = true;
        view.size = TerminalSize::new(20, 3);
        owner.clients.insert(client, view);

        owner
            .handle_event(ServerEvent::ClientMouse {
                client,
                event: mouse(MouseEventKind::ScrollUp, 1, 1),
            })
            .unwrap();

        assert!(owner.clients[&client].scroll_offsets.is_empty());
        assert!(owner
            .runtime
            .state
            .pane(created.pane)
            .unwrap()
            .screen
            .application_mouse_enabled());
    }

    #[test]
    fn copy_mode_is_client_owned_and_copies_through_semantic_clipboard_message() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("copy", 20, 3);
        owner
            .runtime
            .apply_pty_output(created.pane, b"alpha\r\nbeta\r\ngamma");
        let first = owner.runtime.state.add_client();
        let second = owner.runtime.state.add_client();
        let (first_tx, mut first_rx) = mpsc::channel(8);
        let (second_tx, _second_rx) = mpsc::channel(8);
        for (client, tx) in [(first, first_tx), (second, second_tx)] {
            owner
                .runtime
                .state
                .attach_client(client, created.session)
                .unwrap();
            let mut view = ClientView::new(tx, TerminalCapabilities::default());
            view.attached = true;
            view.size = TerminalSize::new(20, 3);
            owner.clients.insert(client, view);
        }

        owner.enter_copy_mode(first).unwrap();
        assert!(owner.clients[&first].copy_mode.is_some());
        assert!(owner.clients[&second].copy_mode.is_none());
        owner.handle_copy_mode_key(first, b"g".to_vec());
        owner.handle_copy_mode_key(first, b" ".to_vec());
        owner.handle_copy_mode_key(first, b"$".to_vec());
        owner.handle_copy_mode_key(first, b"\r".to_vec());

        assert!(owner.clients[&first].copy_mode.is_none());
        assert_eq!(
            owner.runtime.state.paste_buffers.get(None).unwrap().data(),
            b"alpha"
        );
        let outbound = first_rx.try_recv().unwrap();
        assert!(matches!(
            outbound,
            Outbound::Message(wmux_protocol::Message::Clipboard(bytes)) if bytes == b"alpha"
        ));
    }

    #[test]
    fn copy_mode_stores_buffer_when_clipboard_delivery_is_disabled() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("copy-off", 20, 3);
        owner.runtime.apply_pty_output(created.pane, b"alpha");
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        owner
            .runtime
            .state
            .options
            .set(OptionTarget::Client(client), "set-clipboard", "off")
            .unwrap();
        let (tx, mut rx) = mpsc::channel(8);
        let mut view = ClientView::new(tx, TerminalCapabilities::default());
        view.attached = true;
        view.size = TerminalSize::new(20, 3);
        owner.clients.insert(client, view);

        owner.enter_copy_mode(client).unwrap();
        for key in [b"g".as_slice(), b" ", b"$", b"\r"] {
            owner.handle_copy_mode_key(client, key.to_vec());
        }

        assert_eq!(
            owner.runtime.state.paste_buffers.get(None).unwrap().data(),
            b"alpha"
        );
        assert!(rx.try_recv().is_err());
    }

    #[test]
    fn pending_paste_is_chunked_bracketed_once_and_yields_to_commands() {
        let (mut owner, client, pane) = attached_owner();
        owner
            .runtime
            .state
            .pane_mut(pane)
            .unwrap()
            .screen
            .set_bracketed_paste(true);
        let bytes = (0..(1024 * 1024 + 17))
            .map(|index| (index % 251) as u8)
            .collect::<Vec<_>>();
        owner
            .runtime
            .state
            .paste_buffers
            .set_named("bulk", bytes.clone())
            .unwrap();
        owner
            .runtime
            .queue
            .push_list(
                client,
                CommandList::new(vec![Command::PasteBuffer {
                    name: Some("bulk".to_string()),
                    delete: false,
                    bracketed: true,
                    target: None,
                }])
                .unwrap(),
                CommandSource::KeyBinding,
            )
            .unwrap();

        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert!(owner.runtime.test_inputs.is_empty());
        assert!(owner.process_pending_pastes());
        assert_eq!(owner.runtime.test_inputs.len(), 1);
        assert!(owner.runtime.test_inputs[0].1.len() <= super::PASTE_CHUNK_BYTES + 6);

        owner.enqueue_command_list(
            client,
            parse_command_text("set-option -g @paste-progress yes").unwrap(),
            CommandSource::KeyBinding,
        );
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));
        assert_eq!(
            owner
                .runtime
                .state
                .option(OptionTarget::Server, "@paste-progress")
                .unwrap(),
            OptionValue::String("yes".to_string())
        );

        while owner.process_pending_pastes() {}
        assert!(owner.pending_pastes.is_empty());
        let joined = owner
            .runtime
            .test_inputs
            .iter()
            .flat_map(|(_, chunk)| chunk.iter().copied())
            .collect::<Vec<_>>();
        assert!(joined.starts_with(b"\x1b[200~"));
        assert!(joined.ends_with(b"\x1b[201~"));
        assert_eq!(&joined[6..joined.len() - 6], bytes);
        assert_eq!(
            joined
                .windows(6)
                .filter(|window| *window == b"\x1b[200~")
                .count(),
            1
        );
        assert_eq!(
            joined
                .windows(6)
                .filter(|window| *window == b"\x1b[201~")
                .count(),
            1
        );
    }

    #[test]
    fn pending_pastes_remain_fifo_for_each_pane() {
        let (mut owner, _, pane) = attached_owner();
        let first = vec![b'a'; super::PASTE_CHUNK_BYTES + 1];
        owner.pending_pastes.push_back(super::PendingPaste {
            pane,
            bytes: first.clone().into(),
            offset: 0,
            prefix_sent: false,
            suffix_sent: false,
            bracketed: false,
        });
        owner.pending_pastes.push_back(super::PendingPaste {
            pane,
            bytes: Vec::from(b"SECOND".as_slice()).into(),
            offset: 0,
            prefix_sent: false,
            suffix_sent: false,
            bracketed: false,
        });

        assert!(owner.process_pending_pastes());
        assert_eq!(owner.runtime.test_inputs.len(), 1);
        assert!(owner.process_pending_pastes());
        assert_eq!(owner.runtime.test_inputs.len(), 2);
        assert!(owner.process_pending_pastes());
        assert_eq!(owner.runtime.test_inputs.len(), 3);

        let joined = owner
            .runtime
            .test_inputs
            .iter()
            .flat_map(|(_, chunk)| chunk.iter().copied())
            .collect::<Vec<_>>();
        assert_eq!(&joined[..first.len()], first);
        assert_eq!(&joined[first.len()..], b"SECOND");
    }

    #[test]
    fn pending_outbound_frame_defers_the_next_baseline_advance() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("blocked", 80, 24);
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, _outbound_rx) = mpsc::channel(1);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.try_enqueue(Outbound::Message(wmux_protocol::Message::Output(vec![1])))
            .unwrap();
        view.attached = true;
        view.request_immediate_render(Instant::now());
        owner.clients.insert(client, view);

        assert!(!owner.render_due_clients(Instant::now()));
        let view = &owner.clients[&client];
        assert!(view.queued_bytes > 0);
        assert!(view.full_render);
        assert!(view.consumed_generations.is_empty());
    }

    #[tokio::test]
    async fn async_ipc_codec_handles_fragmented_frames() {
        use tokio::io::AsyncWriteExt;

        let message = wmux_protocol::Message::Output(vec![b'x'; 32 * 1024]);
        let frame = wmux_protocol::encode_frame(&message);
        let (mut writer, mut reader) = tokio::io::duplex(64 * 1024);
        let write = tokio::spawn(async move {
            for chunk in frame.chunks(17) {
                writer.write_all(chunk).await.unwrap();
            }
        });

        assert_eq!(
            read_async_message(&mut reader).await.unwrap(),
            Some(message)
        );
        write.await.unwrap();
    }

    #[tokio::test]
    async fn async_ipc_writer_emits_the_existing_protocol_format() {
        let message = wmux_protocol::Message::Key(wmux_protocol::WireKeyEvent {
            code: wmux_protocol::WireKeyCode::Char('a'),
            modifiers: wmux_protocol::WireKeyModifiers::NONE,
            raw: b"abc".to_vec(),
        });
        let (mut writer, mut reader) = tokio::io::duplex(64);
        write_async_message(&mut writer, message.clone())
            .await
            .unwrap();
        assert_eq!(
            read_async_message(&mut reader).await.unwrap(),
            Some(message)
        );
    }

    #[test]
    fn adjacent_output_chunks_are_coalesced() {
        let (mut backend, handle) = TestPtyBackend::pair();
        let pane = PlatformPaneId::new(1);
        handle.emit(
            pane,
            PlatformEvent::PtyOutput {
                pane,
                bytes: b"abc".to_vec(),
            },
        );
        handle.emit(
            pane,
            PlatformEvent::PtyOutput {
                pane,
                bytes: b"def".to_vec(),
            },
        );

        let collected = collect_pane_events(&mut backend, pane, 64, PER_PANE_OUTPUT_TIME);

        assert_eq!(collected.bytes, b"abcdef");
    }

    #[test]
    fn exit_and_eof_are_coalesced_and_release_the_platform_pane() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("exit-race", 80, 24);
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        let pane = PlatformPaneId::new(created.pane.raw());
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyExited {
                pane,
                exit_code: None,
            },
        );
        assert!(
            !runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        assert!(runtime.platform_panes.contains_key(&created.pane));

        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyExited {
                pane,
                exit_code: Some(23),
            },
        );
        runtime.emit_test_platform_event(created.pane, PlatformEvent::PtyClosed { pane });

        assert!(
            runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        let screen = runtime.state.pane(created.pane).unwrap().screen.grid();
        let text = (0..screen.rows())
            .filter_map(|row| screen.line(row))
            .map(|line| line.text())
            .collect::<String>();
        assert_eq!(text.matches("[wmux pane exited code=23]").count(), 1);
        assert!(!runtime.platform_panes.contains_key(&created.pane));
        assert!(!runtime.output_ring.contains(&created.pane));
    }

    #[test]
    fn malformed_and_out_of_order_platform_events_are_bounded_and_final() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("malformed-events", 80, 24);
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        let pane = PlatformPaneId::new(created.pane.raw());
        let unknown = PlatformPaneId::new(999_999);

        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyOutput {
                pane: unknown,
                bytes: b"wrong-pane".to_vec(),
            },
        );
        assert!(
            !runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        assert!(runtime.platform_panes.contains_key(&created.pane));

        runtime.emit_test_platform_event(created.pane, PlatformEvent::PtyClosed { pane });
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyOutput {
                pane,
                bytes: b"after-close".to_vec(),
            },
        );
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyExited {
                pane,
                exit_code: Some(7),
            },
        );

        assert!(
            runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        assert!(
            !runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        let screen = runtime.state.pane(created.pane).unwrap().screen.grid();
        let text = (0..screen.rows())
            .filter_map(|row| screen.line(row))
            .map(|line| line.text())
            .collect::<String>();
        assert!(!text.contains("wrong-pane"));
        assert!(!text.contains("after-close"));
        assert_eq!(text.matches("[wmux pane exited]").count(), 1);
        assert!(!runtime.platform_panes.contains_key(&created.pane));
    }

    #[test]
    fn detached_pane_output_is_applied_to_authoritative_screen() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyOutput {
                pane: PlatformPaneId::new(created.pane.raw()),
                bytes: b"detached".to_vec(),
            },
        );

        assert!(
            runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        assert!(runtime.state.clients.is_empty());
        assert!(runtime
            .state
            .pane(created.pane)
            .unwrap()
            .screen
            .grid()
            .line(0)
            .unwrap()
            .text()
            .starts_with("detached"));
    }

    #[test]
    fn layout_transaction_resizes_only_changed_platform_panes() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 122, 24);
        let second = runtime
            .state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                122,
                24,
            )
            .unwrap();
        let third = runtime
            .state
            .split_pane(
                created.window,
                Some(second),
                SplitDirection::LeftRight,
                122,
                24,
            )
            .unwrap();
        runtime.state.take_pending_pane_resizes();

        for pane in [created.pane, second, third] {
            let rect = runtime.state.pane(pane).unwrap().rect;
            runtime.add_test_platform_pane(pane, TerminalSize::new(rect.cols, rect.rows));
        }

        runtime
            .state
            .resize_window(created.window, 123, 24)
            .unwrap();

        assert_eq!(runtime.apply_pending_pane_resizes().unwrap(), 1);
        assert!(!runtime.resize_repaint_holds.contains_key(&created.pane));
        assert!(!runtime.resize_repaint_holds.contains_key(&second));
        assert!(runtime.resize_repaint_holds.contains_key(&third));
    }

    #[test]
    fn attached_window_reserves_one_terminal_row_for_server_ui() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let client = runtime.state.add_client();
        let created = runtime.state.create_session("status", 80, 25);
        runtime
            .state
            .attach_client(client, created.session)
            .unwrap();

        runtime.resize_client_window(client, TerminalSize::new(80, 25));

        let pane = runtime.state.pane(created.pane).unwrap();
        assert_eq!(pane.rect, wmux_core::Rect::new(0, 0, 80, 24));
        assert_eq!(pane.screen.rows(), 24);
    }

    #[test]
    fn resized_pane_output_is_staged_until_synchronized_commit() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        runtime.state.resize_window(created.window, 79, 24).unwrap();
        runtime.apply_pending_pane_resizes().unwrap();

        let partial = runtime.apply_pty_output(created.pane, b"\x1b[?2026h\x1b[2J");
        assert!(partial.changed);
        assert!(!partial.publishable);
        assert!(runtime.resize_repaint_holds.contains_key(&created.pane));

        let commit = runtime.apply_pty_output(created.pane, b"settled\x1b[?2026l");
        assert!(commit.changed);
        assert!(commit.publishable);
        assert!(commit.synchronized_commit);
        assert!(!runtime.resize_repaint_holds.contains_key(&created.pane));
    }

    #[test]
    fn synchronized_commit_is_detected_inside_one_pty_chunk() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);

        let output =
            runtime.apply_pty_output(created.pane, b"\x1b[?2026h\x1b[2Jsettled\x1b[?2026l");

        assert!(output.changed);
        assert!(output.publishable);
        assert!(output.synchronized_commit);
        assert!(!runtime
            .state
            .pane(created.pane)
            .unwrap()
            .screen
            .synchronized_output());
    }

    #[test]
    fn resized_pane_output_is_staged_until_quiet_deadline() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        runtime.state.resize_window(created.window, 79, 24).unwrap();
        runtime.apply_pending_pane_resizes().unwrap();

        let output = runtime.apply_pty_output(created.pane, b"repaint");
        assert!(output.changed);
        assert!(!output.publishable);
        let quiet_until = runtime
            .resize_repaint_holds
            .get(&created.pane)
            .unwrap()
            .quiet_until;

        assert!(!runtime.expire_resize_repaint_holds(quiet_until - Duration::from_nanos(1)));
        assert!(runtime.expire_resize_repaint_holds(quiet_until));
        assert!(!runtime.resize_repaint_holds.contains_key(&created.pane));
        assert!(runtime
            .state
            .pane(created.pane)
            .unwrap()
            .screen
            .damage_journal()
            .back()
            .is_some_and(|batch| batch.operations.contains(&wmux_core::DamageOperation::Full)));
    }

    #[test]
    fn pane_close_releases_a_resize_deferred_final_frame() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        runtime.state.resize_window(created.window, 79, 24).unwrap();
        runtime.apply_pending_pane_resizes().unwrap();
        let platform_pane = PlatformPaneId::new(created.pane.raw());
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyOutput {
                pane: platform_pane,
                bytes: b"final-before-close".to_vec(),
            },
        );
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyExited {
                pane: platform_pane,
                exit_code: Some(0),
            },
        );
        runtime.emit_test_platform_event(
            created.pane,
            PlatformEvent::PtyClosed {
                pane: platform_pane,
            },
        );

        let result = runtime.process_output_round(OutputBudget::default());

        assert!(result.changed);
        assert!(result.publishable);
        assert!(!runtime.resize_repaint_holds.contains_key(&created.pane));
        assert!(runtime
            .state
            .pane(created.pane)
            .unwrap()
            .screen
            .grid()
            .line(0)
            .unwrap()
            .text()
            .contains("final-before-close"));
    }

    #[test]
    fn noisy_panes_are_serviced_round_robin_with_byte_budgets() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        let second = runtime
            .state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                80,
                24,
            )
            .unwrap();
        runtime.add_test_platform_pane(created.pane, TerminalSize::new(40, 24));
        runtime.add_test_platform_pane(second, TerminalSize::new(39, 24));
        for _ in 0..6 {
            runtime.emit_test_platform_event(
                created.pane,
                PlatformEvent::PtyOutput {
                    pane: PlatformPaneId::new(created.pane.raw()),
                    bytes: b"aaaa".to_vec(),
                },
            );
        }
        runtime.emit_test_platform_event(
            second,
            PlatformEvent::PtyOutput {
                pane: PlatformPaneId::new(second.raw()),
                bytes: b"z".to_vec(),
            },
        );

        runtime.process_output_round(OutputBudget {
            per_pane_bytes: 4,
            per_pane_time: Duration::from_secs(1),
            round_time: Duration::from_secs(1),
        });

        assert_eq!(
            runtime.state.pane(created.pane).unwrap().screen.cursor().1,
            4
        );
        assert_eq!(runtime.state.pane(second).unwrap().screen.cursor().1, 1);
    }

    #[test]
    fn queued_pane_output_does_not_block_control_events() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(4);
        owner.clients.insert(
            client,
            ClientView::new(outbound_tx, TerminalCapabilities::default()),
        );

        let created = owner.runtime.state.create_session("test", 80, 24);
        owner
            .runtime
            .add_test_platform_pane(created.pane, TerminalSize::new(80, 24));
        for _ in 0..1_000 {
            owner.runtime.emit_test_platform_event(
                created.pane,
                PlatformEvent::PtyOutput {
                    pane: PlatformPaneId::new(created.pane.raw()),
                    bytes: vec![b'x'],
                },
            );
        }

        owner
            .handle_event(ServerEvent::Command {
                client,
                command: Command::ListSessions,
            })
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        let Outbound::Message(wmux_protocol::Message::CommandOk(response)) =
            outbound_rx.try_recv().unwrap()
        else {
            panic!("expected command response");
        };
        assert!(response.contains("test"));
    }

    #[test]
    fn clients_consume_pane_generations_independently() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("shared", 80, 24);
        let first = owner.runtime.state.add_client();
        let second = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(first, created.session)
            .unwrap();
        owner
            .runtime
            .state
            .attach_client(second, created.session)
            .unwrap();

        let (first_tx, mut first_rx) = mpsc::channel(4);
        let (second_tx, mut second_rx) = mpsc::channel(4);
        let mut first_view = ClientView::new(first_tx, TerminalCapabilities::default());
        first_view.attached = true;
        first_view.request_immediate_render(Instant::now());
        let mut second_view = ClientView::new(second_tx, TerminalCapabilities::default());
        second_view.attached = true;
        second_view.request_immediate_render(Instant::now());
        owner.clients.insert(first, first_view);
        owner.clients.insert(second, second_view);

        {
            let pane = owner.runtime.state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, b"one");
        }
        assert!(owner.render_due_clients(Instant::now()));
        let first_frame = first_rx.try_recv().unwrap();
        let _second_frame = second_rx.try_recv().unwrap();
        owner
            .clients
            .get_mut(&first)
            .unwrap()
            .drained(first_frame.wire_len());
        let first_generation = owner.clients[&first].consumed_generations[&created.pane];
        assert_eq!(
            owner.clients[&second].consumed_generations[&created.pane],
            first_generation
        );

        {
            let pane = owner.runtime.state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, b"two");
        }
        owner.request_all_attached_renders(RenderCause::Output);
        assert!(owner.render_due_clients(Instant::now() + Duration::from_millis(10)));

        assert!(owner.clients[&first].consumed_generations[&created.pane] > first_generation);
        assert_eq!(
            owner.clients[&second].consumed_generations[&created.pane],
            first_generation
        );
    }

    #[test]
    fn client_outbox_enforces_a_byte_budget_not_only_a_message_count() {
        let (outbound_tx, _outbound_rx) = mpsc::channel(64);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        let first = Outbound::Message(wmux_protocol::Message::Output(vec![0; 3 * 1024 * 1024]));
        assert!(view.try_enqueue(first).is_ok());
        assert!(matches!(
            view.try_enqueue(Outbound::Message(wmux_protocol::Message::Output(vec![
                0;
                2 * 1024 * 1024
            ]))),
            Err(mpsc::error::TrySendError::Full(_))
        ));
        assert!(view.queued_bytes <= super::CLIENT_OUTPUT_BYTES);
    }

    #[test]
    fn pending_render_coalesces_damage_until_the_client_drains() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("coalesce", 80, 24);
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(8);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        view.request_immediate_render(Instant::now());
        owner.clients.insert(client, view);

        assert!(owner.render_due_clients(Instant::now()));
        let first = outbound_rx.try_recv().unwrap();
        let first_generation = owner.clients[&client].consumed_generations[&created.pane];

        for bytes in [b"one".as_slice(), b"two", b"three"] {
            let pane = owner.runtime.state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, bytes);
            owner.request_all_attached_renders(RenderCause::Output);
        }
        assert!(!owner.render_due_clients(Instant::now() + Duration::from_millis(20)));
        assert_eq!(
            owner.clients[&client].consumed_generations[&created.pane],
            first_generation
        );

        owner.handle_owner_message(super::OwnerMessage::OutboundDrained {
            client,
            bytes: first.wire_len(),
        });
        assert!(owner.render_due_clients(Instant::now()));
        assert_eq!(outbound_rx.len(), 1);
        assert!(owner.clients[&client].consumed_generations[&created.pane] > first_generation);
    }

    #[test]
    fn killing_final_pane_sends_explicit_attached_client_exit() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let created = owner.runtime.state.create_session("last-pane", 80, 24);
        let client = owner.runtime.state.add_client();
        owner
            .runtime
            .state
            .attach_client(client, created.session)
            .unwrap();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(4);
        let mut view = ClientView::new(outbound_tx, TerminalCapabilities::default());
        view.attached = true;
        owner.clients.insert(client, view);

        owner
            .handle_event(ServerEvent::Command {
                client,
                command: Command::KillPane,
            })
            .unwrap();
        assert!(owner.process_command_queue(COMMANDS_PER_TURN));

        assert!(!owner.clients.contains_key(&client));
        assert!(!owner.runtime.state.sessions.contains_key(&created.session));
        assert!(matches!(
            outbound_rx.try_recv(),
            Ok(Outbound::Message(wmux_protocol::Message::CommandOk(_)))
        ));
        assert!(matches!(
            outbound_rx.try_recv(),
            Ok(Outbound::Shutdown(wmux_protocol::Message::Shutdown))
        ));
    }
}
