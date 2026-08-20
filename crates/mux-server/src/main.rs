use mux_core::{
    encode_pane_input, parse_terminal_input, render_full_into_state, ClientId, CommandParser,
    PaneId, ParsedCommand, QueuedCommand, RenderState, Screen, ServerState, ServerStateError,
    VtParser,
};
use mux_platform::terminal::TerminalSize;
use mux_platform_windows::{
    conpty::{spawn_shell_pane, ConptyEvent, ConptyPane},
    named_pipe::{accept, has_pending_bytes, NamedPipeEndpoint, ServerLock},
};
use mux_protocol::{
    read_message, write_message, CommandResponse, ProtocolMessage, PROTOCOL_VERSION,
};
use std::{
    collections::{BTreeMap, VecDeque},
    fs::{File, OpenOptions},
    io::{self, Write},
    process,
    sync::{mpsc, Arc},
    thread,
    time::{Duration, SystemTime},
};

fn main() -> io::Result<()> {
    let endpoint = NamedPipeEndpoint::default_for_current_user();
    let lock = Arc::new(ServerLock::acquire(&endpoint)?);
    let (events_tx, events_rx) = mpsc::channel();

    spawn_accept_loop(endpoint.clone(), events_tx.clone());

    println!("wmux server listening on {}", endpoint.pipe_name());

    let parser = CommandParser;
    let mut runtime = ServerRuntime::new(process::id(), parser, Arc::clone(&lock), events_tx);
    runtime.run(events_rx);
    Ok(())
}

fn spawn_accept_loop(endpoint: NamedPipeEndpoint, events_tx: mpsc::Sender<RuntimeEvent>) {
    thread::spawn(move || loop {
        match accept(&endpoint) {
            Ok(pipe) => {
                let events_tx = events_tx.clone();
                thread::spawn(move || {
                    if let Err(error) = handle_client(pipe, events_tx) {
                        eprintln!("event=client_error error={error}");
                    }
                });
            }
            Err(error) => {
                eprintln!("event=accept_error error={error}");
                thread::sleep(Duration::from_millis(100));
            }
        }
    });
}

#[derive(Debug)]
struct ServerRuntime {
    state: ServerState,
    parser: CommandParser,
    lock: Arc<ServerLock>,
    events_tx: mpsc::Sender<RuntimeEvent>,
    pending_results: Vec<CommandExecution>,
    attach_contexts: BTreeMap<u64, AttachContext>,
    panes: BTreeMap<PaneId, PaneRuntime>,
    attached_clients: BTreeMap<ClientId, AttachedClient>,
    client_outputs: BTreeMap<ClientId, VecDeque<ProtocolMessage>>,
}

impl ServerRuntime {
    fn new(
        server_pid: u32,
        parser: CommandParser,
        lock: Arc<ServerLock>,
        events_tx: mpsc::Sender<RuntimeEvent>,
    ) -> Self {
        Self {
            state: ServerState::new(server_pid),
            parser,
            lock,
            events_tx,
            pending_results: Vec::new(),
            attach_contexts: BTreeMap::new(),
            panes: BTreeMap::new(),
            attached_clients: BTreeMap::new(),
            client_outputs: BTreeMap::new(),
        }
    }

    fn run(&mut self, events_rx: mpsc::Receiver<RuntimeEvent>) {
        self.state
            .record_message(format!("server started pid={}", self.state.server_pid));

        while let Ok(event) = events_rx.recv() {
            self.handle_event(event);
            self.drain_command_scheduler();
        }
    }

    fn handle_event(&mut self, event: RuntimeEvent) {
        match event {
            RuntimeEvent::RegisterClient { pid, reply } => {
                let client_id = self.state.register_client(pid);
                self.client_outputs.insert(client_id, VecDeque::new());
                eprintln!(
                    "event=client_registered client_id={} pid={pid}",
                    client_id.raw()
                );
                let _ = reply.send(client_id);
            }
            RuntimeEvent::ClientCommand {
                client_id,
                raw,
                attach,
                reply,
            } => {
                let parse_result = self.parser.parse(&raw);
                match parse_result {
                    Ok(parsed) => {
                        match self
                            .state
                            .enqueue_client_command(client_id, raw.clone(), parsed)
                        {
                            Ok(sequence) => {
                                if let Some(attach) = attach {
                                    self.attach_contexts.insert(sequence, attach);
                                }
                                eprintln!(
                                    "event=command_enqueued sequence={sequence} client_id={} command={raw:?}",
                                    client_id.raw()
                                );
                                self.drain_command_scheduler();
                                self.complete_command_when_ready(sequence, reply);
                            }
                            Err(error) => {
                                let _ = reply.send(CommandReply::error(format!(
                                    "could not queue command: {}",
                                    format_state_error(error)
                                )));
                            }
                        }
                    }
                    Err(error) => {
                        self.state.record_message(format!(
                            "client {} command parse error: {}",
                            client_id.raw(),
                            error.message()
                        ));
                        let _ = reply.send(CommandReply::error(error.message().to_string()));
                    }
                }
            }
            RuntimeEvent::ClientInput { client_id, bytes } => {
                self.handle_client_input(client_id, &bytes);
            }
            RuntimeEvent::ClientResize { client_id, size } => {
                self.handle_client_resize(client_id, size);
            }
            RuntimeEvent::ClientDetach { client_id, reason } => {
                self.detach_client(client_id, reason.as_deref());
            }
            RuntimeEvent::ClientDisconnected { client_id } => {
                self.detach_client(client_id, Some("client disconnected"));
                self.state.remove_client(client_id);
                self.client_outputs.remove(&client_id);
                eprintln!("event=client_disconnected client_id={}", client_id.raw());
            }
            RuntimeEvent::DrainClientOutput { client_id, reply } => {
                let messages = self
                    .client_outputs
                    .get_mut(&client_id)
                    .map(drain_batched_output)
                    .unwrap_or_default();
                let _ = reply.send(messages);
            }
            RuntimeEvent::PaneEvent(event) => {
                self.handle_pane_event(event);
            }
            RuntimeEvent::Shutdown { reply } => {
                self.notify_attached_clients("server stopping");
                self.terminate_all_panes();
                self.lock.remove_now();
                self.state.record_message("server shutdown requested");
                let _ = reply.send(CommandReply::success("server stopping", true));
            }
        }
    }

    fn drain_command_scheduler(&mut self) {
        while let Some(command) = self.state.pop_next_command() {
            let result = self.execute_command(command);
            self.state.record_message(format!(
                "command {} completed success={}",
                result.sequence, result.reply.response.success
            ));
            self.pending_results_insert(result);
        }
    }

    fn execute_command(&mut self, command: QueuedCommand) -> CommandExecution {
        eprintln!(
            "event=command_start sequence={} client_id={} command={:?}",
            command.sequence,
            command.client_id.raw(),
            command.raw
        );

        let reply = match command.parsed {
            ParsedCommand::ServerStatus => CommandReply::success(
                format!(
                    "server running: pid={} clients={} sessions={} panes={}",
                    self.state.server_pid,
                    self.state.clients.len(),
                    self.state.sessions.len(),
                    self.panes.len()
                ),
                false,
            ),
            ParsedCommand::ListClients => {
                CommandReply::success(self.state.list_clients(SystemTime::now()), false)
            }
            ParsedCommand::NewSession { name } => {
                self.execute_new_session(command.sequence, command.client_id, name)
            }
            ParsedCommand::AttachSession { target } => {
                self.execute_attach_session(command.sequence, command.client_id, target)
            }
            ParsedCommand::DisplayMessage { message } => {
                let message = if message.is_empty() {
                    "display-message".to_string()
                } else {
                    message
                };
                self.state
                    .record_message(format!("client {}: {message}", command.client_id.raw()));
                CommandReply::success(message, false)
            }
            ParsedCommand::KillServer => {
                self.notify_attached_clients("server stopping");
                self.terminate_all_panes();
                self.lock.remove_now();
                CommandReply::success("server stopping", true)
            }
            ParsedCommand::ShowMessages => {
                CommandReply::success(self.state.show_messages(SystemTime::now()), false)
            }
        };

        eprintln!(
            "event=command_finish sequence={} client_id={} success={} shutdown={}",
            command.sequence,
            command.client_id.raw(),
            reply.response.success,
            reply.shutdown
        );

        CommandExecution {
            sequence: command.sequence,
            reply,
        }
    }

    fn execute_new_session(
        &mut self,
        sequence: u64,
        client_id: ClientId,
        name: Option<String>,
    ) -> CommandReply {
        let Some(attach) = self.attach_contexts.remove(&sequence) else {
            return CommandReply::error("new-session requires an attached terminal client");
        };

        let created = self
            .state
            .create_session_with_pane(name, "powershell".to_string());
        match spawn_shell_pane(created.pane_id, attach.size, self.conpty_event_sender()) {
            Ok(pane) => {
                let process_id = pane_process_id(&pane);
                eprintln!(
                    "event=pane_spawned pane_id={} process_id={:?}",
                    created.pane_id.raw(),
                    process_id
                );
                self.state.set_pane_process_id(created.pane_id, process_id);
                self.panes.insert(
                    created.pane_id,
                    PaneRuntime {
                        pane,
                        screen: Screen::new(attach.size.columns, attach.size.rows),
                        parser: VtParser::new(),
                    },
                );
                self.attach_client_to_pane(client_id, created.session_id, created.pane_id);
                CommandReply::success(
                    format!(
                        "attached session={} pane={}",
                        created.session_id.raw(),
                        created.pane_id.raw()
                    ),
                    false,
                )
            }
            Err(error) => CommandReply::error(format!("could not spawn ConPTY shell: {error}")),
        }
    }

    fn execute_attach_session(
        &mut self,
        sequence: u64,
        client_id: ClientId,
        target: Option<String>,
    ) -> CommandReply {
        let Some(attach) = self.attach_contexts.remove(&sequence) else {
            return CommandReply::error("attach-session requires an attached terminal client");
        };
        let Some(session_id) = self.state.find_session(target.as_deref()) else {
            return CommandReply::error("no sessions");
        };
        let Some(pane_id) = self.state.active_pane_for_session(session_id) else {
            return CommandReply::error("target session has no active pane");
        };
        if let Some(pane) = self.panes.get_mut(&pane_id) {
            let _ = pane.pane.resize(attach.size);
        }
        self.attach_client_to_pane(client_id, session_id, pane_id);
        CommandReply::success(
            format!(
                "attached session={} pane={}",
                session_id.raw(),
                pane_id.raw()
            ),
            false,
        )
    }

    fn attach_client_to_pane(
        &mut self,
        client_id: ClientId,
        session_id: mux_core::SessionId,
        pane_id: PaneId,
    ) {
        let _ = self.state.attach_client_to_session(client_id, session_id);
        self.attached_clients.insert(
            client_id,
            AttachedClient {
                pane_id,
                render_state: RenderState::new(1, 1),
            },
        );
        self.queue_pane_redraw(client_id, pane_id);
        eprintln!(
            "event=client_attached client_id={} session_id={} pane_id={}",
            client_id.raw(),
            session_id.raw(),
            pane_id.raw()
        );
    }

    fn handle_client_input(&mut self, client_id: ClientId, bytes: &[u8]) {
        eprintln!(
            "event=client_input client_id={} bytes={}",
            client_id.raw(),
            bytes.len()
        );
        let Some(pane_id) = self
            .attached_clients
            .get(&client_id)
            .map(|attached| attached.pane_id)
        else {
            return;
        };
        let Some(pane) = self.panes.get_mut(&pane_id) else {
            return;
        };
        trace_terminal(format_args!(
            "input client_id={} pane_id={} bytes={} hex={} text={:?}",
            client_id.raw(),
            pane_id.raw(),
            bytes.len(),
            hex_preview(bytes),
            text_preview(bytes)
        ));
        let events = parse_terminal_input(bytes);
        let encoded = encode_pane_input(&events);
        trace_terminal(format_args!(
            "encoded_input client_id={} pane_id={} events={:?} bytes={} hex={} text={:?}",
            client_id.raw(),
            pane_id.raw(),
            events,
            encoded.len(),
            hex_preview(&encoded),
            text_preview(&encoded)
        ));
        if encoded.is_empty() {
            return;
        }
        if let Err(error) = pane.pane.write_input(&encoded) {
            self.state.record_message(format!(
                "client {} input write failed: {error}",
                client_id.raw()
            ));
        }
    }

    fn handle_client_resize(&mut self, client_id: ClientId, size: TerminalSize) {
        eprintln!(
            "event=client_resize client_id={} columns={} rows={}",
            client_id.raw(),
            size.columns,
            size.rows
        );
        let Some(pane_id) = self
            .attached_clients
            .get(&client_id)
            .map(|attached| attached.pane_id)
        else {
            return;
        };
        let Some(pane) = self.panes.get_mut(&pane_id) else {
            return;
        };
        if let Err(error) = pane.pane.resize(size) {
            self.state
                .record_message(format!("client {} resize failed: {error}", client_id.raw()));
            return;
        }
        pane.screen.resize(size.columns, size.rows);
        if let Some(attached) = self.attached_clients.get_mut(&client_id) {
            attached.render_state.invalidate();
        }
        self.queue_pane_redraw(client_id, pane_id);
    }

    fn detach_client(&mut self, client_id: ClientId, reason: Option<&str>) {
        if self.attached_clients.remove(&client_id).is_some() {
            self.queue_client_output(
                client_id,
                ProtocolMessage::Exit {
                    code: 0,
                    message: Some(reason.unwrap_or("detached").to_string()),
                },
            );
            self.state.detach_client(client_id);
            eprintln!("event=client_detached client_id={}", client_id.raw());
        }
    }

    fn handle_pane_event(&mut self, event: ConptyEvent) {
        match event {
            ConptyEvent::Output { pane_id, bytes } => {
                eprintln!(
                    "event=pane_output pane_id={} bytes={}",
                    pane_id.raw(),
                    bytes.len()
                );
                let Some(pane) = self.panes.get_mut(&pane_id) else {
                    return;
                };
                let before_row = pane.screen.cursor_row();
                let before_column = pane.screen.cursor_column();
                trace_terminal(format_args!(
                    "output pane_id={} bytes={} cursor_before={},{} hex={} text={:?}",
                    pane_id.raw(),
                    bytes.len(),
                    before_row + 1,
                    before_column + 1,
                    hex_preview(&bytes),
                    text_preview(&bytes)
                ));
                pane.parser.feed(&mut pane.screen, &bytes);
                trace_terminal(format_args!(
                    "parsed pane_id={} cursor_after={},{} dirty_rows={:?}",
                    pane_id.raw(),
                    pane.screen.cursor_row() + 1,
                    pane.screen.cursor_column() + 1,
                    pane.screen.dirty_rows()
                ));
                pane.screen.clear_dirty();
                self.broadcast_pane_bytes(pane_id, bytes);
            }
            ConptyEvent::Exited { pane_id, exit_code } => {
                eprintln!(
                    "event=pane_exited pane_id={} exit_code={exit_code}",
                    pane_id.raw()
                );
                self.state
                    .record_message(format!("pane {} exited code={exit_code}", pane_id.raw()));
                self.broadcast_to_pane(
                    pane_id,
                    ProtocolMessage::Exit {
                        code: exit_code as i32,
                        message: Some(format!("pane exited code={exit_code}")),
                    },
                );
                self.panes.remove(&pane_id);
                self.attached_clients
                    .retain(|_, attached| attached.pane_id != pane_id);
            }
            ConptyEvent::Closed { pane_id } => {
                self.state
                    .record_message(format!("pane {} output closed", pane_id.raw()));
            }
            ConptyEvent::Error { pane_id, message } => {
                self.state
                    .record_message(format!("pane {} error: {message}", pane_id.raw()));
                self.broadcast_to_pane(
                    pane_id,
                    ProtocolMessage::Exit {
                        code: 1,
                        message: Some(message),
                    },
                );
            }
        }
    }

    fn broadcast_to_pane(&mut self, pane_id: PaneId, message: ProtocolMessage) {
        let clients = self
            .attached_clients
            .iter()
            .filter_map(|(client_id, attached)| {
                if attached.pane_id == pane_id {
                    Some(*client_id)
                } else {
                    None
                }
            })
            .collect::<Vec<_>>();

        for client_id in clients {
            self.queue_client_output(client_id, message.clone());
        }
    }

    fn broadcast_pane_bytes(&mut self, pane_id: PaneId, bytes: Vec<u8>) {
        if bytes.is_empty() {
            return;
        }

        let client_ids = self
            .attached_clients
            .iter()
            .filter_map(|(client_id, attached)| (attached.pane_id == pane_id).then_some(*client_id))
            .collect::<Vec<_>>();

        for client_id in client_ids {
            self.queue_client_output(
                client_id,
                ProtocolMessage::Output {
                    bytes: bytes.clone(),
                },
            );
        }
    }

    fn queue_pane_redraw(&mut self, client_id: ClientId, pane_id: PaneId) {
        let Some(bytes) = self.render_full_for_client(client_id, pane_id) else {
            return;
        };
        self.queue_client_output(client_id, ProtocolMessage::Output { bytes });
    }

    fn terminate_all_panes(&mut self) {
        for pane in self.panes.values_mut() {
            pane.pane.terminate(1);
        }
        self.panes.clear();
        self.attached_clients.clear();
    }

    fn notify_attached_clients(&mut self, message: &str) {
        let clients = self.attached_clients.keys().copied().collect::<Vec<_>>();
        for client_id in clients {
            self.queue_client_output(
                client_id,
                ProtocolMessage::Exit {
                    code: 0,
                    message: Some(message.to_string()),
                },
            );
        }
    }

    fn queue_client_output(&mut self, client_id: ClientId, message: ProtocolMessage) {
        if let ProtocolMessage::Output { bytes } = message {
            let queue = self.client_outputs.entry(client_id).or_default();
            if let Some(ProtocolMessage::Output { bytes: pending }) = queue.back_mut() {
                pending.extend_from_slice(&bytes);
            } else {
                queue.push_back(ProtocolMessage::Output { bytes });
            }
            return;
        }

        self.client_outputs
            .entry(client_id)
            .or_default()
            .push_back(message);
    }

    fn render_full_for_client(&mut self, client_id: ClientId, pane_id: PaneId) -> Option<Vec<u8>> {
        let pane = self.panes.get(&pane_id)?;
        let attached = self.attached_clients.get_mut(&client_id)?;
        Some(render_full_into_state(
            &pane.screen,
            &mut attached.render_state,
        ))
    }

    fn conpty_event_sender(&self) -> mpsc::Sender<ConptyEvent> {
        let runtime_tx = self.events_tx.clone();
        let (tx, rx) = mpsc::channel();
        thread::spawn(move || {
            while let Ok(event) = rx.recv() {
                if runtime_tx.send(RuntimeEvent::PaneEvent(event)).is_err() {
                    break;
                }
            }
        });
        tx
    }

    fn complete_command_when_ready(&mut self, sequence: u64, reply: mpsc::Sender<CommandReply>) {
        if let Some(result) = self.take_pending_result(sequence) {
            let _ = reply.send(result.reply);
        } else {
            let _ = reply.send(CommandReply::error(format!(
                "command {sequence} was not completed"
            )));
        }
    }

    fn pending_results_insert(&mut self, result: CommandExecution) {
        self.pending_results.push(result);
    }

    fn take_pending_result(&mut self, sequence: u64) -> Option<CommandExecution> {
        let index = self
            .pending_results
            .iter()
            .position(|result| result.sequence == sequence)?;
        Some(self.pending_results.remove(index))
    }
}

fn pane_process_id(_pane: &ConptyPane) -> Option<u32> {
    Some(_pane.process_id())
}

fn drain_batched_output(queue: &mut VecDeque<ProtocolMessage>) -> Vec<ProtocolMessage> {
    let mut messages = Vec::new();
    let mut pending_output = Vec::new();

    for message in queue.drain(..) {
        match message {
            ProtocolMessage::Output { bytes } => {
                pending_output.extend_from_slice(&bytes);
            }
            other => {
                if !pending_output.is_empty() {
                    messages.push(ProtocolMessage::Output {
                        bytes: std::mem::take(&mut pending_output),
                    });
                }
                messages.push(other);
            }
        }
    }

    if !pending_output.is_empty() {
        messages.push(ProtocolMessage::Output {
            bytes: pending_output,
        });
    }

    messages
}

#[derive(Debug)]
struct PaneRuntime {
    pane: ConptyPane,
    screen: Screen,
    parser: VtParser,
}

#[derive(Debug)]
struct AttachedClient {
    pane_id: PaneId,
    render_state: RenderState,
}

#[derive(Debug)]
struct AttachContext {
    size: TerminalSize,
}

#[derive(Debug)]
enum RuntimeEvent {
    RegisterClient {
        pid: u32,
        reply: mpsc::Sender<ClientId>,
    },
    ClientCommand {
        client_id: ClientId,
        raw: String,
        attach: Option<AttachContext>,
        reply: mpsc::Sender<CommandReply>,
    },
    ClientInput {
        client_id: ClientId,
        bytes: Vec<u8>,
    },
    ClientResize {
        client_id: ClientId,
        size: TerminalSize,
    },
    ClientDetach {
        client_id: ClientId,
        reason: Option<String>,
    },
    ClientDisconnected {
        client_id: ClientId,
    },
    DrainClientOutput {
        client_id: ClientId,
        reply: mpsc::Sender<Vec<ProtocolMessage>>,
    },
    PaneEvent(ConptyEvent),
    Shutdown {
        reply: mpsc::Sender<CommandReply>,
    },
}

#[derive(Debug)]
struct CommandExecution {
    sequence: u64,
    reply: CommandReply,
}

#[derive(Clone, Debug)]
struct CommandReply {
    response: CommandResponse,
    shutdown: bool,
}

impl CommandReply {
    fn success(message: impl Into<String>, shutdown: bool) -> Self {
        Self {
            response: CommandResponse {
                success: true,
                message: message.into(),
            },
            shutdown,
        }
    }

    fn error(message: impl Into<String>) -> Self {
        Self {
            response: CommandResponse {
                success: false,
                message: message.into(),
            },
            shutdown: false,
        }
    }
}

fn handle_client(mut pipe: File, events_tx: mpsc::Sender<RuntimeEvent>) -> io::Result<()> {
    let Some((request_id, message)) = read_message(&mut pipe)? else {
        return Ok(());
    };

    let client_pid = match message {
        ProtocolMessage::Hello {
            version,
            client_pid,
        } => {
            if version != PROTOCOL_VERSION {
                write_message(
                    &mut pipe,
                    request_id,
                    &ProtocolMessage::HelloError {
                        server_version: PROTOCOL_VERSION,
                        message: format!(
                            "unsupported protocol version {version}; server expects {PROTOCOL_VERSION}"
                        ),
                    },
                )?;
                return Ok(());
            }
            client_pid
        }
        other => {
            write_message(
                &mut pipe,
                request_id,
                &ProtocolMessage::CommandResponse(CommandResponse {
                    success: false,
                    message: format!("expected Hello, got {other:?}"),
                }),
            )?;
            return Ok(());
        }
    };

    let client_id = register_client(&events_tx, client_pid)?;

    write_message(
        &mut pipe,
        request_id,
        &ProtocolMessage::HelloOk {
            version: PROTOCOL_VERSION,
            server_pid: process::id(),
        },
    )?;

    let result = handle_client_messages(&mut pipe, &events_tx, client_id);
    let _ = events_tx.send(RuntimeEvent::ClientDisconnected { client_id });
    result
}

fn handle_client_messages(
    pipe: &mut File,
    events_tx: &mpsc::Sender<RuntimeEvent>,
    client_id: ClientId,
) -> io::Result<()> {
    loop {
        for outgoing in drain_client_output(events_tx, client_id)? {
            write_message(pipe, 0, &outgoing)?;
        }

        match has_pending_bytes(pipe) {
            Ok(true) => {}
            Ok(false) => {
                thread::sleep(Duration::from_millis(5));
                continue;
            }
            Err(error)
                if error.kind() == io::ErrorKind::BrokenPipe
                    || error.raw_os_error() == Some(109) =>
            {
                break;
            }
            Err(error) => return Err(error),
        }

        let Some((request_id, message)) = read_message(pipe)? else {
            break;
        };

        match message {
            ProtocolMessage::Command(request) => {
                let attach = attach_context_if_interactive(&request.command);
                let reply = submit_command(events_tx, client_id, request.command, attach)?;
                write_message(
                    pipe,
                    request_id,
                    &ProtocolMessage::CommandResponse(reply.response),
                )?;
                if reply.shutdown {
                    thread::sleep(Duration::from_millis(300));
                    process::exit(0);
                }
            }
            ProtocolMessage::Attach(request) => {
                let command = match request.target {
                    Some(target) => format!("attach-session -t {target}"),
                    None => "attach-session".to_string(),
                };
                let attach = Some(AttachContext {
                    size: TerminalSize::cells(80, 24),
                });
                let reply = submit_command(events_tx, client_id, command, attach)?;
                write_message(
                    pipe,
                    request_id,
                    &ProtocolMessage::CommandResponse(reply.response),
                )?;
            }
            ProtocolMessage::Input { bytes } => {
                let _ = events_tx.send(RuntimeEvent::ClientInput { client_id, bytes });
            }
            ProtocolMessage::Resize {
                cols,
                rows,
                xpixel,
                ypixel,
            } => {
                let _ = events_tx.send(RuntimeEvent::ClientResize {
                    client_id,
                    size: TerminalSize {
                        columns: cols,
                        rows,
                        xpixel,
                        ypixel,
                    },
                });
            }
            ProtocolMessage::Detach(request) => {
                let _ = events_tx.send(RuntimeEvent::ClientDetach {
                    client_id,
                    reason: request.reason,
                });
            }
            ProtocolMessage::Shutdown => {
                let reply = submit_shutdown(events_tx)?;
                write_message(
                    pipe,
                    request_id,
                    &ProtocolMessage::CommandResponse(reply.response),
                )?;
                thread::sleep(Duration::from_millis(300));
                process::exit(0);
            }
            other => {
                write_message(
                    pipe,
                    request_id,
                    &ProtocolMessage::CommandResponse(CommandResponse {
                        success: false,
                        message: format!("unsupported phase 3 message: {other:?}"),
                    }),
                )?;
            }
        }
    }

    Ok(())
}

fn attach_context_if_interactive(command: &str) -> Option<AttachContext> {
    let command = command.trim();
    if command.starts_with("new-session")
        || command.starts_with("new ")
        || command.starts_with("attach-session")
        || command.starts_with("attach ")
    {
        Some(AttachContext {
            size: TerminalSize::cells(80, 24),
        })
    } else {
        None
    }
}

fn drain_client_output(
    events_tx: &mpsc::Sender<RuntimeEvent>,
    client_id: ClientId,
) -> io::Result<Vec<ProtocolMessage>> {
    let (reply_tx, reply_rx) = mpsc::channel();
    events_tx
        .send(RuntimeEvent::DrainClientOutput {
            client_id,
            reply: reply_tx,
        })
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))?;
    reply_rx
        .recv()
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))
}

fn register_client(events_tx: &mpsc::Sender<RuntimeEvent>, pid: u32) -> io::Result<ClientId> {
    let (reply_tx, reply_rx) = mpsc::channel();
    events_tx
        .send(RuntimeEvent::RegisterClient {
            pid,
            reply: reply_tx,
        })
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))?;
    reply_rx
        .recv()
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))
}

fn submit_command(
    events_tx: &mpsc::Sender<RuntimeEvent>,
    client_id: ClientId,
    raw: String,
    attach: Option<AttachContext>,
) -> io::Result<CommandReply> {
    let (reply_tx, reply_rx) = mpsc::channel();
    events_tx
        .send(RuntimeEvent::ClientCommand {
            client_id,
            raw,
            attach,
            reply: reply_tx,
        })
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))?;
    reply_rx
        .recv()
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))
}

fn submit_shutdown(events_tx: &mpsc::Sender<RuntimeEvent>) -> io::Result<CommandReply> {
    let (reply_tx, reply_rx) = mpsc::channel();
    events_tx
        .send(RuntimeEvent::Shutdown { reply: reply_tx })
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))?;
    reply_rx
        .recv()
        .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "server runtime stopped"))
}

fn format_state_error(error: ServerStateError) -> String {
    match error {
        ServerStateError::UnknownClient(client_id) => {
            format!("unknown client {}", client_id.raw())
        }
    }
}

fn trace_terminal(args: std::fmt::Arguments<'_>) {
    if std::env::var_os("WMUX_TRACE_TERMINAL").is_none() && !cfg!(debug_assertions) {
        return;
    }

    let path = std::env::temp_dir().join("wmux-terminal-trace.log");
    if let Ok(mut file) = OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(file, "{args}");
    }
}

fn hex_preview(bytes: &[u8]) -> String {
    bytes
        .iter()
        .take(512)
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}

fn text_preview(bytes: &[u8]) -> String {
    bytes
        .iter()
        .take(512)
        .flat_map(|byte| std::ascii::escape_default(*byte))
        .map(char::from)
        .collect()
}
