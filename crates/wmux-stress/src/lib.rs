use std::{
    collections::{BTreeMap, VecDeque},
    fmt,
    sync::{Arc, Mutex},
    thread,
    time::{Duration, Instant},
};

use tokio::{
    io::{AsyncReadExt, AsyncWriteExt, DuplexStream},
    runtime::Builder as RuntimeBuilder,
    sync::mpsc,
};
use wmux_config::WmuxConfig;
use wmux_core::{pane_area_rows, Screen, TerminalEngine};
use wmux_platform::{
    AcceptedConnection, Endpoint, JobBackend, JobEvent, JobNotifier, JobRequest, PeerIdentity,
    PlatformError, PlatformErrorKind, PlatformEvent, PlatformFuture, PlatformJobId,
    PlatformNotifier, PlatformPaneId, PlatformRequest, PlatformResult, PtyBackend, ServerListener,
    ServerPlatform, TerminalSize,
};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    FRAME_HEADER_LEN, VERSION,
};

const IO_TIMEOUT: Duration = Duration::from_secs(10);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StressProfile {
    Ci,
    Full,
}

impl StressProfile {
    fn limits(self) -> Limits {
        match self {
            Self::Ci => Limits {
                attach_cycles: 25,
                resize_events: 200,
                clients: 8,
                panes: 8,
                history_lines: 2_000,
                paste_bytes: 256 * 1024,
                output_rounds: 16,
            },
            Self::Full => Limits {
                attach_cycles: 250,
                resize_events: 2_000,
                clients: 32,
                panes: 32,
                history_lines: 100_000,
                paste_bytes: 16 * 1024 * 1024,
                output_rounds: 96,
            },
        }
    }

    pub const fn name(self) -> &'static str {
        match self {
            Self::Ci => "ci",
            Self::Full => "full",
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StressCase {
    pub name: &'static str,
    pub operations: u64,
    pub fingerprint: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StressReport {
    pub profile: StressProfile,
    pub cases: Vec<StressCase>,
    pub suite_fingerprint: u64,
}

#[derive(Debug)]
pub struct StressError(String);

impl StressError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for StressError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for StressError {}

impl From<std::io::Error> for StressError {
    fn from(error: std::io::Error) -> Self {
        Self(error.to_string())
    }
}

#[derive(Clone, Copy)]
struct Limits {
    attach_cycles: usize,
    resize_events: usize,
    clients: usize,
    panes: usize,
    history_lines: usize,
    paste_bytes: usize,
    output_rounds: usize,
}

pub fn run_suite(profile: StressProfile) -> Result<StressReport, StressError> {
    let runtime = RuntimeBuilder::new_current_thread().enable_all().build()?;
    let limits = profile.limits();
    let cases = runtime.block_on(async {
        Ok::<_, StressError>(vec![
            lifecycle_scenario(limits).await?,
            event_pressure_scenario(limits).await?,
            fan_out_scenario(limits).await?,
            storage_scenario(limits).await?,
        ])
    })?;
    let mut hash = Fnv64::new();
    hash.bytes(profile.name().as_bytes());
    for case in &cases {
        hash.bytes(case.name.as_bytes());
        hash.u64(case.operations);
        hash.u64(case.fingerprint);
    }
    Ok(StressReport {
        profile,
        cases,
        suite_fingerprint: hash.finish(),
    })
}

async fn lifecycle_scenario(limits: Limits) -> Result<StressCase, StressError> {
    let harness = MemoryHarness::start("memory://wmux-stress-lifecycle");
    let mut controller = harness.connect(64 * 1024).await?;
    command(&mut controller, "new-session -d -s durable").await?;
    let pane = wait_for_spawn(&harness.pty, 1).await?[0];

    for _ in 0..limits.attach_cycles {
        let mut client = harness.connect(64 * 1024).await?;
        command(&mut client, "attach-session -t durable").await?;
        detach(&mut client).await?;
    }

    let mut crashed = harness.connect(64 * 1024).await?;
    command(&mut crashed, "attach-session -t durable").await?;
    drop(crashed);

    let marker = b"wmux-stress-detached-output";
    harness.pty.emit(
        pane,
        PlatformEvent::PtyOutput {
            pane,
            bytes: marker.to_vec(),
        },
    );
    let mut verifier = harness.connect(128 * 1024).await?;
    let attached = command(&mut verifier, "attach-session -t durable").await?;
    if !contains(&attached.output, marker) {
        wait_for_output(&mut verifier, marker).await?;
    }
    let listed = command(&mut verifier, "list-sessions").await?;
    if !listed.message.contains("durable") {
        return Err(StressError::new(
            "abrupt client disconnect destroyed the durable session",
        ));
    }

    harness.shutdown(verifier).await?;

    let restart = MemoryHarness::start("memory://wmux-stress-lifecycle");
    let mut restart_client = restart.connect(64 * 1024).await?;
    command(&mut restart_client, "new-session -d -s restarted").await?;
    let restart_list = command(&mut restart_client, "list-sessions").await?;
    if !restart_list.message.contains("restarted") {
        return Err(StressError::new("server restart did not accept new state"));
    }
    restart.shutdown(restart_client).await?;

    let operations = limits.attach_cycles as u64 * 3 + 10;
    let mut hash = Fnv64::new();
    hash.bytes(marker);
    hash.bytes(listed.message.as_bytes());
    hash.bytes(restart_list.message.as_bytes());
    hash.u64(limits.attach_cycles as u64);
    Ok(StressCase {
        name: "lifecycle",
        operations,
        fingerprint: hash.finish(),
    })
}

async fn event_pressure_scenario(limits: Limits) -> Result<StressCase, StressError> {
    let harness = MemoryHarness::start("memory://wmux-stress-event-pressure");
    let mut client = harness.connect(256 * 1024).await?;
    command(&mut client, "new-session -s pressure").await?;
    let pane = wait_for_spawn(&harness.pty, 1).await?[0];

    for index in 0..limits.resize_events {
        write_message(
            &mut client,
            Message::Resize {
                cols: 80 + (index % 41) as u16,
                rows: 24 + (index % 17) as u16,
            },
        )
        .await?;
    }
    let final_size = TerminalSize::new(119, 39);
    let final_pane_size = TerminalSize::new(final_size.cols, pane_area_rows(final_size.rows));
    write_message(
        &mut client,
        Message::Resize {
            cols: final_size.cols,
            rows: final_size.rows,
        },
    )
    .await?;
    wait_until("final resize request", || {
        harness.pty.latest_resize(pane) == Some(final_pane_size)
    })
    .await?;

    let marker = b"wmux-output-before-exit";
    harness.pty.emit_many(
        pane,
        [
            PlatformEvent::PtyOutput {
                pane,
                bytes: marker.to_vec(),
            },
            PlatformEvent::PtyExited {
                pane,
                exit_code: Some(0),
            },
            PlatformEvent::PtyClosed { pane },
        ],
    );
    wait_for_output_or_shutdown(&mut client, marker).await?;

    let shutdown_client = harness.connect(64 * 1024).await?;
    harness.shutdown(shutdown_client).await?;

    let operations = limits.resize_events as u64 + 5;
    let mut hash = Fnv64::new();
    hash.u64(limits.resize_events as u64);
    hash.u64(u64::from(final_size.cols));
    hash.u64(u64::from(final_size.rows));
    hash.bytes(marker);
    Ok(StressCase {
        name: "event-pressure",
        operations,
        fingerprint: hash.finish(),
    })
}

async fn fan_out_scenario(limits: Limits) -> Result<StressCase, StressError> {
    let harness = MemoryHarness::start("memory://wmux-stress-fan-out");
    let mut controller = harness.connect(512 * 1024).await?;
    command(&mut controller, "new-session -s fanout").await?;
    let pane = wait_for_spawn(&harness.pty, 1).await?[0];

    for index in 1..limits.panes {
        command(&mut controller, &format!("new-window -d -n stress-{index}")).await?;
    }
    wait_for_spawn(&harness.pty, limits.panes).await?;

    let mut stalled = Vec::with_capacity(limits.clients.saturating_sub(1));
    for _ in 1..limits.clients {
        let mut client = harness.connect(4 * 1024).await?;
        command(&mut client, "attach-session -t fanout").await?;
        stalled.push(client);
    }

    let output = vec![b'x'; 64 * 1024];
    for _ in 0..limits.output_rounds {
        harness.pty.emit(
            pane,
            PlatformEvent::PtyOutput {
                pane,
                bytes: output.clone(),
            },
        );
    }

    let deadline = Instant::now() + IO_TIMEOUT;
    let listed = loop {
        match tokio::time::timeout(
            Duration::from_millis(500),
            command(&mut controller, "list-sessions"),
        )
        .await
        {
            Ok(result) => break result?,
            Err(_) if Instant::now() < deadline => continue,
            Err(_) => {
                return Err(StressError::new(
                    "stalled clients blocked the responsive controller",
                ))
            }
        }
    };
    if !listed.message.contains("fanout") {
        return Err(StressError::new(
            "responsive controller received the wrong session state",
        ));
    }
    drop(stalled);
    harness.shutdown(controller).await?;

    let operations = limits.panes as u64 + limits.clients as u64 + limits.output_rounds as u64 + 3;
    let mut hash = Fnv64::new();
    hash.u64(limits.panes as u64);
    hash.u64(limits.clients as u64);
    hash.u64(limits.output_rounds as u64);
    hash.bytes(listed.message.as_bytes());
    Ok(StressCase {
        name: "fan-out",
        operations,
        fingerprint: hash.finish(),
    })
}

async fn storage_scenario(limits: Limits) -> Result<StressCase, StressError> {
    let mut screen = Screen::new(80, 24);
    screen.set_history_limit(limits.history_lines + 24);
    let mut terminal = TerminalEngine::new();
    let mut batch = Vec::with_capacity(64 * 1024);
    for line in 0..limits.history_lines {
        use std::fmt::Write as _;
        let mut text = String::new();
        write!(&mut text, "history-{line:08}\r\n").expect("write to string");
        batch.extend_from_slice(text.as_bytes());
        if batch.len() >= 64 * 1024 {
            terminal.feed(&mut screen, &batch);
            batch.clear();
        }
    }
    terminal.feed(&mut screen, &batch);
    let retained = screen.grid().history_len();
    if retained < limits.history_lines.saturating_sub(24) {
        return Err(StressError::new(format!(
            "history retained {retained} rows, expected at least {}",
            limits.history_lines.saturating_sub(24)
        )));
    }

    let harness = MemoryHarness::start("memory://wmux-stress-storage");
    let mut client = harness.connect(512 * 1024).await?;
    command(&mut client, "new-session -s storage").await?;
    let pane = wait_for_spawn(&harness.pty, 1).await?[0];
    let paste = deterministic_bytes(limits.paste_bytes);
    write_message(&mut client, Message::Paste(paste.clone())).await?;
    wait_until("complete paste delivery", || {
        harness.pty.written_len(pane) == paste.len()
    })
    .await?;
    let written = harness.pty.written_bytes(pane);
    if written != paste {
        return Err(StressError::new(
            "large paste changed bytes or delivery order",
        ));
    }

    let sync_marker = b"wmux-synchronized-output";
    let mut synchronized = b"\x1b[?2026h".to_vec();
    synchronized.extend_from_slice(sync_marker);
    synchronized.extend_from_slice(b"\x1b[?2026l");
    harness.pty.emit(
        pane,
        PlatformEvent::PtyOutput {
            pane,
            bytes: synchronized,
        },
    );
    wait_for_output(&mut client, sync_marker).await?;
    harness.shutdown(client).await?;

    let operations =
        limits.history_lines as u64 + limits.paste_bytes.div_ceil(64 * 1024) as u64 + 4;
    let mut hash = Fnv64::new();
    hash.u64(retained as u64);
    hash.u64(written.len() as u64);
    hash.bytes(&written[..written.len().min(64)]);
    hash.bytes(sync_marker);
    Ok(StressCase {
        name: "storage",
        operations,
        fingerprint: hash.finish(),
    })
}

fn deterministic_bytes(len: usize) -> Vec<u8> {
    let pattern = b"wmux-stress-paste-0123456789abcdef\r\n";
    pattern.iter().copied().cycle().take(len).collect()
}

#[derive(Default)]
struct PtyState {
    requests: Vec<PlatformRequest>,
    events: BTreeMap<PlatformPaneId, VecDeque<PlatformEvent>>,
    notifier: Option<PlatformNotifier>,
}

#[derive(Clone, Default)]
struct PtyHandle(Arc<Mutex<PtyState>>);

impl PtyHandle {
    fn emit(&self, pane: PlatformPaneId, event: PlatformEvent) {
        self.emit_many(pane, [event]);
    }

    fn emit_many(&self, pane: PlatformPaneId, events: impl IntoIterator<Item = PlatformEvent>) {
        let notifier = {
            let mut state = self.0.lock().expect("PTY stress state lock");
            state.events.entry(pane).or_default().extend(events);
            state.notifier.clone()
        };
        if let Some(notifier) = notifier {
            notifier(pane);
        }
    }

    fn spawned_panes(&self) -> Vec<PlatformPaneId> {
        self.0
            .lock()
            .expect("PTY stress state lock")
            .requests
            .iter()
            .filter_map(|request| match request {
                PlatformRequest::SpawnPane(spawn) => Some(spawn.pane),
                _ => None,
            })
            .collect()
    }

    fn latest_resize(&self, pane: PlatformPaneId) -> Option<TerminalSize> {
        self.0
            .lock()
            .expect("PTY stress state lock")
            .requests
            .iter()
            .rev()
            .find_map(|request| match request {
                PlatformRequest::ResizePane {
                    pane: resized,
                    size,
                } if *resized == pane => Some(*size),
                _ => None,
            })
    }

    fn written_len(&self, pane: PlatformPaneId) -> usize {
        self.0
            .lock()
            .expect("PTY stress state lock")
            .requests
            .iter()
            .filter_map(|request| match request {
                PlatformRequest::WritePane {
                    pane: written,
                    bytes,
                } if *written == pane => Some(bytes.len()),
                _ => None,
            })
            .sum()
    }

    fn written_bytes(&self, pane: PlatformPaneId) -> Vec<u8> {
        self.0
            .lock()
            .expect("PTY stress state lock")
            .requests
            .iter()
            .filter_map(|request| match request {
                PlatformRequest::WritePane {
                    pane: written,
                    bytes,
                } if *written == pane => Some(bytes.as_slice()),
                _ => None,
            })
            .flatten()
            .copied()
            .collect()
    }
}

struct ScriptedPtyBackend {
    state: PtyHandle,
}

impl PtyBackend for ScriptedPtyBackend {
    fn submit(&mut self, request: PlatformRequest) -> PlatformResult<()> {
        self.state
            .0
            .lock()
            .expect("PTY stress state lock")
            .requests
            .push(request);
        Ok(())
    }

    fn try_next_event(&mut self, pane: PlatformPaneId) -> PlatformResult<Option<PlatformEvent>> {
        Ok(self
            .state
            .0
            .lock()
            .expect("PTY stress state lock")
            .events
            .get_mut(&pane)
            .and_then(VecDeque::pop_front))
    }
}

#[derive(Default)]
struct JobState {
    requests: Vec<JobRequest>,
    events: BTreeMap<PlatformJobId, VecDeque<JobEvent>>,
    notifier: Option<JobNotifier>,
}

#[derive(Clone, Default)]
struct JobHandle(Arc<Mutex<JobState>>);

struct ScriptedJobBackend {
    state: JobHandle,
}

impl JobBackend for ScriptedJobBackend {
    fn submit(&mut self, request: JobRequest) -> PlatformResult<()> {
        self.state
            .0
            .lock()
            .expect("job stress state lock")
            .requests
            .push(request);
        Ok(())
    }

    fn try_next_event(&mut self, job: PlatformJobId) -> PlatformResult<Option<JobEvent>> {
        Ok(self
            .state
            .0
            .lock()
            .expect("job stress state lock")
            .events
            .get_mut(&job)
            .and_then(VecDeque::pop_front))
    }
}

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
                    "accept stress client",
                    "memory connector closed",
                )
            })
        })
    }
}

struct MemoryServerPlatform {
    listener: Option<MemoryListener>,
    pty: PtyHandle,
    jobs: JobHandle,
}

impl ServerPlatform for MemoryServerPlatform {
    fn bind(&mut self) -> PlatformResult<Box<dyn ServerListener>> {
        self.listener
            .take()
            .map(|listener| Box::new(listener) as Box<dyn ServerListener>)
            .ok_or_else(|| {
                PlatformError::new(
                    PlatformErrorKind::AlreadyRunning,
                    "bind stress listener",
                    "memory listener already bound",
                )
            })
    }

    fn create_pty_backend(
        &mut self,
        notifier: PlatformNotifier,
    ) -> PlatformResult<Box<dyn PtyBackend>> {
        self.pty.0.lock().expect("PTY stress state lock").notifier = Some(notifier);
        Ok(Box::new(ScriptedPtyBackend {
            state: self.pty.clone(),
        }))
    }

    fn create_job_backend(&mut self, notifier: JobNotifier) -> PlatformResult<Box<dyn JobBackend>> {
        self.jobs.0.lock().expect("job stress state lock").notifier = Some(notifier);
        Ok(Box::new(ScriptedJobBackend {
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
    fn connect(&self, capacity: usize) -> Result<DuplexStream, StressError> {
        let (client, server) = tokio::io::duplex(capacity);
        self.accepted
            .send(AcceptedConnection {
                stream: Box::new(server),
                peer: self.owner.clone(),
            })
            .map_err(|_| StressError::new("memory server stopped accepting clients"))?;
        Ok(client)
    }
}

struct MemoryHarness {
    connector: MemoryConnector,
    pty: PtyHandle,
    server: thread::JoinHandle<std::io::Result<()>>,
}

impl MemoryHarness {
    fn start(endpoint: &str) -> Self {
        let (accepted_tx, accepted_rx) = mpsc::unbounded_channel();
        let owner = PeerIdentity::from_token(b"wmux-stress-owner");
        let pty = PtyHandle::default();
        let jobs = JobHandle::default();
        let platform = MemoryServerPlatform {
            listener: Some(MemoryListener {
                endpoint: Endpoint::new(endpoint),
                owner: owner.clone(),
                accepted: accepted_rx,
            }),
            pty: pty.clone(),
            jobs,
        };
        let server = thread::Builder::new()
            .name("wmux-stress-server".to_string())
            .spawn(move || {
                wmux_server::run_with_platform_and_config(Box::new(platform), WmuxConfig::default())
            })
            .expect("stress server thread spawns");
        Self {
            connector: MemoryConnector {
                accepted: accepted_tx,
                owner,
            },
            pty,
            server,
        }
    }

    async fn connect(&self, capacity: usize) -> Result<DuplexStream, StressError> {
        let mut stream = self.connector.connect(capacity)?;
        write_message(
            &mut stream,
            Message::Hello {
                version: VERSION,
                pid: std::process::id(),
                capabilities: TerminalCapabilities::default(),
                current_dir: std::env::current_dir()
                    .expect("stress process has a working directory")
                    .to_string_lossy()
                    .into_owned(),
            },
        )
        .await?;
        match read_message_timeout(&mut stream).await? {
            Some(Message::HelloOk {
                version: VERSION, ..
            }) => Ok(stream),
            other => Err(StressError::new(format!(
                "stress handshake returned {other:?}"
            ))),
        }
    }

    async fn shutdown(self, mut client: DuplexStream) -> Result<(), StressError> {
        write_message(&mut client, Message::Command("kill-server".to_string())).await?;
        let _ = tokio::time::timeout(IO_TIMEOUT, async {
            loop {
                match read_message(&mut client).await {
                    Ok(Some(Message::CommandOk(_) | Message::Shutdown)) | Ok(None) => break,
                    Ok(Some(_)) => {}
                    Err(_) => break,
                }
            }
        })
        .await;
        let deadline = Instant::now() + IO_TIMEOUT;
        while !self.server.is_finished() && Instant::now() < deadline {
            tokio::time::sleep(Duration::from_millis(2)).await;
        }
        if !self.server.is_finished() {
            return Err(StressError::new(
                "stress server did not stop after kill-server",
            ));
        }
        self.server
            .join()
            .map_err(|_| StressError::new("stress server thread panicked"))??;
        Ok(())
    }
}

struct CommandReply {
    message: String,
    output: Vec<u8>,
}

async fn command(stream: &mut DuplexStream, text: &str) -> Result<CommandReply, StressError> {
    write_message(stream, Message::Command(text.to_string())).await?;
    let deadline = Instant::now() + IO_TIMEOUT;
    let mut output = Vec::new();
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(StressError::new(format!("command {text:?} timed out")));
        }
        match tokio::time::timeout(remaining, read_message(stream)).await {
            Ok(Ok(Some(Message::CommandOk(message)))) => {
                return Ok(CommandReply { message, output })
            }
            Ok(Ok(Some(Message::CommandErr(error)))) => {
                return Err(StressError::new(format!(
                    "command {text:?} failed: {error}"
                )))
            }
            Ok(Ok(Some(Message::Output { sequence, bytes }))) => {
                write_message(stream, Message::OutputAck { sequence }).await?;
                output.extend(bytes);
            }
            Ok(Ok(Some(Message::Clipboard(_) | Message::ControlRecord(_)))) => {}
            Ok(Ok(Some(other))) => {
                return Err(StressError::new(format!(
                    "command {text:?} returned {other:?}"
                )))
            }
            Ok(Ok(None)) => {
                return Err(StressError::new(format!(
                    "server closed during command {text:?}"
                )))
            }
            Ok(Err(error)) => return Err(error),
            Err(_) => return Err(StressError::new(format!("command {text:?} timed out"))),
        }
    }
}

async fn detach(stream: &mut DuplexStream) -> Result<(), StressError> {
    write_message(stream, Message::Detach).await?;
    let deadline = Instant::now() + IO_TIMEOUT;
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(StressError::new("detach timed out"));
        }
        match tokio::time::timeout(remaining, read_message(stream)).await {
            Ok(Ok(Some(Message::CommandOk(_)))) => return Ok(()),
            Ok(Ok(Some(Message::Output { sequence, .. }))) => {
                write_message(stream, Message::OutputAck { sequence }).await?;
            }
            Ok(Ok(Some(Message::Clipboard(_)))) => {}
            Ok(Ok(Some(other))) => {
                return Err(StressError::new(format!(
                    "detach returned unexpected {other:?}"
                )))
            }
            Ok(Ok(None)) => return Err(StressError::new("server closed during detach")),
            Ok(Err(error)) => return Err(error),
            Err(_) => return Err(StressError::new("detach timed out")),
        }
    }
}

async fn wait_for_spawn(
    pty: &PtyHandle,
    expected: usize,
) -> Result<Vec<PlatformPaneId>, StressError> {
    wait_until("pane spawn requests", || {
        pty.spawned_panes().len() >= expected
    })
    .await?;
    Ok(pty.spawned_panes())
}

async fn wait_for_output(stream: &mut DuplexStream, marker: &[u8]) -> Result<(), StressError> {
    let deadline = Instant::now() + IO_TIMEOUT;
    let mut output = Vec::new();
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(StressError::new(format!(
                "output marker {:?} timed out",
                String::from_utf8_lossy(marker)
            )));
        }
        match tokio::time::timeout(remaining, read_message(stream)).await {
            Ok(Ok(Some(Message::Output { sequence, bytes }))) => {
                write_message(stream, Message::OutputAck { sequence }).await?;
                output.extend(bytes);
                if contains(&output, marker) {
                    return Ok(());
                }
            }
            Ok(Ok(Some(Message::CommandOk(_) | Message::Clipboard(_)))) => {}
            Ok(Ok(Some(other))) => {
                return Err(StressError::new(format!(
                    "waiting for output returned {other:?}"
                )))
            }
            Ok(Ok(None)) => return Err(StressError::new("server closed before output marker")),
            Ok(Err(error)) => return Err(error),
            Err(_) => return Err(StressError::new("output marker timed out")),
        }
    }
}

async fn wait_for_output_or_shutdown(
    stream: &mut DuplexStream,
    marker: &[u8],
) -> Result<(), StressError> {
    let deadline = Instant::now() + IO_TIMEOUT;
    let mut output = Vec::new();
    loop {
        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Err(StressError::new("pane exit output marker timed out"));
        }
        match tokio::time::timeout(remaining, read_message(stream)).await {
            Ok(Ok(Some(Message::Output { sequence, bytes }))) => {
                write_message(stream, Message::OutputAck { sequence }).await?;
                output.extend(bytes);
                if contains(&output, marker) {
                    return Ok(());
                }
            }
            Ok(Ok(Some(Message::Shutdown))) | Ok(Ok(None)) => {
                return Err(StressError::new(
                    "pane closed before output preceding exit reached the client",
                ))
            }
            Ok(Ok(Some(_))) => {}
            Ok(Err(error)) => return Err(error),
            Err(_) => return Err(StressError::new("pane exit output marker timed out")),
        }
    }
}

async fn wait_until(
    description: &str,
    mut condition: impl FnMut() -> bool,
) -> Result<(), StressError> {
    let deadline = Instant::now() + IO_TIMEOUT;
    while !condition() {
        if Instant::now() >= deadline {
            return Err(StressError::new(format!(
                "timed out waiting for {description}"
            )));
        }
        tokio::time::sleep(Duration::from_millis(1)).await;
    }
    Ok(())
}

fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    haystack
        .windows(needle.len())
        .any(|window| window == needle)
}

async fn read_message_timeout(stream: &mut DuplexStream) -> Result<Option<Message>, StressError> {
    tokio::time::timeout(IO_TIMEOUT, read_message(stream))
        .await
        .map_err(|_| StressError::new("protocol read timed out"))?
}

async fn read_message(stream: &mut DuplexStream) -> Result<Option<Message>, StressError> {
    let mut header = [0_u8; FRAME_HEADER_LEN];
    match stream.read_exact(&mut header).await {
        Ok(_) => {}
        Err(error) if error.kind() == std::io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(error.into()),
    }
    let (tag, payload_len) = decode_frame_header(&header)?;
    let mut payload = vec![0_u8; payload_len];
    stream.read_exact(&mut payload).await?;
    decode_frame_payload_owned(tag, payload)
        .map(Some)
        .map_err(StressError::from)
}

async fn write_message(stream: &mut DuplexStream, message: Message) -> Result<(), StressError> {
    let frame = EncodedFrame::from_message(message);
    stream.write_all(frame.header()).await?;
    stream.write_all(frame.payload()).await?;
    stream.flush().await?;
    Ok(())
}

struct Fnv64(u64);

impl Fnv64 {
    const fn new() -> Self {
        Self(0xcbf29ce484222325)
    }

    fn bytes(&mut self, bytes: &[u8]) {
        for byte in bytes {
            self.0 ^= u64::from(*byte);
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }

    fn u64(&mut self, value: u64) {
        self.bytes(&value.to_le_bytes());
    }

    const fn finish(self) -> u64 {
        self.0
    }
}

#[cfg(test)]
mod tests {
    use super::{run_suite, StressProfile};

    #[test]
    fn stress_suite_covers_every_beta_core_scenario() {
        let report = run_suite(StressProfile::Ci).expect("CI stress suite passes");

        assert_eq!(
            report
                .cases
                .iter()
                .map(|case| case.name)
                .collect::<Vec<_>>(),
            ["lifecycle", "event-pressure", "fan-out", "storage"],
        );
        assert!(report.cases.iter().all(|case| case.operations > 0));
    }

    #[test]
    fn stress_suite_fingerprint_is_deterministic() {
        assert_eq!(
            run_suite(StressProfile::Ci).expect("first CI stress suite"),
            run_suite(StressProfile::Ci).expect("second CI stress suite")
        );
    }
}
