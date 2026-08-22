use crate::{
    conpty::{spawn_shell, ConptyPane, PlatformEventReceiver},
    console, daemon,
    pipe::{self, Endpoint as WindowsEndpoint, ServerLock, ServerPipeFactory, UserSid},
};
use std::{collections::BTreeMap, io, sync::Arc};
use tokio::{runtime::Handle as TokioHandle, sync::mpsc::error::TryRecvError};
use wmux_platform::{
    AcceptedConnection, BoxedIpcStream, ClientTransport, DaemonSpec, Endpoint, JobBackend,
    JobNotifier, PeerIdentity, PlatformError, PlatformErrorKind, PlatformEvent, PlatformFuture,
    PlatformNotifier, PlatformPaneId, PlatformRequest, PlatformResult, PtyBackend, ServerListener,
    ServerPlatform, SpawnPane, TerminalBackend, TerminalInput, TerminalModeGuard, TerminalSize,
    TerminationMode,
};

const ERROR_PIPE_BUSY_RAW: i32 = 231;
const ERROR_SHARING_VIOLATION_RAW: i32 = 32;

pub(crate) fn classify_windows_io_error(
    operation: &'static str,
    error: io::Error,
) -> PlatformError {
    if matches!(
        error.raw_os_error(),
        Some(ERROR_PIPE_BUSY_RAW | ERROR_SHARING_VIOLATION_RAW)
    ) {
        PlatformError::new(PlatformErrorKind::Busy, operation, error.to_string())
    } else {
        PlatformError::from_io(operation, error)
    }
}

pub struct WindowsServerPlatform {
    endpoint: WindowsEndpoint,
}

impl WindowsServerPlatform {
    pub fn current_user() -> PlatformResult<Self> {
        Ok(Self {
            endpoint: WindowsEndpoint::current_user()
                .map_err(|error| classify_windows_io_error("discover server endpoint", error))?,
        })
    }

    pub fn for_instance(instance: &str) -> PlatformResult<Self> {
        Ok(Self {
            endpoint: WindowsEndpoint::for_instance(instance)
                .map_err(|error| classify_windows_io_error("configure server endpoint", error))?,
        })
    }
}

impl ServerPlatform for WindowsServerPlatform {
    fn bind(&mut self) -> PlatformResult<Box<dyn ServerListener>> {
        let server_lock = ServerLock::acquire(&self.endpoint)
            .map_err(|error| classify_windows_io_error("acquire server lock", error))?;
        let owner_sid = self.endpoint.owner_sid().clone();
        let owner_identity = owner_sid.peer_identity();
        let endpoint = Endpoint::new(self.endpoint.pipe_name());
        let mut factory = ServerPipeFactory::new(self.endpoint.clone())
            .map_err(|error| classify_windows_io_error("configure IPC listener", error))?;
        let listener = factory
            .create()
            .map_err(|error| classify_windows_io_error("create IPC listener", error))?;
        Ok(Box::new(WindowsServerListener {
            _server_lock: server_lock,
            factory,
            listener,
            endpoint,
            owner_sid,
            owner_identity,
        }))
    }

    fn create_pty_backend(
        &mut self,
        notifier: PlatformNotifier,
    ) -> PlatformResult<Box<dyn PtyBackend>> {
        Ok(Box::new(WindowsPtyBackend::new(
            TokioHandle::current(),
            notifier,
        )))
    }

    fn create_job_backend(&mut self, notifier: JobNotifier) -> PlatformResult<Box<dyn JobBackend>> {
        Ok(Box::new(crate::job::WindowsJobBackend::new(notifier)))
    }
}

struct WindowsServerListener {
    _server_lock: ServerLock,
    factory: ServerPipeFactory,
    listener: tokio::net::windows::named_pipe::NamedPipeServer,
    endpoint: Endpoint,
    owner_sid: UserSid,
    owner_identity: PeerIdentity,
}

impl ServerListener for WindowsServerListener {
    fn endpoint(&self) -> &Endpoint {
        &self.endpoint
    }

    fn owner_identity(&self) -> &PeerIdentity {
        &self.owner_identity
    }

    fn accept(&mut self) -> PlatformFuture<'_, AcceptedConnection> {
        Box::pin(async move {
            self.listener
                .connect()
                .await
                .map_err(|error| classify_windows_io_error("accept IPC client", error))?;
            let verified = pipe::verify_client(&self.listener, &self.owner_sid)
                .map_err(|error| classify_windows_io_error("verify IPC peer", error));
            let next = self
                .factory
                .create()
                .map_err(|error| classify_windows_io_error("create next IPC listener", error))?;
            let connected = std::mem::replace(&mut self.listener, next);
            verified?;
            Ok(AcceptedConnection {
                stream: Box::new(connected),
                peer: self.owner_identity.clone(),
            })
        })
    }
}

pub struct WindowsClientTransport {
    endpoint: WindowsEndpoint,
    semantic_endpoint: Endpoint,
}

impl WindowsClientTransport {
    pub fn current_user() -> PlatformResult<Self> {
        let endpoint = WindowsEndpoint::current_user()
            .map_err(|error| classify_windows_io_error("discover server endpoint", error))?;
        let semantic_endpoint = Endpoint::new(endpoint.pipe_name());
        Ok(Self {
            endpoint,
            semantic_endpoint,
        })
    }

    pub fn for_instance(instance: &str) -> PlatformResult<Self> {
        let endpoint = WindowsEndpoint::for_instance(instance)
            .map_err(|error| classify_windows_io_error("configure server endpoint", error))?;
        let semantic_endpoint = Endpoint::new(endpoint.pipe_name());
        Ok(Self {
            endpoint,
            semantic_endpoint,
        })
    }
}

impl ClientTransport for WindowsClientTransport {
    fn endpoint(&self) -> &Endpoint {
        &self.semantic_endpoint
    }

    fn connect(&self) -> PlatformFuture<'_, BoxedIpcStream> {
        Box::pin(async move {
            pipe::connect_async(&self.endpoint)
                .map(|pipe| Box::new(pipe) as BoxedIpcStream)
                .map_err(|error| classify_windows_io_error("connect IPC client", error))
        })
    }

    fn spawn_server(&self, spec: &DaemonSpec) -> PlatformResult<()> {
        daemon::spawn_user_daemon(&daemon::DaemonSpec {
            executable: spec.executable.clone(),
            arguments: spec.arguments.clone(),
            current_dir: spec.current_dir.clone(),
        })
        .map(|_| ())
        .map_err(|error| classify_windows_io_error("start server daemon", error))
    }
}

#[derive(Default)]
pub struct WindowsTerminalBackend;

impl TerminalBackend for WindowsTerminalBackend {
    fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
        console::ConsoleGuard::enter()
            .map(|guard| Box::new(guard) as Box<dyn TerminalModeGuard>)
            .map_err(|error| classify_windows_io_error("enter terminal mode", error))
    }

    fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
        console::read_input()
            .map_err(|error| classify_windows_io_error("read terminal input", error))
    }

    fn write_output(&self, bytes: &[u8]) -> PlatformResult<()> {
        console::write_output(bytes)
            .map_err(|error| classify_windows_io_error("write terminal output", error))
    }

    fn write_render_transaction(
        &self,
        bytes: &[u8],
        synchronized_output: bool,
    ) -> PlatformResult<()> {
        console::write_render_transaction(bytes, synchronized_output)
            .map_err(|error| classify_windows_io_error("write render transaction", error))
    }

    fn write_clipboard_text(&self, text: &str) -> PlatformResult<()> {
        console::write_clipboard_text(text)
            .map_err(|error| classify_windows_io_error("write clipboard", error))
    }

    fn size(&self) -> PlatformResult<TerminalSize> {
        console::size().map_err(|error| classify_windows_io_error("query terminal size", error))
    }
}

struct WindowsPane {
    controller: ConptyPane,
    events: PlatformEventReceiver,
    exited: bool,
    pending_close: bool,
}

pub struct WindowsPtyBackend {
    runtime: TokioHandle,
    notifier: PlatformNotifier,
    panes: BTreeMap<PlatformPaneId, WindowsPane>,
}

impl WindowsPtyBackend {
    pub fn new(runtime: TokioHandle, notifier: PlatformNotifier) -> Self {
        Self {
            runtime,
            notifier,
            panes: BTreeMap::new(),
        }
    }

    fn pane_mut(&mut self, pane: PlatformPaneId) -> PlatformResult<&mut WindowsPane> {
        self.panes.get_mut(&pane).ok_or_else(|| {
            PlatformError::new(
                PlatformErrorKind::NotFound,
                "access pane",
                format!("platform pane {} does not exist", pane.raw()),
            )
        })
    }

    fn spawn(&mut self, request: SpawnPane) -> PlatformResult<()> {
        if self.panes.contains_key(&request.pane) {
            return Err(PlatformError::new(
                PlatformErrorKind::AlreadyRunning,
                "spawn pane",
                format!("platform pane {} already exists", request.pane.raw()),
            ));
        }
        if request.command.is_some() || request.cwd.is_some() {
            return Err(PlatformError::new(
                PlatformErrorKind::Unsupported,
                "spawn pane",
                "custom Windows pane command and cwd are not implemented",
            ));
        }
        let environment = request
            .environment
            .iter()
            .map(|(key, value)| {
                (
                    key.to_string_lossy().into_owned(),
                    value.to_string_lossy().into_owned(),
                )
            })
            .collect::<Vec<_>>();
        let (controller, events) = spawn_shell(
            request.pane,
            request.size,
            &environment,
            &self.runtime,
            Arc::clone(&self.notifier),
        )
        .map_err(|error| classify_windows_io_error("spawn ConPTY pane", error))?;
        self.panes.insert(
            request.pane,
            WindowsPane {
                controller,
                events,
                exited: false,
                pending_close: false,
            },
        );
        Ok(())
    }
}

impl PtyBackend for WindowsPtyBackend {
    fn submit(&mut self, request: PlatformRequest) -> PlatformResult<()> {
        match request {
            PlatformRequest::SpawnPane(request) => self.spawn(request),
            PlatformRequest::WritePane { pane, bytes } => self
                .pane_mut(pane)?
                .controller
                .write_input(bytes)
                .map_err(|error| classify_windows_io_error("write ConPTY input", error)),
            PlatformRequest::ResizePane { pane, size } => self
                .pane_mut(pane)?
                .controller
                .resize(size)
                .map_err(|error| classify_windows_io_error("resize ConPTY", error)),
            PlatformRequest::TerminatePane { pane, mode } => {
                self.pane_mut(pane)?.controller.terminate(match mode {
                    TerminationMode::Graceful => 0,
                    TerminationMode::Force => 1,
                });
                Ok(())
            }
        }
    }

    fn try_next_event(&mut self, pane: PlatformPaneId) -> PlatformResult<Option<PlatformEvent>> {
        if self
            .panes
            .get(&pane)
            .is_some_and(|state| state.pending_close)
        {
            self.panes.remove(&pane);
            return Ok(Some(PlatformEvent::PtyClosed { pane }));
        }

        loop {
            let state = self.pane_mut(pane)?;
            match state.events.try_recv() {
                Ok(PlatformEvent::PtyExited { exit_code, .. }) if !state.exited => {
                    state.exited = true;
                    if exit_code.is_some() {
                        state.controller.finish_after_process_exit();
                    } else {
                        state.controller.terminate(1);
                    }
                    return Ok(Some(PlatformEvent::PtyExited { pane, exit_code }));
                }
                Ok(PlatformEvent::PtyExited { .. }) => continue,
                Ok(PlatformEvent::PtyOutput { bytes, .. }) => {
                    return Ok(Some(PlatformEvent::PtyOutput { pane, bytes }));
                }
                Ok(PlatformEvent::BackendError { error, .. }) => {
                    return Ok(Some(PlatformEvent::BackendError { pane, error }));
                }
                Ok(PlatformEvent::PtyClosed { .. }) => {
                    state.pending_close = true;
                    continue;
                }
                Err(TryRecvError::Empty) => return Ok(None),
                Err(TryRecvError::Disconnected) if !state.exited => {
                    state.exited = true;
                    state.pending_close = true;
                    return Ok(Some(PlatformEvent::PtyExited {
                        pane,
                        exit_code: None,
                    }));
                }
                Err(TryRecvError::Disconnected) => {
                    self.panes.remove(&pane);
                    return Ok(Some(PlatformEvent::PtyClosed { pane }));
                }
            }
        }
    }
}
