use smallvec::SmallVec;
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    sync::Arc,
};

use crate::{
    ClientId, PaneId, ResizeDirection, ResolveContext, ResolvedTarget, ServerState, SessionId,
    SplitDirection, TargetError, TargetKind, TargetResolver, TargetSpec,
};

mod execute;
mod lexer;
mod parser;

pub use execute::execute;
pub use lexer::{CommandParseError, SourcePosition, SourceSpan};
pub use parser::{parse_command, parse_command_argv, parse_command_text, MAX_COMMANDS};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandList(Arc<[Command]>);

impl CommandList {
    pub fn new(commands: Vec<Command>) -> Result<Self, CommandParseError> {
        if commands.len() > MAX_COMMANDS {
            return Err(CommandParseError::new("command list exceeds 256 commands"));
        }
        Ok(Self(commands.into()))
    }

    pub fn len(&self) -> usize {
        self.0.len()
    }

    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    pub fn iter(&self) -> std::slice::Iter<'_, Command> {
        self.0.iter()
    }
}

impl std::ops::Index<usize> for CommandList {
    type Output = Command;

    fn index(&self, index: usize) -> &Self::Output {
        &self.0[index]
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Command {
    StartServer,
    KillServer,
    ListClients,
    ListSessions,
    ListWindows {
        target: Option<TargetSpec>,
    },
    ListPanes {
        target: Option<TargetSpec>,
    },
    NewSession {
        name: Option<String>,
        group: Option<TargetSpec>,
        attach: bool,
        attach_if_exists: bool,
        cols: u16,
        rows: u16,
    },
    NewWindow {
        name: Option<String>,
    },
    SplitWindow {
        direction: SplitDirection,
        detached: bool,
    },
    SelectWindow {
        target: TargetSpec,
    },
    SelectPane {
        target: PaneTarget,
    },
    ResizePane {
        target: ResizeTarget,
        amount: u16,
    },
    RenameWindow {
        name: String,
    },
    RotateWindow {
        reverse: bool,
    },
    SwapPane {
        direction: ResizeDirection,
    },
    KillPane,
    KillWindow,
    KillSession {
        target: Option<TargetSpec>,
    },
    AttachSession {
        target: Option<TargetSpec>,
    },
    CopyMode,
    DetachClient,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PaneTarget {
    Target(TargetSpec),
    Direction(ResizeDirection),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResizeTarget {
    Direction(ResizeDirection),
    Zoom,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct QueuedCommand {
    pub invocation: u64,
    pub sequence: u64,
    pub client: ClientId,
    pub command: Command,
    pub source: CommandSource,
    pub final_in_list: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandCompletion {
    pub invocation: u64,
    pub client: ClientId,
    pub source: CommandSource,
    pub result: Result<String, String>,
}

impl CommandCompletion {
    pub const fn requires_reply(&self) -> bool {
        matches!(self.source, CommandSource::ClientRequest)
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CommandSource {
    ClientRequest,
    KeyBinding,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandOutcome {
    pub ok: bool,
    pub message: String,
    pub effects: SmallVec<[CommandEffect; 2]>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CommandEffect {
    EnsurePane {
        pane: PaneId,
    },
    PaneInput {
        pane: PaneId,
        bytes: Vec<u8>,
    },
    EnterCopyMode {
        client: ClientId,
    },
    RefreshClient {
        client: ClientId,
    },
    Confirm {
        client: ClientId,
        prompt: String,
        commands: CommandList,
    },
    DetachClient {
        client: ClientId,
    },
    Shutdown {
        requester: ClientId,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(super) struct CommandResult {
    pub sequence: u64,
    pub ok: bool,
    pub message: String,
    pub attached_pane: Option<PaneId>,
}

#[derive(Debug)]
struct PendingInvocation {
    id: u64,
    source: CommandSource,
    commands: CommandList,
    next: usize,
    messages: String,
}

#[derive(Debug, Default)]
pub struct CommandQueue {
    next_invocation: u64,
    next_sequence: u64,
    pending: BTreeMap<ClientId, VecDeque<PendingInvocation>>,
    ready: VecDeque<ClientId>,
    ready_set: BTreeSet<ClientId>,
    active: BTreeMap<ClientId, u64>,
}

impl CommandQueue {
    pub fn push_list(
        &mut self,
        client: ClientId,
        commands: CommandList,
        source: CommandSource,
    ) -> Result<u64, CommandParseError> {
        if commands.is_empty() {
            return Err(CommandParseError::new("command list is empty"));
        }
        if commands.len() > MAX_COMMANDS {
            return Err(CommandParseError::new("command list exceeds 256 commands"));
        }

        self.next_invocation = self
            .next_invocation
            .checked_add(1)
            .expect("command invocation ID space exhausted");
        let invocation = self.next_invocation;
        self.pending
            .entry(client)
            .or_default()
            .push_back(PendingInvocation {
                id: invocation,
                source,
                commands,
                next: 0,
                messages: String::new(),
            });
        self.mark_ready(client);
        Ok(invocation)
    }

    pub fn pop(&mut self) -> Option<QueuedCommand> {
        while let Some(client) = self.ready.pop_front() {
            self.ready_set.remove(&client);
            if self.active.contains_key(&client) {
                continue;
            }
            let invocation = self.pending.get_mut(&client)?.front_mut()?;
            let command = invocation.commands[invocation.next].clone();
            invocation.next += 1;
            self.next_sequence = self
                .next_sequence
                .checked_add(1)
                .expect("command sequence ID space exhausted");
            let queued = QueuedCommand {
                invocation: invocation.id,
                sequence: self.next_sequence,
                client,
                command,
                source: invocation.source,
                final_in_list: invocation.next == invocation.commands.len(),
            };
            self.active.insert(client, invocation.id);
            return Some(queued);
        }
        None
    }

    pub fn finish(
        &mut self,
        command: QueuedCommand,
        result: Result<String, String>,
    ) -> Option<CommandCompletion> {
        if self.active.remove(&command.client) != Some(command.invocation) {
            return None;
        }

        let mut completion = None;
        let mut remove_client_queue = false;
        if let Some(invocations) = self.pending.get_mut(&command.client) {
            let invocation = invocations.front_mut()?;
            if invocation.id != command.invocation {
                return None;
            }

            match result {
                Err(message) => {
                    completion = Some(CommandCompletion {
                        invocation: invocation.id,
                        client: command.client,
                        source: invocation.source,
                        result: Err(message),
                    });
                    invocations.pop_front();
                }
                Ok(message) => {
                    if !message.is_empty() {
                        if !invocation.messages.is_empty() {
                            invocation.messages.push('\n');
                        }
                        invocation.messages.push_str(&message);
                    }
                    if command.final_in_list {
                        let invocation = invocations.pop_front().expect("front was checked above");
                        completion = Some(CommandCompletion {
                            invocation: invocation.id,
                            client: command.client,
                            source: invocation.source,
                            result: Ok(invocation.messages),
                        });
                    }
                }
            }
            remove_client_queue = invocations.is_empty();
        }

        if remove_client_queue {
            self.pending.remove(&command.client);
        } else {
            self.mark_ready(command.client);
        }
        completion
    }

    pub fn remove_client(&mut self, client: ClientId) -> Vec<u64> {
        self.active.remove(&client);
        self.ready.retain(|ready| *ready != client);
        self.ready_set.remove(&client);
        self.pending
            .remove(&client)
            .map(|invocations| {
                invocations
                    .into_iter()
                    .map(|invocation| invocation.id)
                    .collect()
            })
            .unwrap_or_default()
    }

    pub fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }

    fn mark_ready(&mut self, client: ClientId) {
        if !self.active.contains_key(&client) && self.ready_set.insert(client) {
            self.ready.push_back(client);
        }
    }
}

pub(super) fn execute_state_command(
    state: &mut ServerState,
    queued: QueuedCommand,
) -> CommandResult {
    match queued.command {
        Command::StartServer | Command::KillServer | Command::CopyMode => CommandResult {
            sequence: queued.sequence,
            ok: true,
            message: String::new(),
            attached_pane: None,
        },
        Command::ListClients => {
            let mut lines = state
                .clients
                .values()
                .map(|client| match client.attached_pane {
                    Some(pane) => format!("client {}: pane {}", client.id.raw(), pane.raw()),
                    None => format!("client {}: detached", client.id.raw()),
                })
                .collect::<Vec<_>>();
            lines.sort();
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: lines.join("\n"),
                attached_pane: None,
            }
        }
        Command::ListSessions => CommandResult {
            sequence: queued.sequence,
            ok: true,
            message: state.list_sessions(),
            attached_pane: None,
        },
        Command::ListWindows { target } => {
            let session = match resolve_session_target(state, queued.client, target.as_ref()) {
                Ok(session) => session,
                Err(error) => return error_result(queued.sequence, error),
            };
            match state.list_windows(session) {
                Some(message) => CommandResult {
                    sequence: queued.sequence,
                    ok: true,
                    message,
                    attached_pane: None,
                },
                None => error_result(queued.sequence, "could not list windows"),
            }
        }
        Command::ListPanes { target } => {
            let resolved = match resolve_command_target(
                state,
                queued.client,
                TargetKind::Session,
                target.as_ref(),
            ) {
                Ok(resolved) => resolved,
                Err(error) => return error_result(queued.sequence, error),
            };
            let Some(window) = resolved.window else {
                return error_result(queued.sequence, "session has no active window");
            };
            match state.list_panes(window) {
                Some(message) => CommandResult {
                    sequence: queued.sequence,
                    ok: true,
                    message,
                    attached_pane: None,
                },
                None => error_result(queued.sequence, "could not list panes"),
            }
        }
        Command::NewSession {
            name,
            group,
            attach,
            attach_if_exists,
            cols,
            rows,
        } => {
            let name = name.unwrap_or_else(|| "default".to_string());
            if let Some(existing) = TargetResolver::new(state).find_exact_session(&name) {
                if attach_if_exists {
                    return attach_result(state, queued.sequence, queued.client, existing);
                }
                return error_result(queued.sequence, format!("duplicate session: {name}"));
            }
            let created = if let Some(group) = group {
                let target = match resolve_session_target(state, queued.client, Some(&group)) {
                    Ok(target) => target,
                    Err(error) => return error_result(queued.sequence, error),
                };
                match state.create_grouped_session(name, target) {
                    Some(created) => created,
                    None => {
                        return error_result(queued.sequence, "could not create grouped session")
                    }
                }
            } else {
                state.create_session(name, cols, rows)
            };
            if attach {
                attach_result(state, queued.sequence, queued.client, created.session)
            } else {
                CommandResult {
                    sequence: queued.sequence,
                    ok: true,
                    message: String::new(),
                    attached_pane: Some(created.pane),
                }
            }
        }
        Command::NewWindow { name } => {
            let session = match resolve_session_target(state, queued.client, None) {
                Ok(session) => session,
                Err(error) => return error_result(queued.sequence, error),
            };
            match state.create_window(session, name, 80, 24) {
                Some(created) => {
                    let _ = state.attach_client(queued.client, session);
                    CommandResult {
                        sequence: queued.sequence,
                        ok: true,
                        message: String::new(),
                        attached_pane: Some(created.pane),
                    }
                }
                None => error_result(queued.sequence, "could not create window"),
            }
        }
        Command::SplitWindow {
            direction,
            detached,
        } => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Pane, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(window), target) =
                (resolved.session, resolved.window, resolved.pane)
            else {
                return error_result(queued.sequence, "no active pane");
            };
            match state.split_pane(window, target, direction, 80, 24) {
                Some(pane) => {
                    if !detached {
                        let _ = state.attach_client(queued.client, session);
                    }
                    CommandResult {
                        sequence: queued.sequence,
                        ok: true,
                        message: String::new(),
                        attached_pane: Some(pane),
                    }
                }
                None => error_result(queued.sequence, "could not create pane"),
            }
        }
        Command::SelectWindow { target } => {
            let resolved = match resolve_command_target(
                state,
                queued.client,
                TargetKind::Window,
                Some(&target),
            ) {
                Ok(resolved) => resolved,
                Err(error) => return error_result(queued.sequence, error),
            };
            let (Some(session), Some(winlink)) = (resolved.session, resolved.winlink) else {
                return error_result(queued.sequence, "no matching window");
            };
            match state.select_winlink(session, winlink) {
                Some(pane) => {
                    let _ = state.attach_client(queued.client, session);
                    CommandResult {
                        sequence: queued.sequence,
                        ok: true,
                        message: String::new(),
                        attached_pane: Some(pane),
                    }
                }
                None => error_result(queued.sequence, "no matching window"),
            }
        }
        Command::SelectPane { target } => {
            let selected = match target {
                PaneTarget::Target(target) => {
                    let resolved = match resolve_command_target(
                        state,
                        queued.client,
                        TargetKind::Pane,
                        Some(&target),
                    ) {
                        Ok(resolved) => resolved,
                        Err(error) => return error_result(queued.sequence, error),
                    };
                    let (Some(session), Some(window), Some(pane)) =
                        (resolved.session, resolved.window, resolved.pane)
                    else {
                        return error_result(queued.sequence, "no matching pane");
                    };
                    (session, state.select_pane(window, pane))
                }
                PaneTarget::Direction(direction) => {
                    let resolved = match resolve_command_target(
                        state,
                        queued.client,
                        TargetKind::Window,
                        None,
                    ) {
                        Ok(resolved) => resolved,
                        Err(error) => return error_result(queued.sequence, error),
                    };
                    let (Some(session), Some(window)) = (resolved.session, resolved.window) else {
                        return error_result(queued.sequence, "session has no active window");
                    };
                    (session, state.select_adjacent_pane(window, direction))
                }
            };
            match selected {
                (session, Some(pane)) => {
                    let _ = state.attach_client(queued.client, session);
                    CommandResult {
                        sequence: queued.sequence,
                        ok: true,
                        message: String::new(),
                        attached_pane: Some(pane),
                    }
                }
                (_, None) => error_result(queued.sequence, "no matching pane"),
            }
        }
        Command::ResizePane { target, amount } => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Window, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(window)) = (resolved.session, resolved.window) else {
                return error_result(queued.sequence, "session has no active window");
            };
            match target {
                ResizeTarget::Zoom => {
                    let _ = state.toggle_zoom(window);
                }
                ResizeTarget::Direction(direction) => {
                    let _ = state.resize_active_pane(window, direction, amount);
                }
            }
            let pane = state.active_pane_for_session(session);
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: pane,
            }
        }
        Command::RenameWindow { name } => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Window, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(window)) = (resolved.session, resolved.window) else {
                return error_result(queued.sequence, "session has no active window");
            };
            state.rename_window(window, name);
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: state.active_pane_for_session(session),
            }
        }
        Command::RotateWindow { reverse } => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Window, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(window)) = (resolved.session, resolved.window) else {
                return error_result(queued.sequence, "session has no active window");
            };
            state.rotate_window(window, reverse);
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: state.active_pane_for_session(session),
            }
        }
        Command::SwapPane { direction } => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Window, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(window)) = (resolved.session, resolved.window) else {
                return error_result(queued.sequence, "session has no active window");
            };
            state.swap_active_pane(window, direction);
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: state.active_pane_for_session(session),
            }
        }
        Command::KillPane => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Pane, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(pane)) = (resolved.session, resolved.pane) else {
                return error_result(queued.sequence, "no active pane");
            };
            if state.kill_pane(pane).is_none() {
                return error_result(queued.sequence, "no active pane");
            }
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: state.active_pane_for_session(session),
            }
        }
        Command::KillWindow => {
            let resolved =
                match resolve_command_target(state, queued.client, TargetKind::Window, None) {
                    Ok(resolved) => resolved,
                    Err(error) => return error_result(queued.sequence, error),
                };
            let (Some(session), Some(window)) = (resolved.session, resolved.window) else {
                return error_result(queued.sequence, "session has no active window");
            };
            if state.kill_window(window).is_none() {
                return error_result(queued.sequence, "no active window");
            }
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: state.active_pane_for_session(session),
            }
        }
        Command::KillSession { target } => {
            let session = match resolve_session_target(state, queued.client, target.as_ref()) {
                Ok(session) => session,
                Err(error) => return error_result(queued.sequence, error),
            };
            if state.kill_session(session).is_none() {
                return error_result(queued.sequence, "no matching session");
            }
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: None,
            }
        }
        Command::AttachSession { target } => {
            let session = match resolve_session_target(state, queued.client, target.as_ref()) {
                Ok(session) => session,
                Err(error) => return error_result(queued.sequence, error),
            };
            attach_result(state, queued.sequence, queued.client, session)
        }
        Command::DetachClient => {
            state.detach_client(queued.client);
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: None,
            }
        }
    }
}

fn error_result(sequence: u64, message: impl std::fmt::Display) -> CommandResult {
    CommandResult {
        sequence,
        ok: false,
        message: message.to_string(),
        attached_pane: None,
    }
}

fn attach_result(
    state: &mut ServerState,
    sequence: u64,
    client: ClientId,
    session: SessionId,
) -> CommandResult {
    match state.attach_client(client, session) {
        Some(pane) => CommandResult {
            sequence,
            ok: true,
            message: String::new(),
            attached_pane: Some(pane),
        },
        None => CommandResult {
            sequence,
            ok: false,
            message: "could not attach client".to_string(),
            attached_pane: None,
        },
    }
}

fn resolve_command_target(
    state: &ServerState,
    client: ClientId,
    kind: TargetKind,
    target: Option<&TargetSpec>,
) -> Result<ResolvedTarget, TargetError> {
    let context = ResolveContext::for_client(state, client)?;
    let current = TargetSpec::current();
    TargetResolver::new(state).resolve(&context, kind, target.unwrap_or(&current))
}

fn resolve_session_target(
    state: &ServerState,
    client: ClientId,
    target: Option<&TargetSpec>,
) -> Result<SessionId, TargetError> {
    resolve_command_target(state, client, TargetKind::Session, target)?
        .session
        .ok_or_else(|| TargetError::NotFound {
            kind: TargetKind::Session,
            target: target.map_or_else(|| "{current}".to_string(), ToString::to_string),
        })
}

struct CommandEntry {
    name: &'static str,
    alias: Option<&'static str>,
}

const COMMAND_TABLE: &[CommandEntry] = &[
    CommandEntry {
        name: "attach-session",
        alias: Some("attach"),
    },
    CommandEntry {
        name: "copy-mode",
        alias: None,
    },
    CommandEntry {
        name: "detach-client",
        alias: Some("detach"),
    },
    CommandEntry {
        name: "kill-pane",
        alias: Some("killp"),
    },
    CommandEntry {
        name: "kill-server",
        alias: None,
    },
    CommandEntry {
        name: "kill-session",
        alias: None,
    },
    CommandEntry {
        name: "kill-window",
        alias: Some("killw"),
    },
    CommandEntry {
        name: "last-pane",
        alias: Some("lastp"),
    },
    CommandEntry {
        name: "last-window",
        alias: Some("last"),
    },
    CommandEntry {
        name: "list-clients",
        alias: Some("lsc"),
    },
    CommandEntry {
        name: "list-panes",
        alias: Some("lsp"),
    },
    CommandEntry {
        name: "list-sessions",
        alias: Some("ls"),
    },
    CommandEntry {
        name: "list-windows",
        alias: Some("lsw"),
    },
    CommandEntry {
        name: "new-session",
        alias: Some("new"),
    },
    CommandEntry {
        name: "new-window",
        alias: Some("neww"),
    },
    CommandEntry {
        name: "next-window",
        alias: Some("next"),
    },
    CommandEntry {
        name: "previous-window",
        alias: Some("prev"),
    },
    CommandEntry {
        name: "rename-window",
        alias: Some("renamew"),
    },
    CommandEntry {
        name: "resize-pane",
        alias: Some("resizep"),
    },
    CommandEntry {
        name: "rotate-window",
        alias: Some("rotatew"),
    },
    CommandEntry {
        name: "select-pane",
        alias: Some("selectp"),
    },
    CommandEntry {
        name: "select-window",
        alias: Some("selectw"),
    },
    CommandEntry {
        name: "split-window",
        alias: Some("splitw"),
    },
    CommandEntry {
        name: "start-server",
        alias: Some("start"),
    },
    CommandEntry {
        name: "swap-pane",
        alias: Some("swapp"),
    },
];

pub fn resolve_command_name(name: &str) -> Result<&'static str, CommandParseError> {
    if let Some(entry) = COMMAND_TABLE
        .iter()
        .find(|entry| entry.alias == Some(name) || entry.name == name)
    {
        return Ok(entry.name);
    }

    let matches = COMMAND_TABLE
        .iter()
        .filter(|entry| entry.name.starts_with(name))
        .map(|entry| entry.name)
        .collect::<Vec<_>>();

    match matches.as_slice() {
        [name] => Ok(name),
        [] => Err(CommandParseError::new(format!("unknown command: {name}"))),
        _ => Err(CommandParseError::new(format!(
            "ambiguous command: {name}, could be: {}",
            matches.join(", ")
        ))),
    }
}

pub(super) fn parse_single_command(argv: &[String]) -> Result<Command, CommandParseError> {
    let Some(requested_command) = argv.first().map(String::as_str) else {
        return Err(CommandParseError::new("empty command"));
    };
    let command = resolve_command_name(requested_command)?;

    match command {
        "start-server" => {
            validate_arguments(argv, &[], &[], 0)?;
            Ok(Command::StartServer)
        }
        "kill-server" => {
            validate_arguments(argv, &[], &[], 0)?;
            Ok(Command::KillServer)
        }
        "list-clients" => Ok(Command::ListClients),
        "list-sessions" => Ok(Command::ListSessions),
        "list-windows" => Ok(Command::ListWindows {
            target: target_flag_value(argv, "-t")?,
        }),
        "list-panes" => Ok(Command::ListPanes {
            target: target_flag_value(argv, "-t")?,
        }),
        "detach-client" => Ok(Command::DetachClient),
        "copy-mode" => {
            validate_arguments(argv, &["-u"], &[], 0)?;
            Ok(Command::CopyMode)
        }
        "new-session" => {
            let name = flag_value(argv, "-s");
            let group = target_flag_value(argv, "-t")?;
            Ok(Command::NewSession {
                name,
                group,
                attach: !has_flag(argv, "-d"),
                attach_if_exists: has_flag(argv, "-A"),
                cols: 80,
                rows: 24,
            })
        }
        "attach-session" => {
            let target = target_flag_value(argv, "-t")?;
            Ok(Command::AttachSession { target })
        }
        "new-window" => Ok(Command::NewWindow {
            name: flag_value(argv, "-n"),
        }),
        "split-window" => Ok(Command::SplitWindow {
            direction: if has_flag(argv, "-h") {
                SplitDirection::LeftRight
            } else {
                SplitDirection::TopBottom
            },
            detached: has_flag(argv, "-d"),
        }),
        "select-window" => {
            let target = if has_flag(argv, "-n") {
                TargetSpec::parse("+1")
            } else if has_flag(argv, "-p") {
                TargetSpec::parse("-1")
            } else if has_flag(argv, "-l") {
                TargetSpec::parse("{last}")
            } else {
                TargetSpec::parse(
                    flag_value(argv, "-t")
                        .or_else(|| argv.get(1).cloned())
                        .unwrap_or_else(|| "0".to_string()),
                )
            }
            .map_err(|error| CommandParseError::new(error.to_string()))?;
            Ok(Command::SelectWindow { target })
        }
        "next-window" => Ok(Command::SelectWindow {
            target: TargetSpec::parse("+1").expect("literal target is valid"),
        }),
        "previous-window" => Ok(Command::SelectWindow {
            target: TargetSpec::parse("-1").expect("literal target is valid"),
        }),
        "last-window" => Ok(Command::SelectWindow {
            target: TargetSpec::parse("{last}").expect("literal target is valid"),
        }),
        "select-pane" => Ok(Command::SelectPane {
            target: if has_flag(argv, "-L") {
                PaneTarget::Direction(ResizeDirection::Left)
            } else if has_flag(argv, "-R") {
                PaneTarget::Direction(ResizeDirection::Right)
            } else if has_flag(argv, "-U") {
                PaneTarget::Direction(ResizeDirection::Up)
            } else if has_flag(argv, "-D") {
                PaneTarget::Direction(ResizeDirection::Down)
            } else if has_flag(argv, "-l") {
                PaneTarget::Target(TargetSpec::parse("{last}").expect("literal target is valid"))
            } else {
                PaneTarget::Target(
                    target_flag_value(argv, "-t")?.unwrap_or_else(TargetSpec::current),
                )
            },
        }),
        "last-pane" => Ok(Command::SelectPane {
            target: PaneTarget::Target(
                TargetSpec::parse("{last}").expect("literal target is valid"),
            ),
        }),
        "resize-pane" => Ok(Command::ResizePane {
            target: if has_flag(argv, "-Z") {
                ResizeTarget::Zoom
            } else if has_flag(argv, "-L") {
                ResizeTarget::Direction(ResizeDirection::Left)
            } else if has_flag(argv, "-R") {
                ResizeTarget::Direction(ResizeDirection::Right)
            } else if has_flag(argv, "-U") {
                ResizeTarget::Direction(ResizeDirection::Up)
            } else {
                ResizeTarget::Direction(ResizeDirection::Down)
            },
            amount: resize_amount(argv),
        }),
        "rename-window" => Ok(Command::RenameWindow {
            name: flag_value(argv, "-n")
                .or_else(|| argv.get(1).cloned())
                .unwrap_or_else(|| "window".to_string()),
        }),
        "rotate-window" => Ok(Command::RotateWindow {
            reverse: has_flag(argv, "-D"),
        }),
        "swap-pane" => Ok(Command::SwapPane {
            direction: if has_flag(argv, "-L") {
                ResizeDirection::Left
            } else if has_flag(argv, "-R") {
                ResizeDirection::Right
            } else if has_flag(argv, "-U") {
                ResizeDirection::Up
            } else {
                ResizeDirection::Down
            },
        }),
        "kill-pane" => {
            validate_arguments(argv, &[], &[], 0)?;
            Ok(Command::KillPane)
        }
        "kill-window" => {
            validate_arguments(argv, &[], &[], 0)?;
            Ok(Command::KillWindow)
        }
        "kill-session" => {
            validate_arguments(argv, &[], &["-t"], 0)?;
            Ok(Command::KillSession {
                target: target_flag_value(argv, "-t")?,
            })
        }
        _ => unreachable!("resolved command is missing a parser: {command}"),
    }
}

fn flag_value(argv: &[String], flag: &str) -> Option<String> {
    argv.windows(2)
        .find_map(|pair| (pair[0] == flag).then(|| pair[1].clone()))
}

fn target_flag_value(argv: &[String], flag: &str) -> Result<Option<TargetSpec>, CommandParseError> {
    flag_value(argv, flag)
        .map(TargetSpec::parse)
        .transpose()
        .map_err(|error| CommandParseError::new(error.to_string()))
}

fn has_flag(argv: &[String], flag: &str) -> bool {
    argv.iter().any(|value| value == flag)
}

fn validate_arguments(
    argv: &[String],
    flags: &[&str],
    value_flags: &[&str],
    max_positionals: usize,
) -> Result<(), CommandParseError> {
    let mut index = 1;
    let mut positionals = 0;

    while index < argv.len() {
        let argument = argv[index].as_str();
        if value_flags.contains(&argument) {
            if index + 1 >= argv.len() {
                return Err(CommandParseError::new(format!(
                    "missing argument for {argument}"
                )));
            }
            index += 2;
        } else if flags.contains(&argument) {
            index += 1;
        } else if argument.starts_with('-') {
            return Err(CommandParseError::new(format!(
                "unknown option: {argument}"
            )));
        } else {
            positionals += 1;
            if positionals > max_positionals {
                return Err(CommandParseError::new(format!(
                    "too many arguments for {}",
                    argv[0]
                )));
            }
            index += 1;
        }
    }

    Ok(())
}

fn resize_amount(argv: &[String]) -> u16 {
    for flag in ["-L", "-R", "-U", "-D"] {
        if let Some(value) = flag_value(argv, flag) {
            if let Ok(amount) = value.parse() {
                return amount;
            }
        }
    }
    argv.get(1)
        .and_then(|value| value.parse().ok())
        .unwrap_or(1)
}

#[cfg(test)]
mod tests {
    use super::{
        execute, parse_command, parse_command_text, Command, CommandEffect, CommandQueue,
        CommandSource,
    };
    use crate::{
        build_window_scene, render_full_scene, RenderState, ServerState, SplitDirection,
        TargetResolver, TargetSpec,
    };

    fn target(raw: &str) -> TargetSpec {
        TargetSpec::parse(raw).unwrap()
    }

    fn list(raw: &str) -> super::CommandList {
        parse_command_text(raw).unwrap()
    }

    fn ensured_pane(outcome: &super::CommandOutcome) -> Option<crate::PaneId> {
        outcome.effects.iter().find_map(|effect| match effect {
            CommandEffect::EnsurePane { pane } => Some(*pane),
            _ => None,
        })
    }

    #[test]
    fn command_lists_are_round_robin_across_clients_and_ordered_within_each_client() {
        let mut queue = CommandQueue::default();
        let a = crate::ClientId::new(1);
        let b = crate::ClientId::new(2);
        queue
            .push_list(
                a,
                list("new-window -n a1; new-window -n a2"),
                CommandSource::ClientRequest,
            )
            .unwrap();
        queue
            .push_list(
                b,
                list("new-window -n b1; new-window -n b2"),
                CommandSource::ClientRequest,
            )
            .unwrap();

        let mut names = Vec::new();
        while let Some(command) = queue.pop() {
            let Command::NewWindow { name } = &command.command else {
                panic!("expected new-window");
            };
            names.push(name.clone().unwrap());
            let _ = queue.finish(command, Ok(String::new()));
        }

        assert_eq!(names, ["a1", "b1", "a2", "b2"]);
    }

    #[test]
    fn one_failed_list_does_not_remove_another_clients_work() {
        let mut queue = CommandQueue::default();
        let a = crate::ClientId::new(1);
        let b = crate::ClientId::new(2);
        queue
            .push_list(
                a,
                list("new-window -n a1; new-window -n a2"),
                CommandSource::ClientRequest,
            )
            .unwrap();
        queue
            .push_list(b, list("new-window -n b1"), CommandSource::KeyBinding)
            .unwrap();

        let failed = queue.pop().unwrap();
        let completion = queue.finish(failed, Err("no target".to_string())).unwrap();

        assert_eq!(completion.result, Err("no target".to_string()));
        assert_eq!(queue.pop().unwrap().client, b);
        assert!(queue.remove_client(a).is_empty());
    }

    #[test]
    fn queue_sequences_are_monotonic_and_disconnect_is_isolated() {
        let mut queue = CommandQueue::default();
        let a = crate::ClientId::new(1);
        let b = crate::ClientId::new(2);
        let a_invocation = queue
            .push_list(
                a,
                list("list-sessions; list-clients"),
                CommandSource::ClientRequest,
            )
            .unwrap();
        let b_invocation = queue
            .push_list(b, list("list-windows"), CommandSource::KeyBinding)
            .unwrap();
        let first = queue.pop().unwrap();
        let _ = queue.finish(first.clone(), Ok("sessions".to_string()));
        let second = queue.pop().unwrap();

        assert!(first.sequence < second.sequence);
        assert_eq!(queue.remove_client(a), vec![a_invocation]);
        let completion = queue.finish(second, Ok(String::new())).unwrap();
        assert_eq!(completion.invocation, b_invocation);
        assert!(!completion.requires_reply());
        assert!(queue.is_empty());
    }

    #[test]
    fn empty_lists_are_rejected_and_active_disconnects_are_cancelled() {
        let mut queue = CommandQueue::default();
        let client = crate::ClientId::new(1);
        assert!(queue
            .push_list(
                client,
                super::CommandList::new(Vec::new()).unwrap(),
                CommandSource::ClientRequest,
            )
            .is_err());

        let invocation = queue
            .push_list(client, list("list-sessions"), CommandSource::ClientRequest)
            .unwrap();
        let active = queue.pop().unwrap();
        assert_eq!(queue.remove_client(client), vec![invocation]);
        assert!(queue.finish(active, Ok(String::new())).is_none());
        assert!(queue.is_empty());
    }

    #[test]
    fn command_queue_executes_deterministically() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let mut queue = CommandQueue::default();

        queue
            .push_list(
                client,
                list("new-session -s a; start-server"),
                CommandSource::ClientRequest,
            )
            .unwrap();

        let first = queue.pop().unwrap();
        assert_eq!(first.sequence, 1);
        let result = execute(&mut state, &first);
        assert!(result.ok);
        assert!(ensured_pane(&result).is_some());
        assert!(queue.finish(first, Ok(result.message)).is_none());
        assert_eq!(queue.pop().unwrap().sequence, 2);
    }

    #[test]
    fn new_session_with_existing_name_errors() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let first = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("test".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        let second = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 2,
                sequence: 2,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("test".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );

        assert!(first.ok);
        assert!(!second.ok);
        assert_eq!(second.message, "duplicate session: test");
        assert_eq!(state.sessions.len(), 1);
        assert!(second.effects.is_empty());
    }

    #[test]
    fn new_session_attach_if_exists_reuses_existing_session() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let first = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("test".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        let first_pane = state.clients[&client].attached_pane;
        let second = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 2,
                sequence: 2,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("test".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: true,
                    cols: 80,
                    rows: 24,
                },
            },
        );

        assert!(first.ok);
        assert!(second.ok);
        assert_eq!(state.sessions.len(), 1);
        assert!(ensured_pane(&first).is_some());
        assert_eq!(first_pane, state.clients[&client].attached_pane);
    }

    #[test]
    fn detached_new_session_returns_created_pane_for_server_spawn() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let result = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("detached".to_string()),
                    group: None,
                    attach: false,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );

        assert!(result.ok);
        assert!(ensured_pane(&result).is_some());
        assert!(state
            .clients
            .get(&client)
            .unwrap()
            .attached_session
            .is_none());
    }

    #[test]
    fn commands_create_and_list_windows_and_panes() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let new_session = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("work".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        assert!(new_session.ok);

        let new_window = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 2,
                sequence: 2,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewWindow {
                    name: Some("edit".to_string()),
                },
            },
        );
        assert!(new_window.ok);

        let split = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 3,
                sequence: 3,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::SplitWindow {
                    direction: SplitDirection::TopBottom,
                    detached: false,
                },
            },
        );
        assert!(split.ok);

        let windows = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 4,
                sequence: 4,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::ListWindows { target: None },
            },
        );
        assert!(windows.message.contains("*1: edit panes=2"));

        let panes = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 5,
                sequence: 5,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::ListPanes { target: None },
            },
        );
        assert!(panes.message.lines().count() == 2);
    }

    #[test]
    fn split_window_builds_layout_scene_with_border() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 20, 6);
        state.attach_client(client, created.session);
        let second = state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                20,
                6,
            )
            .unwrap();
        {
            let pane = state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, b"left");
        }
        {
            let pane = state.pane_mut(second).unwrap();
            pane.terminal.feed(&mut pane.screen, b"right");
        }

        let scene = build_window_scene(&state, created.session, 20, 6).unwrap();
        let mut render_state = RenderState::new(20, 6);
        let output = String::from_utf8(render_full_scene(&scene, &mut render_state)).unwrap();

        assert!(output.contains("left"));
        assert!(output.contains("right"));
        assert!(output.contains("|"));
    }

    #[test]
    fn grouped_sessions_share_windows_but_keep_current_window_independent() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let first = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("G1".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        assert!(first.ok);
        let second = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 2,
                sequence: 2,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::NewSession {
                    name: Some("G2".to_string()),
                    group: Some(target("G1")),
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        assert!(second.ok);
        let g1 = TargetResolver::new(&state)
            .find_exact_session("G1")
            .unwrap();
        let g2 = TargetResolver::new(&state)
            .find_exact_session("G2")
            .unwrap();
        assert_eq!(state.session_groups.len(), 1);
        assert_eq!(
            state.active_window_for_session(g1),
            state.active_window_for_session(g2)
        );

        let created = state
            .create_window(g1, Some("shared".to_string()), 80, 24)
            .unwrap();
        assert_eq!(state.sessions.get(&g1).unwrap().winlinks.len(), 2);
        assert_eq!(state.sessions.get(&g2).unwrap().winlinks.len(), 2);
        assert_eq!(state.active_window_for_session(g1), Some(created.window));
        assert_ne!(state.active_window_for_session(g2), Some(created.window));
        assert_eq!(state.select_window(g2, 1), Some(created.pane));
        assert_eq!(state.active_window_for_session(g2), Some(created.window));
    }

    #[test]
    fn parses_tmux_style_surface_commands() {
        assert_eq!(
            parse_command(&["new".to_string(), "-s".to_string(), "test".to_string()]).unwrap(),
            Command::NewSession {
                name: Some("test".to_string()),
                group: None,
                attach: true,
                attach_if_exists: false,
                cols: 80,
                rows: 24,
            }
        );
        assert_eq!(
            parse_command(&[
                "new-session".to_string(),
                "-A".to_string(),
                "-d".to_string(),
                "-s".to_string(),
                "test".to_string(),
                "-t".to_string(),
                "base".to_string()
            ])
            .unwrap(),
            Command::NewSession {
                name: Some("test".to_string()),
                group: Some(target("base")),
                attach: false,
                attach_if_exists: true,
                cols: 80,
                rows: 24,
            }
        );
        assert_eq!(
            parse_command(&[
                "attach-session".to_string(),
                "-t".to_string(),
                "test".to_string()
            ])
            .unwrap(),
            Command::AttachSession {
                target: Some(target("test"))
            }
        );
        assert_eq!(
            parse_command(&[
                "new-window".to_string(),
                "-n".to_string(),
                "edit".to_string()
            ])
            .unwrap(),
            Command::NewWindow {
                name: Some("edit".to_string())
            }
        );
        assert_eq!(
            parse_command(&["lsw".to_string(), "-t".to_string(), "test".to_string()]).unwrap(),
            Command::ListWindows {
                target: Some(target("test"))
            }
        );
        assert_eq!(
            parse_command(&["lsp".to_string(), "-t".to_string(), "test".to_string()]).unwrap(),
            Command::ListPanes {
                target: Some(target("test"))
            }
        );
        assert_eq!(
            parse_command(&["lsc".to_string()]).unwrap(),
            Command::ListClients
        );
        assert_eq!(
            parse_command(&["neww".to_string(), "-n".to_string(), "edit".to_string()]).unwrap(),
            Command::NewWindow {
                name: Some("edit".to_string())
            }
        );
        assert_eq!(
            parse_command(&["start".to_string()]).unwrap(),
            Command::StartServer
        );
        assert_eq!(
            parse_command(&["kill-server".to_string()]).unwrap(),
            Command::KillServer
        );
    }

    #[test]
    fn window_and_pane_selectors_parse_once_as_target_specs() {
        assert_eq!(
            parse_command(&["next-window".to_string()]).unwrap(),
            Command::SelectWindow {
                target: target("+1")
            }
        );
        assert_eq!(
            parse_command(&["previous-window".to_string()]).unwrap(),
            Command::SelectWindow {
                target: target("-1")
            }
        );
        assert_eq!(
            parse_command(&["last-window".to_string()]).unwrap(),
            Command::SelectWindow {
                target: target("{last}")
            }
        );
        assert_eq!(
            parse_command(&[
                "select-window".to_string(),
                "-t".to_string(),
                "work:logs".to_string(),
            ])
            .unwrap(),
            Command::SelectWindow {
                target: target("work:logs")
            }
        );
        assert_eq!(
            parse_command(&[
                "select-pane".to_string(),
                "-t".to_string(),
                "work:0.1".to_string(),
            ])
            .unwrap(),
            Command::SelectPane {
                target: super::PaneTarget::Target(target("work:0.1"))
            }
        );
        assert_eq!(
            parse_command(&["last-pane".to_string()]).unwrap(),
            Command::SelectPane {
                target: super::PaneTarget::Target(target("{last}"))
            }
        );
    }

    #[test]
    fn resolves_tmux_aliases_and_unique_command_prefixes() {
        assert_eq!(
            super::resolve_command_name("attach").unwrap(),
            "attach-session"
        );
        assert_eq!(super::resolve_command_name("a").unwrap(), "attach-session");
        assert_eq!(super::resolve_command_name("new").unwrap(), "new-session");
        assert_eq!(
            super::resolve_command_name("list-s").unwrap(),
            "list-sessions"
        );
    }

    #[test]
    fn reports_tmux_style_ambiguous_and_unknown_commands() {
        assert_eq!(
            super::resolve_command_name("kill-s").unwrap_err().message,
            "ambiguous command: kill-s, could be: kill-server, kill-session"
        );
        assert_eq!(
            super::resolve_command_name("status").unwrap_err().message,
            "unknown command: status"
        );
    }

    #[test]
    fn destructive_commands_reject_ignored_arguments() {
        assert_eq!(
            parse_command(&[
                "kill-session".to_string(),
                "-s".to_string(),
                "test".to_string()
            ])
            .unwrap_err()
            .message,
            "unknown option: -s"
        );
        assert_eq!(
            parse_command(&["kill-session".to_string(), "test".to_string()])
                .unwrap_err()
                .message,
            "too many arguments for kill-session"
        );
        assert_eq!(
            parse_command(&["kill-session".to_string(), "-t".to_string()])
                .unwrap_err()
                .message,
            "missing argument for -t"
        );
    }

    #[test]
    fn successful_kill_session_is_silent_like_tmux() {
        let mut state = ServerState::new();
        let client = state.add_client();
        state.create_session("test".to_string(), 80, 24);

        let result = execute(
            &mut state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                source: CommandSource::ClientRequest,
                final_in_list: true,
                command: Command::KillSession {
                    target: Some(target("test")),
                },
            },
        );

        assert!(result.ok);
        assert!(result.message.is_empty());
        assert!(TargetResolver::new(&state)
            .find_exact_session("test")
            .is_none());
    }
}
