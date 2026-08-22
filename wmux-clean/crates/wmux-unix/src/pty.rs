use crate::process::{signal_group, spawn_pane, SpawnedPane};
use nix::fcntl::{fcntl, FcntlArg, OFlag};
use std::{
    collections::BTreeMap,
    io,
    os::fd::{AsRawFd, OwnedFd},
    sync::Arc,
};
use tokio::{
    io::unix::AsyncFd,
    runtime::Handle,
    sync::mpsc::{self, error::TryRecvError, error::TrySendError},
};
use wmux_platform::{
    PlatformError, PlatformErrorKind, PlatformEvent, PlatformNotifier, PlatformPaneId,
    PlatformRequest, PlatformResult, PtyBackend, SpawnPane, TerminationMode,
};

const PTY_READ_CHUNK_BYTES: usize = 16 * 1024;
const PTY_EVENT_QUEUE_CHUNKS: usize = 64;
const PTY_INPUT_QUEUE_CHUNKS: usize = 64;

enum InternalEvent {
    Output(Vec<u8>),
    Exit(Option<u32>),
    Eof,
    Error(PlatformError),
}

struct UnixPane {
    io: Arc<AsyncFd<OwnedFd>>,
    input: mpsc::Sender<Vec<u8>>,
    events: mpsc::Receiver<InternalEvent>,
    process_group: libc::pid_t,
    exit_emitted: bool,
    eof: bool,
}

pub(crate) struct UnixPtyBackend {
    runtime: Handle,
    notifier: PlatformNotifier,
    panes: BTreeMap<PlatformPaneId, UnixPane>,
}

impl UnixPtyBackend {
    pub(crate) fn new(runtime: Handle, notifier: PlatformNotifier) -> Self {
        Self {
            runtime,
            notifier,
            panes: BTreeMap::new(),
        }
    }

    fn spawn(&mut self, request: SpawnPane) -> PlatformResult<()> {
        if self.panes.contains_key(&request.pane) {
            return Err(PlatformError::new(
                PlatformErrorKind::AlreadyRunning,
                "spawn Unix PTY pane",
                format!("platform pane {} already exists", request.pane.raw()),
            ));
        }
        let pane = request.pane;
        let spawned = spawn_pane(&request)
            .map_err(|error| PlatformError::from_io("spawn Unix PTY pane", error))?;
        let SpawnedPane {
            master,
            mut child,
            process_group,
        } = spawned;
        set_nonblocking(&master)
            .map_err(|error| PlatformError::from_io("configure Unix PTY master", error))?;
        let io = Arc::new(
            AsyncFd::new(master)
                .map_err(|error| PlatformError::from_io("register Unix PTY master", error))?,
        );
        let (event_tx, event_rx) = mpsc::channel(PTY_EVENT_QUEUE_CHUNKS);
        let (input_tx, input_rx) = mpsc::channel(PTY_INPUT_QUEUE_CHUNKS);
        self.runtime.spawn(read_output(
            pane,
            Arc::clone(&io),
            event_tx.clone(),
            Arc::clone(&self.notifier),
        ));
        self.runtime.spawn(write_input(
            pane,
            Arc::clone(&io),
            input_rx,
            event_tx.clone(),
            Arc::clone(&self.notifier),
        ));
        let notifier = Arc::clone(&self.notifier);
        self.runtime.spawn_blocking(move || {
            let event = match child.wait() {
                Ok(status) => InternalEvent::Exit(status.code().map(|code| code as u32)),
                Err(error) => {
                    InternalEvent::Error(PlatformError::from_io("reap Unix PTY child", error))
                }
            };
            if event_tx.blocking_send(event).is_ok() {
                notifier(pane);
            }
        });
        self.panes.insert(
            pane,
            UnixPane {
                io,
                input: input_tx,
                events: event_rx,
                process_group,
                exit_emitted: false,
                eof: false,
            },
        );
        Ok(())
    }

    fn pane_mut(&mut self, pane: PlatformPaneId) -> PlatformResult<&mut UnixPane> {
        self.panes.get_mut(&pane).ok_or_else(|| {
            PlatformError::new(
                PlatformErrorKind::NotFound,
                "access Unix PTY pane",
                format!("platform pane {} does not exist", pane.raw()),
            )
        })
    }
}

impl PtyBackend for UnixPtyBackend {
    fn submit(&mut self, request: PlatformRequest) -> PlatformResult<()> {
        match request {
            PlatformRequest::SpawnPane(request) => self.spawn(request),
            PlatformRequest::WritePane { pane, bytes } => {
                match self.pane_mut(pane)?.input.try_send(bytes) {
                    Ok(()) => Ok(()),
                    Err(TrySendError::Full(_)) => Err(PlatformError::new(
                        PlatformErrorKind::Busy,
                        "queue Unix PTY input",
                        "PTY input queue is full",
                    )),
                    Err(TrySendError::Closed(_)) => Err(PlatformError::new(
                        PlatformErrorKind::Disconnected,
                        "queue Unix PTY input",
                        "PTY input writer is closed",
                    )),
                }
            }
            PlatformRequest::ResizePane { pane, size } => {
                let io = Arc::clone(&self.pane_mut(pane)?.io);
                resize(io.as_raw_fd(), size.cols, size.rows)
                    .map_err(|error| PlatformError::from_io("resize Unix PTY", error))
            }
            PlatformRequest::TerminatePane { pane, mode } => {
                let process_group = self.pane_mut(pane)?.process_group;
                let signal = match mode {
                    TerminationMode::Graceful => libc::SIGHUP,
                    TerminationMode::Force => libc::SIGKILL,
                };
                signal_group(process_group, signal)
                    .map_err(|error| PlatformError::from_io("terminate Unix PTY group", error))
            }
        }
    }

    fn try_next_event(&mut self, pane: PlatformPaneId) -> PlatformResult<Option<PlatformEvent>> {
        let mut should_close = false;
        loop {
            let state = self.pane_mut(pane)?;
            match state.events.try_recv() {
                Ok(InternalEvent::Output(bytes)) => {
                    return Ok(Some(PlatformEvent::PtyOutput { pane, bytes }));
                }
                Ok(InternalEvent::Exit(exit_code)) if !state.exit_emitted => {
                    state.exit_emitted = true;
                    return Ok(Some(PlatformEvent::PtyExited { pane, exit_code }));
                }
                Ok(InternalEvent::Exit(_)) => continue,
                Ok(InternalEvent::Eof) => {
                    state.eof = true;
                }
                Ok(InternalEvent::Error(error)) => {
                    return Ok(Some(PlatformEvent::BackendError { pane, error }));
                }
                Err(TryRecvError::Empty) if state.eof && state.exit_emitted => {
                    should_close = true;
                }
                Err(TryRecvError::Empty) => return Ok(None),
                Err(TryRecvError::Disconnected) if !state.exit_emitted => {
                    state.exit_emitted = true;
                    state.eof = true;
                    return Ok(Some(PlatformEvent::PtyExited {
                        pane,
                        exit_code: None,
                    }));
                }
                Err(TryRecvError::Disconnected) => {
                    should_close = true;
                }
            }
            if should_close {
                break;
            }
        }
        self.panes.remove(&pane);
        Ok(Some(PlatformEvent::PtyClosed { pane }))
    }
}

fn set_nonblocking(fd: &OwnedFd) -> io::Result<()> {
    let flags = fcntl(fd, FcntlArg::F_GETFL).map_err(nix_error)?;
    let flags = OFlag::from_bits_truncate(flags) | OFlag::O_NONBLOCK;
    fcntl(fd, FcntlArg::F_SETFL(flags)).map_err(nix_error)?;
    Ok(())
}

fn resize(fd: libc::c_int, cols: u16, rows: u16) -> io::Result<()> {
    let size = libc::winsize {
        ws_row: rows.max(1),
        ws_col: cols.max(1),
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    let result = unsafe { libc::ioctl(fd, libc::TIOCSWINSZ, &size) };
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}

async fn read_output(
    pane: PlatformPaneId,
    io: Arc<AsyncFd<OwnedFd>>,
    events: mpsc::Sender<InternalEvent>,
    notifier: PlatformNotifier,
) {
    let mut buffer = [0_u8; PTY_READ_CHUNK_BYTES];
    loop {
        let mut ready = match io.readable().await {
            Ok(ready) => ready,
            Err(error) => {
                send_event(
                    pane,
                    &events,
                    &notifier,
                    InternalEvent::Error(PlatformError::from_io("wait for Unix PTY output", error)),
                )
                .await;
                break;
            }
        };
        let read = ready.try_io(|inner| {
            let read = unsafe {
                libc::read(
                    inner.get_ref().as_raw_fd(),
                    buffer.as_mut_ptr().cast(),
                    buffer.len(),
                )
            };
            if read >= 0 {
                Ok(read as usize)
            } else {
                Err(io::Error::last_os_error())
            }
        });
        match read {
            Ok(Ok(0)) => break,
            Ok(Ok(read)) => {
                if !send_event(
                    pane,
                    &events,
                    &notifier,
                    InternalEvent::Output(buffer[..read].to_vec()),
                )
                .await
                {
                    return;
                }
            }
            Ok(Err(error)) if error.raw_os_error() == Some(libc::EIO) => break,
            Ok(Err(error)) => {
                send_event(
                    pane,
                    &events,
                    &notifier,
                    InternalEvent::Error(PlatformError::from_io("read Unix PTY output", error)),
                )
                .await;
                break;
            }
            Err(_) => continue,
        }
    }
    send_event(pane, &events, &notifier, InternalEvent::Eof).await;
}

async fn write_input(
    pane: PlatformPaneId,
    io: Arc<AsyncFd<OwnedFd>>,
    mut input: mpsc::Receiver<Vec<u8>>,
    events: mpsc::Sender<InternalEvent>,
    notifier: PlatformNotifier,
) {
    while let Some(bytes) = input.recv().await {
        let mut offset = 0;
        while offset < bytes.len() {
            let mut ready = match io.writable().await {
                Ok(ready) => ready,
                Err(error) => {
                    send_event(
                        pane,
                        &events,
                        &notifier,
                        InternalEvent::Error(PlatformError::from_io(
                            "wait for Unix PTY input",
                            error,
                        )),
                    )
                    .await;
                    return;
                }
            };
            let written = ready.try_io(|inner| {
                let written = unsafe {
                    libc::write(
                        inner.get_ref().as_raw_fd(),
                        bytes[offset..].as_ptr().cast(),
                        bytes.len() - offset,
                    )
                };
                if written >= 0 {
                    Ok(written as usize)
                } else {
                    Err(io::Error::last_os_error())
                }
            });
            match written {
                Ok(Ok(0)) => continue,
                Ok(Ok(written)) => offset += written,
                Ok(Err(error)) if error.kind() == io::ErrorKind::Interrupted => continue,
                Ok(Err(error)) => {
                    send_event(
                        pane,
                        &events,
                        &notifier,
                        InternalEvent::Error(PlatformError::from_io("write Unix PTY input", error)),
                    )
                    .await;
                    return;
                }
                Err(_) => continue,
            }
        }
    }
}

async fn send_event(
    pane: PlatformPaneId,
    events: &mpsc::Sender<InternalEvent>,
    notifier: &PlatformNotifier,
    event: InternalEvent,
) -> bool {
    if events.send(event).await.is_err() {
        return false;
    }
    notifier(pane);
    true
}

fn nix_error(error: nix::errno::Errno) -> io::Error {
    io::Error::from_raw_os_error(error as i32)
}

#[cfg(test)]
mod tests {
    use super::{set_nonblocking, InternalEvent, UnixPane, UnixPtyBackend};
    use crate::process::signal_group;
    use nix::pty::openpty;
    use std::{ffi::OsString, os::fd::AsRawFd, sync::Arc, time::Duration};
    use tokio::{
        io::unix::AsyncFd,
        runtime::Handle,
        sync::{mpsc, Notify},
    };
    use wmux_platform::{
        CommandSpec, PlatformErrorKind, PlatformEvent, PlatformNotifier, PlatformPaneId,
        PlatformRequest, PtyBackend, SpawnPane, TerminalSize, TerminationMode,
    };

    struct ProcessGroupGuard(libc::pid_t);

    impl Drop for ProcessGroupGuard {
        fn drop(&mut self) {
            let _ = signal_group(self.0, libc::SIGKILL);
        }
    }

    fn process_guard(backend: &UnixPtyBackend, pane: PlatformPaneId) -> ProcessGroupGuard {
        ProcessGroupGuard(backend.panes.get(&pane).expect("pane exists").process_group)
    }

    fn kernel_size(backend: &UnixPtyBackend, pane: PlatformPaneId) -> TerminalSize {
        let fd = backend
            .panes
            .get(&pane)
            .expect("pane exists")
            .io
            .as_raw_fd();
        let mut size = libc::winsize {
            ws_row: 0,
            ws_col: 0,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        let result = unsafe { libc::ioctl(fd, libc::TIOCGWINSZ, &mut size) };
        assert_eq!(result, 0, "TIOCGWINSZ succeeds");
        TerminalSize::new(size.ws_col, size.ws_row)
    }

    fn backend_with_event_queue(
        pane: PlatformPaneId,
    ) -> (UnixPtyBackend, mpsc::Sender<InternalEvent>) {
        let notifier: PlatformNotifier = Arc::new(|_| {});
        let mut backend = UnixPtyBackend::new(Handle::current(), notifier);
        let pty = openpty(None, None).expect("test PTY opens");
        set_nonblocking(&pty.master).expect("test PTY becomes nonblocking");
        let io = Arc::new(AsyncFd::new(pty.master).expect("test PTY registers"));
        let (input, _input_rx) = mpsc::channel(8);
        let (events, event_rx) = mpsc::channel(8);
        backend.panes.insert(
            pane,
            UnixPane {
                io,
                input,
                events: event_rx,
                process_group: 0,
                exit_emitted: false,
                eof: false,
            },
        );
        (backend, events)
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn default_shell_roundtrips_bytes_through_a_real_pty() {
        let notified = Arc::new(Notify::new());
        let notifier: PlatformNotifier = {
            let notified = Arc::clone(&notified);
            Arc::new(move |_| notified.notify_one())
        };
        let mut backend = UnixPtyBackend::new(Handle::current(), notifier);
        let pane = PlatformPaneId::new(1);
        backend
            .submit(PlatformRequest::SpawnPane(SpawnPane {
                pane,
                size: TerminalSize::new(80, 24),
                command: None,
                cwd: None,
                environment: Vec::new(),
            }))
            .expect("default shell spawns");
        let _process = process_guard(&backend, pane);
        backend
            .submit(PlatformRequest::WritePane {
                pane,
                bytes: b"printf WMUX_PTY_ROUNDTRIP; exit 7\n".to_vec(),
            })
            .expect("shell input queues");

        let events = tokio::time::timeout(Duration::from_secs(5), async {
            let mut events = Vec::new();
            loop {
                while let Some(event) = backend.try_next_event(pane).expect("event poll succeeds") {
                    let closed = matches!(event, PlatformEvent::PtyClosed { .. });
                    events.push(event);
                    if closed {
                        return events;
                    }
                }
                notified.notified().await;
            }
        })
        .await
        .expect("PTY lifecycle completes");
        let mut output = Vec::new();
        let mut exit_code = None;
        for event in &events {
            match event {
                PlatformEvent::PtyOutput { bytes, .. } => output.extend_from_slice(bytes),
                PlatformEvent::PtyExited {
                    exit_code: code, ..
                } => exit_code = Some(*code),
                _ => {}
            }
        }

        assert!(String::from_utf8_lossy(&output).contains("WMUX_PTY_ROUNDTRIP"));
        assert_eq!(exit_code, Some(Some(7)));
        assert!(
            matches!(events.last(), Some(PlatformEvent::PtyClosed { pane: id }) if *id == pane)
        );
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn large_input_survives_short_nonblocking_writes_in_order() {
        const INPUT_BYTES: usize = 256 * 1024;
        let notified = Arc::new(Notify::new());
        let notifier: PlatformNotifier = {
            let notified = Arc::clone(&notified);
            Arc::new(move |_| notified.notify_one())
        };
        let mut backend = UnixPtyBackend::new(Handle::current(), notifier);
        let pane = PlatformPaneId::new(2);
        backend
            .submit(PlatformRequest::SpawnPane(SpawnPane {
                pane,
                size: TerminalSize::new(80, 24),
                command: Some(CommandSpec {
                    program: OsString::from("/bin/sh"),
                    args: vec![
                        OsString::from("-c"),
                        OsString::from(concat!(
                            "stty -echo -icanon min 1 time 0; printf READY; ",
                            "dd bs=1 count=262144 2>/dev/null | wc -c"
                        )),
                    ],
                }),
                cwd: None,
                environment: Vec::new(),
            }))
            .expect("reader shell spawns");
        let _process = process_guard(&backend, pane);

        let mut output = Vec::new();
        tokio::time::timeout(Duration::from_secs(5), async {
            while !output.windows(5).any(|bytes| bytes == b"READY") {
                while let Some(event) = backend.try_next_event(pane).expect("event poll succeeds") {
                    match event {
                        PlatformEvent::PtyOutput { bytes, .. } => output.extend_from_slice(&bytes),
                        PlatformEvent::BackendError { error, .. } => {
                            panic!("PTY failed before READY: {error}")
                        }
                        PlatformEvent::PtyExited { exit_code, .. } => {
                            panic!("PTY exited before READY: {exit_code:?}")
                        }
                        PlatformEvent::PtyClosed { .. } => panic!("PTY closed before READY"),
                    }
                }
                if output.windows(5).any(|bytes| bytes == b"READY") {
                    break;
                }
                notified.notified().await;
            }
        })
        .await
        .expect("child enters noncanonical mode");
        backend
            .submit(PlatformRequest::WritePane {
                pane,
                bytes: vec![b'x'; INPUT_BYTES],
            })
            .expect("large input queues");

        tokio::time::timeout(Duration::from_secs(10), async {
            loop {
                while let Some(event) = backend.try_next_event(pane).expect("event poll succeeds") {
                    match event {
                        PlatformEvent::PtyOutput { bytes, .. } => output.extend_from_slice(&bytes),
                        PlatformEvent::PtyClosed { .. } => return,
                        _ => {}
                    }
                }
                notified.notified().await;
            }
        })
        .await
        .expect("large PTY write completes");

        assert!(String::from_utf8_lossy(&output).contains("262144"));
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn resize_storm_leaves_the_final_kernel_winsize() {
        let notifier: PlatformNotifier = Arc::new(|_| {});
        let mut backend = UnixPtyBackend::new(Handle::current(), notifier);
        let pane = PlatformPaneId::new(3);
        backend
            .submit(PlatformRequest::SpawnPane(SpawnPane {
                pane,
                size: TerminalSize::new(80, 24),
                command: Some(CommandSpec {
                    program: OsString::from("/bin/sh"),
                    args: vec![OsString::from("-c"), OsString::from("exec sleep 30")],
                }),
                cwd: None,
                environment: Vec::new(),
            }))
            .expect("sleeping pane spawns");
        let _process = process_guard(&backend, pane);

        for iteration in 0..2_000_u16 {
            backend
                .submit(PlatformRequest::ResizePane {
                    pane,
                    size: TerminalSize::new(40 + (iteration % 180), 10 + (iteration % 90)),
                })
                .expect("resize succeeds");
        }
        let expected = TerminalSize::new(211, 73);
        backend
            .submit(PlatformRequest::ResizePane {
                pane,
                size: expected,
            })
            .expect("final resize succeeds");

        assert_eq!(kernel_size(&backend, pane), expected);
        backend
            .submit(PlatformRequest::TerminatePane {
                pane,
                mode: TerminationMode::Force,
            })
            .expect("test child terminates");
    }

    #[tokio::test(flavor = "multi_thread", worker_threads = 2)]
    async fn stalled_pane_input_applies_bounded_backpressure() {
        let notifier: PlatformNotifier = Arc::new(|_| {});
        let mut backend = UnixPtyBackend::new(Handle::current(), notifier);
        let pane = PlatformPaneId::new(8);
        backend
            .submit(PlatformRequest::SpawnPane(SpawnPane {
                pane,
                size: TerminalSize::new(80, 24),
                command: Some(CommandSpec {
                    program: OsString::from("/bin/sh"),
                    args: vec![OsString::from("-c"), OsString::from("exec sleep 30")],
                }),
                cwd: None,
                environment: Vec::new(),
            }))
            .expect("non-reading pane spawns");
        let _process = process_guard(&backend, pane);

        let mut backpressured = false;
        for _ in 0..256 {
            if let Err(error) = backend.submit(PlatformRequest::WritePane {
                pane,
                bytes: vec![b'x'; 16 * 1024],
            }) {
                assert_eq!(error.kind(), PlatformErrorKind::Busy);
                backpressured = true;
                break;
            }
        }

        assert!(backpressured, "stalled input queue must be bounded");
    }

    #[tokio::test]
    async fn output_after_child_exit_is_drained_before_closed() {
        let pane = PlatformPaneId::new(4);
        let (mut backend, events) = backend_with_event_queue(pane);
        events
            .try_send(InternalEvent::Exit(Some(9)))
            .expect("exit queues");
        events
            .try_send(InternalEvent::Output(b"late output".to_vec()))
            .expect("late output queues");
        events.try_send(InternalEvent::Eof).expect("EOF queues");

        assert!(matches!(
            backend.try_next_event(pane).expect("exit poll succeeds"),
            Some(PlatformEvent::PtyExited {
                pane: id,
                exit_code: Some(9)
            }) if id == pane
        ));
        assert!(matches!(
            backend.try_next_event(pane).expect("output poll succeeds"),
            Some(PlatformEvent::PtyOutput { pane: id, bytes })
                if id == pane && bytes == b"late output"
        ));
        assert!(matches!(
            backend.try_next_event(pane).expect("close poll succeeds"),
            Some(PlatformEvent::PtyClosed { pane: id }) if id == pane
        ));
    }

    #[tokio::test]
    async fn eof_waits_for_child_exit_before_closing() {
        let pane = PlatformPaneId::new(5);
        let (mut backend, events) = backend_with_event_queue(pane);
        events.try_send(InternalEvent::Eof).expect("EOF queues");

        assert_eq!(
            backend.try_next_event(pane).expect("EOF poll succeeds"),
            None
        );
        events
            .try_send(InternalEvent::Exit(None))
            .expect("signal exit queues");
        assert!(matches!(
            backend.try_next_event(pane).expect("exit poll succeeds"),
            Some(PlatformEvent::PtyExited {
                pane: id,
                exit_code: None
            }) if id == pane
        ));
        assert!(matches!(
            backend.try_next_event(pane).expect("close poll succeeds"),
            Some(PlatformEvent::PtyClosed { pane: id }) if id == pane
        ));
    }

    #[tokio::test]
    async fn eof_without_status_emits_unknown_exit_then_closed() {
        let pane = PlatformPaneId::new(6);
        let (mut backend, events) = backend_with_event_queue(pane);
        events.try_send(InternalEvent::Eof).expect("EOF queues");
        drop(events);

        assert!(matches!(
            backend.try_next_event(pane).expect("exit poll succeeds"),
            Some(PlatformEvent::PtyExited {
                pane: id,
                exit_code: None
            }) if id == pane
        ));
        assert!(matches!(
            backend.try_next_event(pane).expect("close poll succeeds"),
            Some(PlatformEvent::PtyClosed { pane: id }) if id == pane
        ));
    }

    #[tokio::test]
    async fn duplicate_and_post_close_events_are_impossible() {
        let pane = PlatformPaneId::new(7);
        let (mut backend, events) = backend_with_event_queue(pane);
        events
            .try_send(InternalEvent::Exit(Some(0)))
            .expect("first exit queues");
        events
            .try_send(InternalEvent::Exit(Some(1)))
            .expect("duplicate exit queues");
        events.try_send(InternalEvent::Eof).expect("EOF queues");
        drop(events);

        let mut exits = 0;
        let mut closes = 0;
        loop {
            match backend
                .try_next_event(pane)
                .expect("lifecycle poll succeeds")
            {
                Some(PlatformEvent::PtyExited { .. }) => exits += 1,
                Some(PlatformEvent::PtyClosed { .. }) => {
                    closes += 1;
                    break;
                }
                Some(_) | None => {}
            }
        }
        assert_eq!(exits, 1);
        assert_eq!(closes, 1);
        assert_eq!(
            backend
                .try_next_event(pane)
                .expect_err("closed pane cannot emit again")
                .kind(),
            PlatformErrorKind::NotFound
        );
    }
}
