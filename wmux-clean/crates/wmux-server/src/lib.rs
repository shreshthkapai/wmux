use std::{
    collections::{BTreeMap, VecDeque},
    fs::OpenOptions,
    io::{self, IoSlice, Write},
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
    runtime::{Builder as RuntimeBuilder, Handle as TokioHandle},
    sync::{
        mpsc as async_mpsc,
        mpsc::error::{TryRecvError as AsyncTryRecvError, TrySendError},
        oneshot,
    },
    task::JoinSet,
};
use wmux_config::{config_path, WmuxConfig};
use wmux_core::{
    build_window_scene_with_viewports, build_window_structure, execute, parse_command,
    render_damage_from_structure, render_diff_scene_with_capabilities, ClientId, ClientInput,
    Command, CommandQueue, CopyMode, CopyModeResult, Line, PaneId, PaneSceneOverrides,
    PaneViewport, RenderCapabilities, RenderState, RetainedPaneFrame, ServerEvent, ServerState,
    StructuralScene, Style,
};
use wmux_platform::{
    MouseButton, MouseEvent, MouseEventKind, PlatformEvent, PlatformPaneId, TerminalSize,
};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    FRAME_HEADER_LEN, MAX_FRAME, VERSION,
};
use wmux_windows::{
    conpty::{spawn_shell, ConptyPane, PlatformEventReceiver, PlatformNotifier},
    pipe::{verify_client, Endpoint, NamedPipeServer, ServerLock, ServerPipeFactory, UserSid},
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
const CLIENT_OUTPUT_MESSAGES: usize = 64;
const CLIENT_OUTPUT_BYTES: usize = 4 * 1024 * 1024;
const CLIENT_MAX_FRAME_BYTES: usize = FRAME_HEADER_LEN + MAX_FRAME;
const PER_PANE_OUTPUT_BYTES: usize = 64 * 1024;
const PER_PANE_OUTPUT_TIME: Duration = Duration::from_millis(1);
const OUTPUT_ROUND_TIME: Duration = Duration::from_millis(4);
const CONNECTION_DRAIN_TIMEOUT: Duration = Duration::from_secs(5);

pub fn run() -> io::Result<()> {
    RuntimeBuilder::new_multi_thread()
        .worker_threads(2)
        .thread_name("wmux-io")
        .enable_all()
        .build()?
        .block_on(run_async())
}

async fn run_async() -> io::Result<()> {
    let endpoint = Endpoint::current_user()?;
    run_async_at(endpoint).await
}

async fn run_async_at(endpoint: Endpoint) -> io::Result<()> {
    let _lock = ServerLock::acquire(&endpoint)?;
    let config = load_config();
    eprintln!("wmux config {}", config_path().display());

    let endpoint_name = endpoint.pipe_name().to_string();
    let owner_sid = endpoint.owner_sid().clone();
    let mut pipe_factory = ServerPipeFactory::new(endpoint)?;
    let mut listener = pipe_factory.create()?;

    let (owner_tx, owner_rx) = mpsc::channel();
    let state_owner_tx = owner_tx.clone();
    let (shutdown_tx, mut shutdown_rx) = oneshot::channel();
    let io = TokioHandle::current();
    let state_owner = thread::Builder::new()
        .name("wmux-state-owner".to_string())
        .spawn(move || ServerOwner::new(config, io, state_owner_tx, shutdown_tx).run(owner_rx))?;

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
            accepted = listener.connect() => {
                if let Err(error) = accepted {
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
                    listener = match pipe_factory.create() {
                        Ok(listener) => listener,
                        Err(error) => break 'accept Err(error),
                    };
                    continue;
                }

                let next_listener = match pipe_factory.create() {
                    Ok(listener) => listener,
                    Err(error) => break 'accept Err(error),
                };
                let connected = std::mem::replace(&mut listener, next_listener);
                let connection_owner = owner_tx.clone();
                let connection_sid = owner_sid.clone();
                connections.spawn(async move {
                    handle_connection(connected, connection_owner, connection_sid).await
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
    InvalidCommand {
        client: ClientId,
        message: String,
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
    mut pipe: NamedPipeServer,
    owner_tx: mpsc::Sender<OwnerMessage>,
    owner_sid: UserSid,
) -> io::Result<()> {
    let capabilities = handshake(&mut pipe, &owner_sid).await?;

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

    let result = connection_io_loop(pipe, outbound_rx, &owner_tx, client).await;
    let _ = owner_tx.send(OwnerMessage::Disconnect(client));
    result
}

async fn connection_io_loop(
    pipe: NamedPipeServer,
    outbound_rx: async_mpsc::Receiver<Outbound>,
    owner_tx: &mpsc::Sender<OwnerMessage>,
    client: ClientId,
) -> io::Result<()> {
    let (mut reader, mut writer) = tokio::io::split(pipe);
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
            Ok(command) => ServerEvent::Command { client, command },
            Err(error) => {
                return Some(OwnerMessage::InvalidCommand {
                    client,
                    message: error.to_string(),
                });
            }
        },
        Message::Input(bytes) | Message::Key(bytes) => ServerEvent::ClientInput {
            client,
            input: ClientInput::Bytes(bytes),
        },
        Message::Paste(bytes) => ServerEvent::ClientInput {
            client,
            input: ClientInput::Paste(bytes),
        },
        Message::Mouse(event) => ServerEvent::ClientMouse { client, event },
        Message::Resize { cols, rows } => ServerEvent::ClientResize { client, cols, rows },
        Message::Detach => ServerEvent::Command {
            client,
            command: Command::DetachClient,
        },
        Message::Shutdown => ServerEvent::Command {
            client,
            command: Command::KillServer,
        },
        _ => return None,
    };
    Some(OwnerMessage::Event(event))
}

async fn handshake(
    pipe: &mut NamedPipeServer,
    owner_sid: &UserSid,
) -> io::Result<TerminalCapabilities> {
    match read_async_message(&mut *pipe).await? {
        Some(Message::Hello {
            version,
            capabilities,
            ..
        }) if version == VERSION => {
            verify_client(pipe, owner_sid)?;
            write_async_message(
                pipe,
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
                pipe,
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
                pipe,
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

fn parse_command_line(raw: &str) -> io::Result<Command> {
    let argv = raw
        .split_whitespace()
        .map(ToString::to_string)
        .collect::<Vec<_>>();
    parse_command(&argv).map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error.0))
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
    io: Option<TokioHandle>,
    owner_tx: Option<mpsc::Sender<OwnerMessage>>,
}

#[derive(Clone, Copy, Default)]
struct OutputResult {
    changed: bool,
    publishable: bool,
    synchronized_commit: bool,
}

impl Runtime {
    fn with_config(config: WmuxConfig) -> Self {
        Self {
            state: ServerState::new(),
            queue: CommandQueue::default(),
            config,
            platform_panes: BTreeMap::new(),
            output_ring: VecDeque::new(),
            sync_started_at: BTreeMap::new(),
            resize_repaint_holds: BTreeMap::new(),
            history_growth: Vec::new(),
            io: None,
            owner_tx: None,
        }
    }

    fn with_native_io(
        config: WmuxConfig,
        io: TokioHandle,
        owner_tx: mpsc::Sender<OwnerMessage>,
    ) -> Self {
        let mut runtime = Self::with_config(config);
        runtime.io = Some(io);
        runtime.owner_tx = Some(owner_tx);
        runtime
    }

    fn submit(&mut self, client: ClientId, command: Command) -> wmux_core::CommandResult {
        let sequence = self.queue.push(client, command);
        let queued = self.queue.pop().expect("queued command exists");
        debug_assert_eq!(queued.sequence, sequence);
        execute(&mut self.state, queued)
    }

    fn ensure_platform_pane(&mut self, pane_id: PaneId, size: TerminalSize) -> io::Result<()> {
        if self.platform_panes.contains_key(&pane_id) {
            return Ok(());
        }
        let env_overrides = self.config.pane_environment(pane_id.raw());
        let io = self
            .io
            .as_ref()
            .ok_or_else(|| io::Error::other("native I/O runtime is unavailable"))?;
        let owner_tx = self
            .owner_tx
            .as_ref()
            .ok_or_else(|| io::Error::other("state owner notifier is unavailable"))?
            .clone();
        let notify: PlatformNotifier = Arc::new(move |_| {
            let _ = owner_tx.send(OwnerMessage::PlatformReady);
        });
        let (conpty, rx) = spawn_shell(
            PlatformPaneId::new(pane_id.raw()),
            size,
            &env_overrides,
            io,
            notify,
        )?;
        eprintln!(
            "spawned conpty pane={} pid={}",
            pane_id.raw(),
            conpty.process_id()
        );
        self.platform_panes
            .insert(pane_id, PlatformPane::live(conpty, rx, size));
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
        self.state.resize_window(window, size.cols, size.rows);
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
        if let Some(conpty) = platform.conpty.as_mut() {
            conpty.resize(size)?;
        }
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
        self.platform_panes
            .retain(|pane, _| self.state.panes.contains_key(pane));
        self.output_ring
            .retain(|pane| self.platform_panes.contains_key(pane));
        self.sync_started_at
            .retain(|pane, _| self.state.panes.contains_key(pane));
        self.resize_repaint_holds
            .retain(|pane, _| self.state.panes.contains_key(pane));
    }

    fn shutdown_platform_panes(&mut self) {
        for platform in self.platform_panes.values_mut() {
            if let Some(conpty) = platform.conpty.as_mut() {
                conpty.terminate(1);
            }
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
        if let Some(conpty) = self
            .platform_panes
            .get_mut(&pane_id)
            .and_then(|platform| platform.conpty.as_mut())
        {
            conpty.write_input(bytes)?;
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
            let collected = {
                let Some(platform) = self.platform_panes.get_mut(&pane_id) else {
                    continue;
                };
                collect_pane_events(
                    &mut platform.rx,
                    budget.per_pane_bytes,
                    budget.per_pane_time,
                )
            };
            let (still_polling, completed) = {
                let platform = self
                    .platform_panes
                    .get_mut(&pane_id)
                    .expect("platform pane still exists");
                if collected.disconnected {
                    platform.disconnected = true;
                }
                let first_exit = collected.exit_code.is_some() && !platform.exited;
                if first_exit {
                    platform.exited = true;
                    if let Some(conpty) = platform.conpty.as_mut() {
                        if collected.exit_code.flatten().is_some() {
                            conpty.finish_after_process_exit();
                        } else {
                            conpty.terminate(1);
                        }
                    }
                }
                let completed = platform.disconnected && platform.exited;
                (!completed, completed)
            };
            if still_polling {
                self.output_ring.push_back(pane_id);
            }
            let applied = self.apply_pty_output(pane_id, &collected.bytes);
            result.changed |= applied.changed;
            result.publishable |= applied.publishable;
            result.synchronized_commit |= applied.synchronized_commit;
            let exit_to_apply = match collected.exit_code {
                Some(Some(exit_code)) => Some(Some(exit_code)),
                Some(None) if collected.disconnected => Some(None),
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
                self.resize_repaint_holds.remove(&pane_id);
            }
        }
        result
    }
}

struct PlatformPane {
    conpty: Option<ConptyPane>,
    rx: PlatformEventReceiver,
    exited: bool,
    disconnected: bool,
    size: TerminalSize,
}

impl PlatformPane {
    fn live(conpty: ConptyPane, rx: PlatformEventReceiver, size: TerminalSize) -> Self {
        Self {
            conpty: Some(conpty),
            rx,
            exited: false,
            disconnected: false,
            size,
        }
    }

    #[cfg(test)]
    fn test(rx: PlatformEventReceiver, size: TerminalSize) -> Self {
        Self {
            conpty: None,
            rx,
            exited: false,
            disconnected: false,
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
    disconnected: bool,
}

fn collect_pane_events(
    rx: &mut PlatformEventReceiver,
    byte_budget: usize,
    time_budget: Duration,
) -> CollectedOutput {
    let started = Instant::now();
    let mut collected = CollectedOutput::default();
    while collected.bytes.len() < byte_budget && started.elapsed() < time_budget {
        match rx.try_recv() {
            Ok(PlatformEvent::PtyOutput { bytes, .. }) => collected.bytes.extend_from_slice(&bytes),
            Ok(PlatformEvent::PtyExited { exit_code, .. }) => {
                if collected.exit_code.is_none() || exit_code.is_some() {
                    collected.exit_code = Some(exit_code);
                }
            }
            Err(AsyncTryRecvError::Empty) => break,
            Err(AsyncTryRecvError::Disconnected) => {
                collected.disconnected = true;
                if collected.exit_code.is_none() {
                    collected.exit_code = Some(None);
                }
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
    shutdown: ShutdownState,
    shutdown_tx: Option<oneshot::Sender<()>>,
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
        io: TokioHandle,
        owner_tx: mpsc::Sender<OwnerMessage>,
        shutdown_tx: oneshot::Sender<()>,
    ) -> Self {
        Self {
            runtime: Runtime::with_native_io(config, io, owner_tx),
            clients: BTreeMap::new(),
            shutdown: ShutdownState::Running,
            shutdown_tx: Some(shutdown_tx),
        }
    }

    #[cfg(test)]
    fn new_test(config: WmuxConfig) -> Self {
        Self {
            runtime: Runtime::with_config(config),
            clients: BTreeMap::new(),
            shutdown: ShutdownState::Running,
            shutdown_tx: None,
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

            let output = self.runtime.process_output_round(OutputBudget::default());
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
            OwnerMessage::InvalidCommand { client, message } => {
                if !self.is_shutting_down() {
                    self.send_critical(client, Message::CommandErr(message));
                }
            }
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

    fn handle_event(&mut self, event: ServerEvent) -> io::Result<()> {
        match event {
            ServerEvent::PtyOutput { pane, bytes } => {
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
                        self.handle_copy_mode_key(client, &bytes);
                    }
                } else if let Some(pane) = self.active_pane_for_client(client) {
                    self.runtime.write_pane_input(pane, input)?;
                }
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
            ServerEvent::Command { client, command } => self.handle_command(client, command)?,
            ServerEvent::Timer { .. } => {}
        }
        Ok(())
    }

    fn handle_command(&mut self, client: ClientId, command: Command) -> io::Result<()> {
        if matches!(command, Command::CopyMode) {
            let result = self.enter_copy_mode(client);
            self.send_critical(
                client,
                match result {
                    Ok(()) => Message::CommandOk(String::new()),
                    Err(message) => Message::CommandErr(message),
                },
            );
            return Ok(());
        }
        let attach = is_attach_command(&command);
        let detach = matches!(command, Command::DetachClient);
        let shutdown = matches!(command, Command::KillServer);
        let result = self.runtime.submit(client, command);
        let response = if result.ok {
            Message::CommandOk(result.message.clone())
        } else {
            Message::CommandErr(result.message.clone())
        };

        if shutdown && result.ok {
            self.begin_shutdown(client, response);
            return Ok(());
        }
        self.send_critical(client, response);
        if !result.ok {
            return Ok(());
        }
        if detach {
            self.disconnect_client(client, false);
            return Ok(());
        }
        if self
            .close_clients_with_destroyed_sessions()
            .contains(&client)
        {
            return Ok(());
        }
        if attach {
            let size = self
                .clients
                .get(&client)
                .map_or(TerminalSize::new(80, 24), |view| view.size);
            if let Some(view) = self.clients.get_mut(&client) {
                view.attached = true;
                view.full_render = true;
                view.render_state.invalidate();
                view.structure = None;
                view.request_immediate_render(Instant::now());
            }
            self.commit_layout_transaction(client, size)?;
            return Ok(());
        }

        if self.clients.get(&client).is_some_and(|view| view.attached) {
            let size = self
                .clients
                .get(&client)
                .map_or(TerminalSize::new(80, 24), |view| view.size);
            self.commit_layout_transaction(client, size)?;
            if let Some(view) = self.clients.get_mut(&client) {
                view.request_render(Instant::now(), RenderCause::Structural);
            }
        } else if let Some(pane) = result.attached_pane {
            let size = self.pane_size(pane).unwrap_or(TerminalSize::new(80, 24));
            self.runtime.ensure_platform_pane(pane, size)?;
            self.runtime.apply_pending_pane_resizes()?;
        }
        Ok(())
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

    fn handle_copy_mode_key(&mut self, client: ClientId, bytes: &[u8]) {
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
        let result = mode.handle_key(bytes, &lines, rows);
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
                self.send_critical(client, Message::Clipboard(bytes));
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

    fn begin_shutdown(&mut self, requester: ClientId, response: Message) {
        if self.is_shutting_down() {
            return;
        }
        self.shutdown = ShutdownState::Draining;
        self.runtime.shutdown_platform_panes();

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
            .iter()
            .filter_map(|(client_id, view)| {
                (view.attached
                    && self
                        .runtime
                        .state
                        .clients
                        .get(client_id)
                        .is_none_or(|client| client.attached_session.is_none()))
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
            self.runtime.state.remove_client(*client_id);
        }
        clients
    }

    fn disconnect_client(&mut self, client: ClientId, submit_detach: bool) {
        if !self.clients.contains_key(&client) {
            return;
        }
        if submit_detach {
            let _ = self.runtime.submit(client, Command::DetachClient);
        }
        self.clients.remove(&client);
        self.runtime.state.remove_client(client);
    }
}

fn is_attach_command(command: &Command) -> bool {
    matches!(
        command,
        Command::NewSession { attach: true, .. } | Command::AttachSession { .. }
    )
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
    let (previous_frame_panes, retained_frames) = retained_panes_for_client(runtime, client);
    let viewports = pane_viewports_for_client(runtime, client, scroll_offsets, copy_mode);
    let Some(scene) = build_window_scene_with_viewports(
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
    let (previous_frame_panes, retained_frames) = retained_panes_for_client(runtime, client);
    let viewports = pane_viewports_for_client(runtime, client, scroll_offsets, copy_mode);
    let Some(scene) = build_window_scene_with_viewports(
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
        collect_pane_events, read_async_message, run_async_at, write_async_message,
        write_outbound_messages, ClientView, FrameScheduler, Outbound, OutputBudget, PlatformPane,
        RenderCause, Runtime, ServerOwner, PER_PANE_OUTPUT_TIME,
    };
    use std::time::{Duration, Instant};
    use tokio::sync::mpsc;
    use wmux_config::WmuxConfig;
    use wmux_core::{Command, ServerEvent, SplitDirection};
    use wmux_platform::{
        MouseButton, MouseEvent, MouseEventKind, MouseModifiers, PlatformEvent, PlatformPaneId,
        TerminalSize,
    };
    use wmux_protocol::{Message, TerminalCapabilities, VERSION};
    use wmux_windows::pipe::{connect_async, Endpoint, NamedPipeClient};

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
        let (_pane_tx, pane_rx) = mpsc::channel(1);
        owner.runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(pane_rx, TerminalSize::new(80, 24)),
        );
        owner.runtime.output_ring.push_back(created.pane);

        owner
            .handle_event(ServerEvent::Command {
                client: requester,
                command: Command::KillServer,
            })
            .unwrap();

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

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn shutdown_drains_clients_releases_lock_and_restarts() {
        let endpoint = Endpoint::for_instance(&unique_instance("shutdown-restart")).unwrap();
        let server_endpoint = endpoint.clone();
        let (server_thread, server_done) = start_test_server(server_endpoint);

        let mut requester = connect_test_client(&endpoint).await;
        write_async_message(
            &mut requester,
            Message::Command("new-session -d".to_string()),
        )
        .await
        .unwrap();
        assert!(matches!(
            read_until(&mut requester, |message| matches!(
                message,
                Message::CommandOk(_)
            ))
            .await,
            Message::CommandOk(_)
        ));

        let mut attached = connect_test_client(&endpoint).await;
        write_async_message(
            &mut attached,
            Message::Command("attach-session".to_string()),
        )
        .await
        .unwrap();
        assert!(matches!(
            read_until(&mut attached, |message| matches!(
                message,
                Message::CommandOk(_)
            ))
            .await,
            Message::CommandOk(_)
        ));

        write_async_message(&mut requester, Message::Command("kill-server".to_string()))
            .await
            .unwrap();
        assert!(matches!(
            read_until(&mut requester, |message| matches!(
                message,
                Message::CommandOk(_)
            ))
            .await,
            Message::CommandOk(_)
        ));
        assert_eq!(
            read_until(&mut attached, |message| matches!(
                message,
                Message::Shutdown
            ))
            .await,
            Message::Shutdown
        );
        drop(requester);
        drop(attached);
        await_test_server(server_thread, server_done).await;
        assert!(!endpoint.lock_path().exists());

        let restart_endpoint = endpoint.clone();
        let (restart_thread, restart_done) = start_test_server(restart_endpoint);
        let mut killer = connect_test_client(&endpoint).await;
        write_async_message(&mut killer, Message::Command("kill-server".to_string()))
            .await
            .unwrap();
        assert!(matches!(
            read_until(&mut killer, |message| matches!(
                message,
                Message::CommandOk(_)
            ))
            .await,
            Message::CommandOk(_)
        ));
        drop(killer);
        await_test_server(restart_thread, restart_done).await;
        assert!(!endpoint.lock_path().exists());
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 4)]
    async fn disconnect_after_hello_preserves_detached_session() {
        let endpoint = Endpoint::for_instance(&unique_instance("hello-disconnect")).unwrap();
        let (server_thread, server_done) = start_test_server(endpoint.clone());

        let mut controller = connect_test_client(&endpoint).await;
        write_async_message(
            &mut controller,
            Message::Command("new-session -d -s durable".to_string()),
        )
        .await
        .unwrap();
        assert!(matches!(
            read_until(&mut controller, |message| matches!(
                message,
                Message::CommandOk(_)
            ))
            .await,
            Message::CommandOk(_)
        ));

        let disposable = connect_test_client(&endpoint).await;
        drop(disposable);

        write_async_message(
            &mut controller,
            Message::Command("list-sessions".to_string()),
        )
        .await
        .unwrap();
        let listed = read_until(&mut controller, |message| {
            matches!(message, Message::CommandOk(_))
        })
        .await;
        assert!(matches!(listed, Message::CommandOk(text) if text.contains("durable:")));

        write_async_message(&mut controller, Message::Command("kill-server".to_string()))
            .await
            .unwrap();
        assert!(matches!(
            read_until(&mut controller, |message| matches!(
                message,
                Message::CommandOk(_)
            ))
            .await,
            Message::CommandOk(_)
        ));
        drop(controller);
        await_test_server(server_thread, server_done).await;
    }

    fn start_test_server(
        endpoint: Endpoint,
    ) -> (
        std::thread::JoinHandle<()>,
        tokio::sync::oneshot::Receiver<std::io::Result<()>>,
    ) {
        let (done_tx, done_rx) = tokio::sync::oneshot::channel();
        let server_thread = std::thread::spawn(move || {
            let runtime = tokio::runtime::Builder::new_multi_thread()
                .worker_threads(2)
                .enable_all()
                .build()
                .unwrap();
            let _ = done_tx.send(runtime.block_on(run_async_at(endpoint)));
        });
        (server_thread, done_rx)
    }

    async fn await_test_server(
        server_thread: std::thread::JoinHandle<()>,
        server_done: tokio::sync::oneshot::Receiver<std::io::Result<()>>,
    ) {
        tokio::time::timeout(Duration::from_secs(10), server_done)
            .await
            .expect("server did not finish graceful shutdown")
            .expect("server thread dropped completion")
            .expect("server returned an error");
        server_thread.join().expect("server thread panicked");
    }

    async fn connect_test_client(endpoint: &Endpoint) -> NamedPipeClient {
        let mut last_error = None;
        for _ in 0..100 {
            match connect_async(endpoint) {
                Ok(mut pipe) => {
                    write_async_message(
                        &mut pipe,
                        Message::Hello {
                            version: VERSION,
                            pid: std::process::id(),
                            capabilities: TerminalCapabilities::default(),
                        },
                    )
                    .await
                    .unwrap();
                    assert!(matches!(
                        read_async_message(&mut pipe).await.unwrap(),
                        Some(Message::HelloOk {
                            version: VERSION,
                            ..
                        })
                    ));
                    return pipe;
                }
                Err(error) => {
                    last_error = Some(error);
                    tokio::time::sleep(Duration::from_millis(20)).await;
                }
            }
        }
        panic!("could not connect test client: {:?}", last_error)
    }

    async fn read_until(
        pipe: &mut NamedPipeClient,
        predicate: impl Fn(&Message) -> bool,
    ) -> Message {
        tokio::time::timeout(Duration::from_secs(5), async {
            loop {
                let message = read_async_message(&mut *pipe)
                    .await
                    .expect("failed to read server message")
                    .expect("server closed before expected message");
                if predicate(&message) {
                    return message;
                }
            }
        })
        .await
        .expect("timed out waiting for server message")
    }

    fn unique_instance(label: &str) -> String {
        use std::time::{SystemTime, UNIX_EPOCH};

        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock is after Unix epoch")
            .as_nanos();
        format!("{label}-{}-{nonce}", std::process::id())
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
        owner.handle_copy_mode_key(first, b"g");
        owner.handle_copy_mode_key(first, b" ");
        owner.handle_copy_mode_key(first, b"$");
        owner.handle_copy_mode_key(first, b"\r");

        assert!(owner.clients[&first].copy_mode.is_none());
        let outbound = first_rx.try_recv().unwrap();
        assert!(matches!(
            outbound,
            Outbound::Message(wmux_protocol::Message::Clipboard(bytes)) if bytes == b"alpha"
        ));
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
        let message = wmux_protocol::Message::Key(b"abc".to_vec());
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
        let (tx, mut rx) = mpsc::channel(4);
        let pane = PlatformPaneId::new(1);
        tx.try_send(PlatformEvent::PtyOutput {
            pane,
            bytes: b"abc".to_vec(),
        })
        .unwrap();
        tx.try_send(PlatformEvent::PtyOutput {
            pane,
            bytes: b"def".to_vec(),
        })
        .unwrap();

        let collected = collect_pane_events(&mut rx, 64, PER_PANE_OUTPUT_TIME);

        assert_eq!(collected.bytes, b"abcdef");
    }

    #[test]
    fn exit_and_eof_are_coalesced_and_release_the_platform_pane() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("exit-race", 80, 24);
        let (tx, rx) = mpsc::channel(4);
        runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(rx, TerminalSize::new(80, 24)),
        );
        runtime.output_ring.push_back(created.pane);
        let pane = PlatformPaneId::new(created.pane.raw());
        tx.try_send(PlatformEvent::PtyExited {
            pane,
            exit_code: None,
        })
        .unwrap();
        assert!(
            !runtime
                .process_output_round(OutputBudget::default())
                .changed
        );
        assert!(runtime.platform_panes.contains_key(&created.pane));

        tx.try_send(PlatformEvent::PtyExited {
            pane,
            exit_code: Some(23),
        })
        .unwrap();
        drop(tx);

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
    fn pane_queue_overflow_is_explicit_backpressure() {
        let (tx, _rx) = mpsc::channel(2);
        let pane = PlatformPaneId::new(1);
        tx.try_send(PlatformEvent::PtyOutput {
            pane,
            bytes: vec![1],
        })
        .unwrap();
        tx.try_send(PlatformEvent::PtyOutput {
            pane,
            bytes: vec![2],
        })
        .unwrap();
        assert!(matches!(
            tx.try_send(PlatformEvent::PtyOutput {
                pane,
                bytes: vec![3],
            }),
            Err(mpsc::error::TrySendError::Full(_))
        ));
    }

    #[test]
    fn detached_pane_output_is_applied_to_authoritative_screen() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        let (tx, rx) = mpsc::channel(4);
        runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(rx, TerminalSize::new(80, 24)),
        );
        runtime.output_ring.push_back(created.pane);
        tx.try_send(PlatformEvent::PtyOutput {
            pane: PlatformPaneId::new(created.pane.raw()),
            bytes: b"detached".to_vec(),
        })
        .unwrap();

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

        let mut senders = Vec::new();
        for pane in [created.pane, second, third] {
            let (tx, rx) = mpsc::channel(1);
            senders.push(tx);
            let rect = runtime.state.pane(pane).unwrap().rect;
            runtime.platform_panes.insert(
                pane,
                PlatformPane::test(rx, TerminalSize::new(rect.cols, rect.rows)),
            );
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
    fn resized_pane_output_is_staged_until_synchronized_commit() {
        let mut runtime = Runtime::with_config(WmuxConfig::default());
        let created = runtime.state.create_session("test", 80, 24);
        let (_tx, rx) = mpsc::channel(1);
        runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(rx, TerminalSize::new(80, 24)),
        );
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
        let (_tx, rx) = mpsc::channel(1);
        runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(rx, TerminalSize::new(80, 24)),
        );
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
        let (first_tx, first_rx) = mpsc::channel(8);
        let (second_tx, second_rx) = mpsc::channel(8);
        runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(first_rx, TerminalSize::new(40, 24)),
        );
        runtime.platform_panes.insert(
            second,
            PlatformPane::test(second_rx, TerminalSize::new(39, 24)),
        );
        runtime.output_ring.extend([created.pane, second]);
        for _ in 0..6 {
            first_tx
                .try_send(PlatformEvent::PtyOutput {
                    pane: PlatformPaneId::new(created.pane.raw()),
                    bytes: b"aaaa".to_vec(),
                })
                .unwrap();
        }
        second_tx
            .try_send(PlatformEvent::PtyOutput {
                pane: PlatformPaneId::new(second.raw()),
                bytes: b"z".to_vec(),
            })
            .unwrap();

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
    fn full_pane_queue_does_not_block_control_events() {
        let mut owner = ServerOwner::new_test(WmuxConfig::default());
        let client = owner.runtime.state.add_client();
        let (outbound_tx, mut outbound_rx) = mpsc::channel(4);
        owner.clients.insert(
            client,
            ClientView::new(outbound_tx, TerminalCapabilities::default()),
        );

        let created = owner.runtime.state.create_session("test", 80, 24);
        let (pane_tx, pane_rx) = mpsc::channel(1);
        pane_tx
            .try_send(PlatformEvent::PtyOutput {
                pane: PlatformPaneId::new(created.pane.raw()),
                bytes: vec![b'x'],
            })
            .unwrap();
        owner.runtime.platform_panes.insert(
            created.pane,
            PlatformPane::test(pane_rx, TerminalSize::new(80, 24)),
        );
        owner.runtime.output_ring.push_back(created.pane);

        owner
            .handle_event(ServerEvent::Command {
                client,
                command: Command::ListSessions,
            })
            .unwrap();

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
