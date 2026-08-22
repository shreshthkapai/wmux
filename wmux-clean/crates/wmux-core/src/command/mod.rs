use smallvec::SmallVec;
use std::{
    collections::{BTreeMap, BTreeSet, VecDeque},
    path::PathBuf,
    sync::Arc,
};

use crate::{
    ClientId, FormatContext, FormatEngine, KeyBinding, KeyCode, KeyTableTarget, OptionScope,
    OptionTarget, PaneId, ResizeDirection, ResolveContext, ResolvedTarget, ServerState, SessionId,
    SplitDirection, TargetError, TargetKind, TargetResolver, TargetSpec,
};

mod execute;
mod lexer;
mod parser;

pub use execute::execute;
pub use lexer::{CommandParseError, SourcePosition, SourceSpan};
pub use parser::{
    parse_command, parse_command_argv, parse_command_text, quote_argument, MAX_COMMANDS,
};

const MAX_CONFIRM_PROMPT_BYTES: usize = 4 * 1024;
const MAX_SEND_BYTES: usize = 1024 * 1024;
const MAX_SEND_REPEAT: u16 = 10_000;
pub const MAX_SOURCE_DEPTH: u8 = 16;

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

    pub fn with_source_context(
        &self,
        depth: u8,
        ancestors: Arc<[PathBuf]>,
    ) -> Result<Self, CommandParseError> {
        if depth > MAX_SOURCE_DEPTH {
            return Err(CommandParseError::new(
                "source-file depth exceeds 16 levels",
            ));
        }
        Self::new(
            self.0
                .iter()
                .cloned()
                .map(|command| match command {
                    Command::SourceFile { path, .. } => Command::SourceFile {
                        path,
                        depth,
                        ancestors: Arc::clone(&ancestors),
                    },
                    command => command,
                })
                .collect(),
        )
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
    BindKey {
        table: KeyTableTarget,
        key: KeyCode,
        repeatable: bool,
        commands: CommandList,
    },
    UnbindKey {
        table: KeyTableTarget,
        key: Option<KeyCode>,
        all: bool,
    },
    ListKeys {
        table: Option<KeyTableTarget>,
    },
    SendKeys {
        target: Option<TargetSpec>,
        keys: Vec<SendKey>,
        literal: bool,
        repeat: u16,
    },
    SendPrefix {
        target: Option<TargetSpec>,
    },
    SwitchClient {
        target: SessionSelector,
    },
    RefreshClient,
    ConfirmBefore {
        prompt: String,
        commands: CommandList,
    },
    SetOption {
        scope: OptionScope,
        target: Option<TargetSpec>,
        unset: bool,
        name: String,
        value: Option<String>,
    },
    ShowOptions {
        scope: OptionScope,
        target: Option<TargetSpec>,
        local_only: bool,
        name: Option<String>,
        value_only: bool,
    },
    DisplayMessage {
        target: Option<TargetSpec>,
        template: String,
    },
    SourceFile {
        path: PathBuf,
        depth: u8,
        ancestors: Arc<[PathBuf]>,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SendKey {
    Key(KeyCode),
    Literal(String),
}

impl SendKey {
    fn append_bytes(&self, output: &mut Vec<u8>) {
        match self {
            Self::Key(key) => key.append_terminal_bytes(output),
            Self::Literal(text) => output.extend_from_slice(text.as_bytes()),
        }
    }

    fn encoded_len(&self) -> usize {
        match self {
            Self::Key(key) => {
                let mut bytes = Vec::with_capacity(16);
                key.append_terminal_bytes(&mut bytes);
                bytes.len()
            }
            Self::Literal(text) => text.len(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum SessionSelector {
    Target(TargetSpec),
    Next,
    Previous,
    Last,
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
    Config,
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
    SourceFile {
        path: PathBuf,
        depth: u8,
        ancestors: Arc<[PathBuf]>,
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
    inserted: VecDeque<Command>,
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
                inserted: VecDeque::new(),
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
            let command = if let Some(command) = invocation.inserted.pop_front() {
                command
            } else {
                let command = invocation.commands[invocation.next].clone();
                invocation.next += 1;
                command
            };
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
                final_in_list: invocation.inserted.is_empty()
                    && invocation.next == invocation.commands.len(),
            };
            self.active.insert(client, invocation.id);
            return Some(queued);
        }
        None
    }

    pub fn insert_after_active(
        &mut self,
        active: &QueuedCommand,
        commands: CommandList,
    ) -> Result<(), CommandParseError> {
        if self.active.get(&active.client) != Some(&active.invocation) {
            return Err(CommandParseError::new("command invocation is not active"));
        }
        let invocation = self
            .pending
            .get_mut(&active.client)
            .and_then(|invocations| invocations.front_mut())
            .filter(|invocation| invocation.id == active.invocation)
            .ok_or_else(|| CommandParseError::new("active command invocation is missing"))?;
        let remaining = invocation
            .commands
            .len()
            .saturating_sub(invocation.next)
            .saturating_add(invocation.inserted.len());
        if remaining
            .checked_add(commands.len())
            .is_none_or(|count| count > MAX_COMMANDS)
        {
            return Err(CommandParseError::new("command list exceeds 256 commands"));
        }
        for command in commands.iter().rev() {
            invocation.inserted.push_front(command.clone());
        }
        Ok(())
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
                    if invocation.inserted.is_empty()
                        && invocation.next == invocation.commands.len()
                    {
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
        Command::SendKeys { target, .. } | Command::SendPrefix { target } => {
            let resolved = match resolve_command_target(
                state,
                queued.client,
                TargetKind::Pane,
                target.as_ref(),
            ) {
                Ok(resolved) => resolved,
                Err(error) => return error_result(queued.sequence, error),
            };
            let Some(pane) = resolved.pane else {
                return error_result(queued.sequence, "no matching pane");
            };
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: Some(pane),
            }
        }
        Command::SwitchClient { target } => {
            let session = match target {
                SessionSelector::Target(target) => {
                    resolve_session_target(state, queued.client, Some(&target))
                }
                SessionSelector::Last => {
                    let target = TargetSpec::parse("{last}").expect("literal target is valid");
                    resolve_session_target(state, queued.client, Some(&target))
                }
                SessionSelector::Next => adjacent_session(state, queued.client, false),
                SessionSelector::Previous => adjacent_session(state, queued.client, true),
            };
            match session {
                Ok(session) => attach_result(state, queued.sequence, queued.client, session),
                Err(error) => error_result(queued.sequence, error),
            }
        }
        Command::RefreshClient | Command::ConfirmBefore { .. } => {
            if !state.clients.contains_key(&queued.client) {
                return error_result(queued.sequence, "no matching client");
            }
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: None,
            }
        }
        Command::BindKey {
            table,
            key,
            repeatable,
            commands,
        } => {
            let table = match state.key_tables.ensure_named(table.as_str()) {
                Ok(table) => table,
                Err(error) => return error_result(queued.sequence, error),
            };
            state
                .key_tables
                .table_mut(table)
                .expect("ensured key table exists")
                .bind(KeyBinding {
                    key,
                    repeatable,
                    commands,
                });
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: None,
            }
        }
        Command::UnbindKey { table, key, all } => {
            let Some(table) = state.key_tables.named(table.as_str()) else {
                return error_result(
                    queued.sequence,
                    format!("unknown key table: {}", table.as_str()),
                );
            };
            let table = state
                .key_tables
                .table_mut(table)
                .expect("named key table exists");
            if all {
                table.clear();
            } else if let Some(key) = key {
                table.unbind(key);
            }
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: String::new(),
                attached_pane: None,
            }
        }
        Command::ListKeys { table } => match list_key_bindings(state, table.as_ref()) {
            Ok(message) => CommandResult {
                sequence: queued.sequence,
                ok: true,
                message,
                attached_pane: None,
            },
            Err(error) => error_result(queued.sequence, error),
        },
        Command::SetOption {
            scope,
            target,
            unset,
            name,
            value,
        } => {
            let target = match resolve_option_target(state, queued.client, scope, target.as_ref()) {
                Ok(target) => target,
                Err(error) => return error_result(queued.sequence, error),
            };
            let result = if unset {
                state.options.unset(target, &name).map(|_| ())
            } else {
                state
                    .options
                    .set(
                        target,
                        &name,
                        value.as_deref().expect("parser requires a value"),
                    )
                    .map(|_| ())
            };
            match result {
                Ok(()) => CommandResult {
                    sequence: queued.sequence,
                    ok: true,
                    message: String::new(),
                    attached_pane: None,
                },
                Err(error) => error_result(queued.sequence, error),
            }
        }
        Command::ShowOptions {
            scope,
            target,
            local_only,
            name,
            value_only,
        } => {
            let target = match resolve_option_target(state, queued.client, scope, target.as_ref()) {
                Ok(target) => target,
                Err(error) => return error_result(queued.sequence, error),
            };
            let path = state.option_path(target);
            let values = if let Some(name) = name {
                let value = if local_only {
                    state
                        .options
                        .list_local(target)
                        .into_iter()
                        .find(|(candidate, _)| candidate == &name)
                        .map(|(_, value)| value)
                        .ok_or_else(|| format!("option not set: {name}"))
                } else {
                    state
                        .options
                        .get(&path, &name)
                        .map_err(|error| error.to_string())
                };
                match values_to_message(value.map(|value| vec![(name, value)]), value_only) {
                    Ok(message) => message,
                    Err(error) => return error_result(queued.sequence, error),
                }
            } else {
                let values = if local_only {
                    state.options.list_local(target)
                } else {
                    state.options.list_effective(&path)
                };
                match values_to_message(Ok(values), false) {
                    Ok(message) => message,
                    Err(error) => return error_result(queued.sequence, error),
                }
            };
            CommandResult {
                sequence: queued.sequence,
                ok: true,
                message: values,
                attached_pane: None,
            }
        }
        Command::DisplayMessage { target, template } => {
            let resolved = match resolve_command_target(
                state,
                queued.client,
                TargetKind::Pane,
                target.as_ref(),
            ) {
                Ok(resolved) => resolved,
                Err(error) => return error_result(queued.sequence, error),
            };
            let context = FormatContext {
                client: Some(resolved.client),
                session: resolved.session,
                window: resolved.window,
                pane: resolved.pane,
            };
            match FormatEngine::expand(state, context, &template) {
                Ok(message) => CommandResult {
                    sequence: queued.sequence,
                    ok: true,
                    message,
                    attached_pane: None,
                },
                Err(error) => error_result(queued.sequence, error),
            }
        }
        Command::SourceFile { .. } => CommandResult {
            sequence: queued.sequence,
            ok: true,
            message: String::new(),
            attached_pane: None,
        },
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

fn adjacent_session(
    state: &ServerState,
    client: ClientId,
    reverse: bool,
) -> Result<SessionId, TargetError> {
    let context = ResolveContext::for_client(state, client)?;
    let current = context
        .current_session
        .ok_or_else(|| TargetError::NotFound {
            kind: TargetKind::Session,
            target: "{current}".to_string(),
        })?;
    let sessions = state.sessions.keys().copied().collect::<Vec<_>>();
    let position = sessions
        .iter()
        .position(|session| *session == current)
        .ok_or_else(|| TargetError::NotFound {
            kind: TargetKind::Session,
            target: current.raw().to_string(),
        })?;
    let next = if reverse {
        position.checked_sub(1).unwrap_or(sessions.len() - 1)
    } else {
        (position + 1) % sessions.len()
    };
    Ok(sessions[next])
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

fn list_key_bindings(
    state: &ServerState,
    selected: Option<&KeyTableTarget>,
) -> Result<String, String> {
    let selected = selected
        .map(|table| {
            state
                .key_tables
                .named(table.as_str())
                .ok_or_else(|| format!("unknown key table: {}", table.as_str()))
        })
        .transpose()?;
    let mut lines = Vec::new();
    for table in state.key_tables.tables() {
        if selected.is_some_and(|selected| selected != table.name()) {
            continue;
        }
        let name = state
            .key_tables
            .name(table.name())
            .expect("every key table has a name");
        for binding in table.bindings() {
            let nested = format_command_list(&binding.commands);
            let nested = if binding.commands.len() == 1 {
                nested
            } else {
                parser::quote_argument(&nested)
            };
            lines.push(format!(
                "bind-key{} -T {} {} {nested}",
                if binding.repeatable { " -r" } else { "" },
                parser::quote_argument(name),
                parser::quote_argument(&binding.key.to_string()),
            ));
        }
    }
    Ok(lines.join("\n"))
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

fn resolve_option_target(
    state: &ServerState,
    client: ClientId,
    scope: OptionScope,
    target: Option<&TargetSpec>,
) -> Result<OptionTarget, String> {
    if scope == OptionScope::Server {
        if target.is_some() {
            return Err("server options do not accept a target".to_string());
        }
        return Ok(OptionTarget::Server);
    }
    let kind = match scope {
        OptionScope::Server => unreachable!(),
        OptionScope::Session => TargetKind::Session,
        OptionScope::Window => TargetKind::Window,
        OptionScope::Pane => TargetKind::Pane,
        OptionScope::Client => TargetKind::Client,
    };
    let resolved =
        resolve_command_target(state, client, kind, target).map_err(|error| error.to_string())?;
    match scope {
        OptionScope::Server => unreachable!(),
        OptionScope::Session => resolved.session.map(OptionTarget::Session),
        OptionScope::Window => resolved.window.map(OptionTarget::Window),
        OptionScope::Pane => resolved.pane.map(OptionTarget::Pane),
        OptionScope::Client => Some(OptionTarget::Client(resolved.client)),
    }
    .ok_or_else(|| format!("no matching {scope}"))
}

fn values_to_message(
    values: Result<Vec<(String, crate::OptionValue)>, String>,
    value_only: bool,
) -> Result<String, String> {
    values.map(|values| {
        values
            .into_iter()
            .map(|(name, value)| {
                if value_only {
                    value.to_string()
                } else {
                    format!("{name} {value}")
                }
            })
            .collect::<Vec<_>>()
            .join("\n")
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
        name: "bind-key",
        alias: Some("bind"),
    },
    CommandEntry {
        name: "copy-mode",
        alias: None,
    },
    CommandEntry {
        name: "confirm-before",
        alias: Some("confirm"),
    },
    CommandEntry {
        name: "detach-client",
        alias: Some("detach"),
    },
    CommandEntry {
        name: "display-message",
        alias: Some("display"),
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
        name: "list-keys",
        alias: Some("lsk"),
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
        name: "refresh-client",
        alias: Some("refresh"),
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
        name: "set-option",
        alias: Some("set"),
    },
    CommandEntry {
        name: "set-window-option",
        alias: Some("setw"),
    },
    CommandEntry {
        name: "send-keys",
        alias: Some("send"),
    },
    CommandEntry {
        name: "send-prefix",
        alias: None,
    },
    CommandEntry {
        name: "split-window",
        alias: Some("splitw"),
    },
    CommandEntry {
        name: "source-file",
        alias: Some("source"),
    },
    CommandEntry {
        name: "start-server",
        alias: Some("start"),
    },
    CommandEntry {
        name: "show-options",
        alias: Some("show"),
    },
    CommandEntry {
        name: "show-window-options",
        alias: Some("showw"),
    },
    CommandEntry {
        name: "swap-pane",
        alias: Some("swapp"),
    },
    CommandEntry {
        name: "switch-client",
        alias: Some("switchc"),
    },
    CommandEntry {
        name: "unbind-key",
        alias: Some("unbind"),
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

pub(super) fn parse_single_command(
    argv: &[String],
    depth: usize,
) -> Result<Command, CommandParseError> {
    let Some(requested_command) = argv.first().map(String::as_str) else {
        return Err(CommandParseError::new("empty command"));
    };
    let command = resolve_command_name(requested_command)?;

    match command {
        "bind-key" => parse_bind_key(argv, depth),
        "unbind-key" => parse_unbind_key(argv),
        "list-keys" => parse_list_keys(argv),
        "send-keys" => parse_send_keys(argv),
        "send-prefix" => parse_send_prefix(argv),
        "switch-client" => parse_switch_client(argv),
        "refresh-client" => {
            validate_arguments(argv, &[], &[], 0)?;
            Ok(Command::RefreshClient)
        }
        "confirm-before" => parse_confirm_before(argv, depth),
        "display-message" => parse_display_message(argv),
        "source-file" => parse_source_file(argv),
        "set-option" => parse_set_option(argv, OptionScope::Session),
        "set-window-option" => parse_set_option(argv, OptionScope::Window),
        "show-options" => parse_show_options(argv, OptionScope::Session),
        "show-window-options" => parse_show_options(argv, OptionScope::Window),
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

fn parse_source_file(argv: &[String]) -> Result<Command, CommandParseError> {
    match argv {
        [_, path] => Ok(Command::SourceFile {
            path: PathBuf::from(path),
            depth: 0,
            ancestors: Arc::from([]),
        }),
        [_] => Err(CommandParseError::new("source-file requires a path")),
        _ => Err(CommandParseError::new("too many arguments for source-file")),
    }
}

fn parse_display_message(argv: &[String]) -> Result<Command, CommandParseError> {
    let mut target = None;
    let mut positionals = Vec::new();
    let mut index = 1;
    while let Some(argument) = argv.get(index) {
        match argument.as_str() {
            "-p" => {}
            "-t" => {
                let raw = argv
                    .get(index + 1)
                    .ok_or_else(|| CommandParseError::new("missing argument for -t"))?;
                target = Some(
                    TargetSpec::parse(raw.clone())
                        .map_err(|error| CommandParseError::new(error.to_string()))?,
                );
                index += 1;
            }
            "--" => {
                positionals.extend_from_slice(&argv[index + 1..]);
                break;
            }
            option if option.starts_with('-') => {
                return Err(CommandParseError::new(format!("unknown option: {option}")));
            }
            _ => positionals.push(argument.clone()),
        }
        index += 1;
    }
    if positionals.len() != 1 {
        return Err(CommandParseError::new(
            "display-message requires exactly one format",
        ));
    }
    Ok(Command::DisplayMessage {
        target,
        template: positionals.remove(0),
    })
}

fn parse_set_option(
    argv: &[String],
    default_scope: OptionScope,
) -> Result<Command, CommandParseError> {
    let parsed = parse_option_arguments(argv, default_scope, false)?;
    let name = parsed
        .positionals
        .first()
        .cloned()
        .ok_or_else(|| CommandParseError::new("set-option requires an option name"))?;
    let value = parsed.positionals.get(1).cloned();
    if parsed.positionals.len() > 2 {
        return Err(CommandParseError::new("too many arguments for set-option"));
    }
    if !parsed.unset && value.is_none() {
        return Err(CommandParseError::new(
            "set-option requires a value unless -u is used",
        ));
    }
    if parsed.unset && value.is_some() {
        return Err(CommandParseError::new(
            "set-option -u does not accept a value",
        ));
    }
    Ok(Command::SetOption {
        scope: parsed.scope,
        target: parsed.target,
        unset: parsed.unset,
        name,
        value,
    })
}

fn parse_show_options(
    argv: &[String],
    default_scope: OptionScope,
) -> Result<Command, CommandParseError> {
    let parsed = parse_option_arguments(argv, default_scope, true)?;
    if parsed.positionals.len() > 1 {
        return Err(CommandParseError::new(
            "too many arguments for show-options",
        ));
    }
    let name = parsed.positionals.first().cloned();
    if parsed.value_only && name.is_none() {
        return Err(CommandParseError::new(
            "show-options -v requires an option name",
        ));
    }
    Ok(Command::ShowOptions {
        scope: parsed.scope,
        target: parsed.target,
        local_only: parsed.local_only,
        name,
        value_only: parsed.value_only,
    })
}

struct ParsedOptionArguments {
    scope: OptionScope,
    target: Option<TargetSpec>,
    unset: bool,
    local_only: bool,
    value_only: bool,
    positionals: Vec<String>,
}

fn parse_option_arguments(
    argv: &[String],
    default_scope: OptionScope,
    showing: bool,
) -> Result<ParsedOptionArguments, CommandParseError> {
    let mut scope = None;
    let mut target = None;
    let mut unset = false;
    let mut local_only = false;
    let mut value_only = false;
    let mut positionals = Vec::new();
    let mut index = 1;
    while let Some(argument) = argv.get(index) {
        let selected_scope = match argument.as_str() {
            "-g" => Some(OptionScope::Server),
            "-s" => Some(OptionScope::Session),
            "-w" => Some(OptionScope::Window),
            "-p" => Some(OptionScope::Pane),
            "-c" => Some(OptionScope::Client),
            _ => None,
        };
        if let Some(selected) = selected_scope {
            if scope.replace(selected).is_some() {
                return Err(CommandParseError::new(
                    "option scope flags are mutually exclusive",
                ));
            }
        } else {
            match argument.as_str() {
                "-t" => {
                    let raw = argv
                        .get(index + 1)
                        .ok_or_else(|| CommandParseError::new("missing argument for -t"))?;
                    target = Some(
                        TargetSpec::parse(raw.clone())
                            .map_err(|error| CommandParseError::new(error.to_string()))?,
                    );
                    index += 1;
                }
                "-u" if !showing => unset = true,
                "-A" if showing => local_only = false,
                "-L" if showing => local_only = true,
                "-v" if showing => value_only = true,
                "--" => {
                    positionals.extend_from_slice(&argv[index + 1..]);
                    break;
                }
                option if option.starts_with('-') => {
                    return Err(CommandParseError::new(format!("unknown option: {option}")));
                }
                _ => positionals.push(argument.clone()),
            }
        }
        index += 1;
    }
    let scope = scope.unwrap_or(default_scope);
    if scope == OptionScope::Server && target.is_some() {
        return Err(CommandParseError::new("server options do not accept -t"));
    }
    Ok(ParsedOptionArguments {
        scope,
        target,
        unset,
        local_only,
        value_only,
        positionals,
    })
}

fn parse_bind_key(argv: &[String], depth: usize) -> Result<Command, CommandParseError> {
    let mut index = 1;
    let mut repeatable = false;
    let mut root = false;
    let mut table = None;
    while let Some(argument) = argv.get(index) {
        match argument.as_str() {
            "-r" => repeatable = true,
            "-n" => root = true,
            "-T" => {
                if table.is_some() {
                    return Err(CommandParseError::new("duplicate -T option"));
                }
                let name = argv
                    .get(index + 1)
                    .ok_or_else(|| CommandParseError::new("missing argument for -T"))?;
                table = Some(KeyTableTarget::parse(name.clone()).map_err(key_parse_error)?);
                index += 1;
            }
            option if option.starts_with('-') => {
                return Err(CommandParseError::new(format!("unknown option: {option}")));
            }
            _ => break,
        }
        index += 1;
    }
    if root && table.is_some() {
        return Err(CommandParseError::new("-n and -T are mutually exclusive"));
    }
    let key = argv
        .get(index)
        .ok_or_else(|| CommandParseError::new("bind-key requires a key"))?;
    let key = KeyCode::parse(key).map_err(key_parse_error)?;
    let nested = argv
        .get(index + 1..)
        .filter(|nested| !nested.is_empty())
        .ok_or_else(|| CommandParseError::new("bind-key requires a command"))?;
    let commands = parser::parse_nested_command(nested, depth)?;
    Ok(Command::BindKey {
        table: table.unwrap_or_else(|| {
            if root {
                KeyTableTarget::root()
            } else {
                KeyTableTarget::prefix()
            }
        }),
        key,
        repeatable,
        commands,
    })
}

fn parse_send_keys(argv: &[String]) -> Result<Command, CommandParseError> {
    let mut index = 1;
    let mut literal = false;
    let mut repeat = 1_u16;
    let mut target = None;
    while let Some(argument) = argv.get(index) {
        match argument.as_str() {
            "-l" => literal = true,
            "-N" => {
                let raw = argv
                    .get(index + 1)
                    .ok_or_else(|| CommandParseError::new("missing argument for -N"))?;
                repeat = raw.parse::<u16>().map_err(|_| {
                    CommandParseError::new("send-keys repeat must be between 1 and 10000")
                })?;
                if !(1..=MAX_SEND_REPEAT).contains(&repeat) {
                    return Err(CommandParseError::new(
                        "send-keys repeat must be between 1 and 10000",
                    ));
                }
                index += 1;
            }
            "-t" => {
                let raw = argv
                    .get(index + 1)
                    .ok_or_else(|| CommandParseError::new("missing argument for -t"))?;
                target = Some(
                    TargetSpec::parse(raw.clone())
                        .map_err(|error| CommandParseError::new(error.to_string()))?,
                );
                index += 1;
            }
            "--" => {
                index += 1;
                break;
            }
            option if option.starts_with('-') => {
                return Err(CommandParseError::new(format!("unknown option: {option}")));
            }
            _ => break,
        }
        index += 1;
    }
    let raw_keys = argv
        .get(index..)
        .filter(|keys| !keys.is_empty())
        .ok_or_else(|| CommandParseError::new("send-keys requires at least one key"))?;
    let keys = raw_keys
        .iter()
        .map(|raw| {
            if literal {
                SendKey::Literal(raw.clone())
            } else {
                KeyCode::parse(raw)
                    .map(SendKey::Key)
                    .unwrap_or_else(|_| SendKey::Literal(raw.clone()))
            }
        })
        .collect::<Vec<_>>();
    let bytes_per_repeat = keys
        .iter()
        .try_fold(0_usize, |total, key| total.checked_add(key.encoded_len()));
    let total_bytes = bytes_per_repeat.and_then(|bytes| bytes.checked_mul(usize::from(repeat)));
    if total_bytes.is_none_or(|bytes| bytes > MAX_SEND_BYTES) {
        return Err(CommandParseError::new(
            "send-keys encoded output exceeds 1048576 bytes",
        ));
    }
    Ok(Command::SendKeys {
        target,
        keys,
        literal,
        repeat,
    })
}

fn parse_send_prefix(argv: &[String]) -> Result<Command, CommandParseError> {
    match argv {
        [_] => Ok(Command::SendPrefix { target: None }),
        [_, option, raw] if option == "-t" => Ok(Command::SendPrefix {
            target: Some(
                TargetSpec::parse(raw.clone())
                    .map_err(|error| CommandParseError::new(error.to_string()))?,
            ),
        }),
        [_, option] if option == "-t" => Err(CommandParseError::new("missing argument for -t")),
        [_, option, ..] if option.starts_with('-') => {
            Err(CommandParseError::new(format!("unknown option: {option}")))
        }
        _ => Err(CommandParseError::new("invalid send-prefix arguments")),
    }
}

fn parse_switch_client(argv: &[String]) -> Result<Command, CommandParseError> {
    let mut index = 1;
    let mut selector = None;
    while let Some(argument) = argv.get(index) {
        let candidate = match argument.as_str() {
            "-n" => SessionSelector::Next,
            "-p" => SessionSelector::Previous,
            "-l" => SessionSelector::Last,
            "-t" => {
                let raw = argv
                    .get(index + 1)
                    .ok_or_else(|| CommandParseError::new("missing argument for -t"))?;
                index += 1;
                SessionSelector::Target(
                    TargetSpec::parse(raw.clone())
                        .map_err(|error| CommandParseError::new(error.to_string()))?,
                )
            }
            option if option.starts_with('-') => {
                return Err(CommandParseError::new(format!("unknown option: {option}")));
            }
            _ => return Err(CommandParseError::new("invalid switch-client arguments")),
        };
        if selector.replace(candidate).is_some() {
            return Err(CommandParseError::new(
                "switch-client selectors are mutually exclusive",
            ));
        }
        index += 1;
    }
    Ok(Command::SwitchClient {
        target: selector.unwrap_or_else(|| SessionSelector::Target(TargetSpec::current())),
    })
}

fn parse_confirm_before(argv: &[String], depth: usize) -> Result<Command, CommandParseError> {
    let mut index = 1;
    let mut prompt = None;
    while let Some(argument) = argv.get(index) {
        match argument.as_str() {
            "-p" => {
                prompt = Some(
                    argv.get(index + 1)
                        .ok_or_else(|| CommandParseError::new("missing argument for -p"))?
                        .clone(),
                );
                index += 2;
            }
            option if option.starts_with('-') => {
                return Err(CommandParseError::new(format!("unknown option: {option}")));
            }
            _ => break,
        }
    }
    let nested = argv
        .get(index..)
        .filter(|commands| !commands.is_empty())
        .ok_or_else(|| CommandParseError::new("confirm-before requires a command"))?;
    let commands = parser::parse_nested_command(nested, depth)?;
    let prompt = prompt.unwrap_or_else(|| {
        let formatted = format_command(&commands[0]);
        let name = formatted
            .split_once(' ')
            .map_or(formatted.as_str(), |(name, _)| name);
        format!("Confirm '{name}'? (y/n)")
    });
    if prompt.len() > MAX_CONFIRM_PROMPT_BYTES {
        return Err(CommandParseError::new(
            "confirmation prompt exceeds 4096 bytes",
        ));
    }
    Ok(Command::ConfirmBefore { prompt, commands })
}

fn parse_unbind_key(argv: &[String]) -> Result<Command, CommandParseError> {
    let mut index = 1;
    let mut all = false;
    let mut root = false;
    let mut table = None;
    while let Some(argument) = argv.get(index) {
        match argument.as_str() {
            "-a" => all = true,
            "-n" => root = true,
            "-T" => {
                if table.is_some() {
                    return Err(CommandParseError::new("duplicate -T option"));
                }
                let name = argv
                    .get(index + 1)
                    .ok_or_else(|| CommandParseError::new("missing argument for -T"))?;
                table = Some(KeyTableTarget::parse(name.clone()).map_err(key_parse_error)?);
                index += 1;
            }
            option if option.starts_with('-') => {
                return Err(CommandParseError::new(format!("unknown option: {option}")));
            }
            _ => break,
        }
        index += 1;
    }
    if root && table.is_some() {
        return Err(CommandParseError::new("-n and -T are mutually exclusive"));
    }
    let remaining = &argv[index..];
    if remaining.len() > 1 {
        return Err(CommandParseError::new("unbind-key accepts at most one key"));
    }
    if all && !remaining.is_empty() {
        return Err(CommandParseError::new(
            "unbind-key -a does not accept a key",
        ));
    }
    if !all && remaining.is_empty() {
        return Err(CommandParseError::new("unbind-key requires a key or -a"));
    }
    let key = remaining
        .first()
        .map(|key| KeyCode::parse(key).map_err(key_parse_error))
        .transpose()?;
    Ok(Command::UnbindKey {
        table: table.unwrap_or_else(|| {
            if root {
                KeyTableTarget::root()
            } else {
                KeyTableTarget::prefix()
            }
        }),
        key,
        all,
    })
}

fn parse_list_keys(argv: &[String]) -> Result<Command, CommandParseError> {
    match argv {
        [_] => Ok(Command::ListKeys { table: None }),
        [_, option, name] if option == "-T" => Ok(Command::ListKeys {
            table: Some(KeyTableTarget::parse(name.clone()).map_err(key_parse_error)?),
        }),
        [_, option, ..] if option.starts_with('-') && option != "-T" => {
            Err(CommandParseError::new(format!("unknown option: {option}")))
        }
        [_, option] if option == "-T" => Err(CommandParseError::new("missing argument for -T")),
        _ => Err(CommandParseError::new("invalid list-keys arguments")),
    }
}

fn key_parse_error(error: impl std::fmt::Display) -> CommandParseError {
    CommandParseError::new(error.to_string())
}

fn format_command_list(commands: &CommandList) -> String {
    commands
        .iter()
        .map(format_command)
        .collect::<Vec<_>>()
        .join(" ; ")
}

fn format_command(command: &Command) -> String {
    use parser::quote_argument;

    let mut arguments = Vec::new();
    let name = match command {
        Command::StartServer => "start-server",
        Command::KillServer => "kill-server",
        Command::ListClients => "list-clients",
        Command::ListSessions => "list-sessions",
        Command::ListWindows { target } => {
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            "list-windows"
        }
        Command::ListPanes { target } => {
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            "list-panes"
        }
        Command::NewSession {
            name,
            group,
            attach,
            attach_if_exists,
            ..
        } => {
            if !attach {
                arguments.push("-d".to_string());
            }
            if *attach_if_exists {
                arguments.push("-A".to_string());
            }
            if let Some(name) = name {
                arguments.extend(["-s".to_string(), name.clone()]);
            }
            if let Some(group) = group {
                arguments.extend(["-t".to_string(), group.to_string()]);
            }
            "new-session"
        }
        Command::NewWindow { name } => {
            if let Some(name) = name {
                arguments.extend(["-n".to_string(), name.clone()]);
            }
            "new-window"
        }
        Command::SplitWindow {
            direction,
            detached,
        } => {
            if matches!(direction, SplitDirection::LeftRight) {
                arguments.push("-h".to_string());
            }
            if *detached {
                arguments.push("-d".to_string());
            }
            "split-window"
        }
        Command::SelectWindow { target } => {
            arguments.extend(["-t".to_string(), target.to_string()]);
            "select-window"
        }
        Command::SelectPane { target } => {
            match target {
                PaneTarget::Target(target) => {
                    arguments.extend(["-t".to_string(), target.to_string()]);
                }
                PaneTarget::Direction(direction) => arguments.push(
                    match direction {
                        ResizeDirection::Left => "-L",
                        ResizeDirection::Right => "-R",
                        ResizeDirection::Up => "-U",
                        ResizeDirection::Down => "-D",
                    }
                    .to_string(),
                ),
            }
            "select-pane"
        }
        Command::ResizePane { target, amount } => {
            arguments.push(
                match target {
                    ResizeTarget::Zoom => "-Z",
                    ResizeTarget::Direction(ResizeDirection::Left) => "-L",
                    ResizeTarget::Direction(ResizeDirection::Right) => "-R",
                    ResizeTarget::Direction(ResizeDirection::Up) => "-U",
                    ResizeTarget::Direction(ResizeDirection::Down) => "-D",
                }
                .to_string(),
            );
            if !matches!(target, ResizeTarget::Zoom) {
                arguments.push(amount.to_string());
            }
            "resize-pane"
        }
        Command::RenameWindow { name } => {
            arguments.push(name.clone());
            "rename-window"
        }
        Command::RotateWindow { reverse } => {
            if *reverse {
                arguments.push("-D".to_string());
            }
            "rotate-window"
        }
        Command::SwapPane { direction } => {
            arguments.push(
                match direction {
                    ResizeDirection::Left => "-L",
                    ResizeDirection::Right => "-R",
                    ResizeDirection::Up => "-U",
                    ResizeDirection::Down => "-D",
                }
                .to_string(),
            );
            "swap-pane"
        }
        Command::KillPane => "kill-pane",
        Command::KillWindow => "kill-window",
        Command::KillSession { target } => {
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            "kill-session"
        }
        Command::AttachSession { target } => {
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            "attach-session"
        }
        Command::CopyMode => "copy-mode",
        Command::DetachClient => "detach-client",
        Command::BindKey {
            table,
            key,
            repeatable,
            commands,
        } => {
            if *repeatable {
                arguments.push("-r".to_string());
            }
            arguments.extend([
                "-T".to_string(),
                table.as_str().to_string(),
                key.to_string(),
            ]);
            let nested = format_command_list(commands);
            let nested = if commands.len() == 1 {
                nested
            } else {
                quote_argument(&nested)
            };
            return format!(
                "bind-key {} {nested}",
                arguments
                    .iter()
                    .map(|argument| quote_argument(argument))
                    .collect::<Vec<_>>()
                    .join(" ")
            );
        }
        Command::UnbindKey { table, key, all } => {
            if *all {
                arguments.push("-a".to_string());
            }
            arguments.extend(["-T".to_string(), table.as_str().to_string()]);
            if let Some(key) = key {
                arguments.push(key.to_string());
            }
            "unbind-key"
        }
        Command::ListKeys { table } => {
            if let Some(table) = table {
                arguments.extend(["-T".to_string(), table.as_str().to_string()]);
            }
            "list-keys"
        }
        Command::SendKeys {
            target,
            keys,
            literal,
            repeat,
        } => {
            if *literal {
                arguments.push("-l".to_string());
            }
            if *repeat != 1 {
                arguments.extend(["-N".to_string(), repeat.to_string()]);
            }
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            arguments.extend(keys.iter().map(|key| match key {
                SendKey::Key(key) => key.to_string(),
                SendKey::Literal(text) => text.clone(),
            }));
            "send-keys"
        }
        Command::SendPrefix { target } => {
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            "send-prefix"
        }
        Command::SwitchClient { target } => {
            match target {
                SessionSelector::Target(target) => {
                    arguments.extend(["-t".to_string(), target.to_string()]);
                }
                SessionSelector::Next => arguments.push("-n".to_string()),
                SessionSelector::Previous => arguments.push("-p".to_string()),
                SessionSelector::Last => arguments.push("-l".to_string()),
            }
            "switch-client"
        }
        Command::RefreshClient => "refresh-client",
        Command::ConfirmBefore { prompt, commands } => {
            arguments.extend(["-p".to_string(), prompt.clone()]);
            let nested = format_command_list(commands);
            let nested = if commands.len() == 1 {
                nested
            } else {
                quote_argument(&nested)
            };
            return format!(
                "confirm-before {} {nested}",
                arguments
                    .iter()
                    .map(|argument| quote_argument(argument))
                    .collect::<Vec<_>>()
                    .join(" ")
            );
        }
        Command::SetOption {
            scope,
            target,
            unset,
            name,
            value,
        } => {
            push_option_scope(&mut arguments, *scope);
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            if *unset {
                arguments.push("-u".to_string());
            }
            arguments.push(name.clone());
            if let Some(value) = value {
                arguments.push(value.clone());
            }
            "set-option"
        }
        Command::ShowOptions {
            scope,
            target,
            local_only,
            name,
            value_only,
        } => {
            push_option_scope(&mut arguments, *scope);
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            if *local_only {
                arguments.push("-L".to_string());
            }
            if *value_only {
                arguments.push("-v".to_string());
            }
            if let Some(name) = name {
                arguments.push(name.clone());
            }
            "show-options"
        }
        Command::DisplayMessage { target, template } => {
            if let Some(target) = target {
                arguments.extend(["-t".to_string(), target.to_string()]);
            }
            arguments.push(template.clone());
            "display-message"
        }
        Command::SourceFile { path, .. } => {
            arguments.push(path.to_string_lossy().into_owned());
            "source-file"
        }
    };

    if arguments.is_empty() {
        name.to_string()
    } else {
        format!(
            "{name} {}",
            arguments
                .iter()
                .map(|argument| quote_argument(argument))
                .collect::<Vec<_>>()
                .join(" ")
        )
    }
}

fn push_option_scope(arguments: &mut Vec<String>, scope: OptionScope) {
    let flag = match scope {
        OptionScope::Server => "-g",
        OptionScope::Session => "-s",
        OptionScope::Window => "-w",
        OptionScope::Pane => "-p",
        OptionScope::Client => "-c",
    };
    arguments.push(flag.to_string());
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
        execute, parse_command, parse_command_argv, parse_command_text, Command, CommandEffect,
        CommandQueue, CommandSource,
    };
    use crate::{
        build_window_scene, render_full_scene, KeyCode, RenderState, ServerState, SplitDirection,
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

    fn execute_text(
        state: &mut ServerState,
        client: crate::ClientId,
        raw: &str,
    ) -> Result<String, String> {
        let commands = parse_command_text(raw).map_err(|error| error.to_string())?;
        let mut output = String::new();
        for (index, command) in commands.iter().cloned().enumerate() {
            let outcome = execute(
                state,
                super::QueuedCommand {
                    invocation: 1,
                    sequence: index as u64 + 1,
                    client,
                    command,
                    source: CommandSource::ClientRequest,
                    final_in_list: index + 1 == commands.len(),
                },
            );
            if !outcome.ok {
                return Err(outcome.message);
            }
            if !outcome.message.is_empty() {
                if !output.is_empty() {
                    output.push('\n');
                }
                output.push_str(&outcome.message);
            }
        }
        Ok(output)
    }

    fn execute_one(
        state: &mut ServerState,
        client: crate::ClientId,
        raw: &str,
    ) -> super::CommandOutcome {
        let commands = parse_command_text(raw).unwrap();
        assert_eq!(commands.len(), 1);
        execute(
            state,
            super::QueuedCommand {
                invocation: 1,
                sequence: 1,
                client,
                command: commands[0].clone(),
                source: CommandSource::ClientRequest,
                final_in_list: true,
            },
        )
    }

    #[test]
    fn option_commands_resolve_scope_target_and_inheritance_once() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 80, 24);
        state.attach_client(client, created.session).unwrap();

        assert!(execute_one(&mut state, client, "set-option -g @level server").ok);
        assert!(execute_one(&mut state, client, "set-option @level session").ok);
        assert!(execute_one(&mut state, client, "set-window-option @level window").ok);
        assert!(execute_one(&mut state, client, "set-option -p @level pane").ok);

        assert_eq!(
            execute_text(&mut state, client, "show-options -p -v @level").unwrap(),
            "pane"
        );
        assert_eq!(
            execute_text(&mut state, client, "show-options -w -v @level").unwrap(),
            "window"
        );
        assert!(execute_one(&mut state, client, "set-option -p -u @level").ok);
        assert_eq!(
            execute_text(&mut state, client, "show-options -p -v @level").unwrap(),
            "window"
        );
        assert_eq!(
            execute_text(&mut state, client, "show-options -g @level").unwrap(),
            "@level server"
        );
    }

    #[test]
    fn option_commands_reject_wrong_scopes_and_conflicting_flags() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 80, 24);
        state.attach_client(client, created.session).unwrap();

        assert!(
            execute_one(&mut state, client, "set-option -p buffer-limit 10")
                .message
                .contains("pane scope")
        );
        assert!(parse_command_text("set-option -g -p @x value")
            .unwrap_err()
            .to_string()
            .contains("mutually exclusive"));
        assert!(parse_command_text("show-options -v")
            .unwrap_err()
            .to_string()
            .contains("requires an option name"));
    }

    #[test]
    fn display_message_uses_the_shared_target_aware_format_engine() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 90, 30);
        state.attach_client(client, created.session).unwrap();

        assert_eq!(
            execute_text(
                &mut state,
                client,
                "display-message '#{session_name}:#{window_index}:#{pane_width}x#{pane_height}'",
            )
            .unwrap(),
            "work:0:90x30"
        );
        assert!(
            execute_one(&mut state, client, "display-message -t %999 '#{pane_id}'")
                .message
                .contains("no matching pane")
        );
    }

    #[test]
    fn inserted_commands_run_before_the_remaining_active_invocation() {
        let mut queue = CommandQueue::default();
        let client = crate::ClientId::new(1);
        queue
            .push_list(
                client,
                list("display-message first; display-message last"),
                CommandSource::ClientRequest,
            )
            .unwrap();

        let first = queue.pop().unwrap();
        assert!(matches!(
            &first.command,
            Command::DisplayMessage { template, .. } if template == "first"
        ));
        queue
            .insert_after_active(
                &first,
                list("display-message inserted-1; display-message inserted-2"),
            )
            .unwrap();
        assert!(queue.finish(first, Ok(String::new())).is_none());

        for expected in ["inserted-1", "inserted-2", "last"] {
            let queued = queue.pop().unwrap();
            assert!(matches!(
                &queued.command,
                Command::DisplayMessage { template, .. } if template == expected
            ));
            let completion = queue.finish(queued, Ok(String::new()));
            if expected == "last" {
                assert!(completion.is_some());
            } else {
                assert!(completion.is_none());
            }
        }
    }

    #[test]
    fn source_file_is_a_bounded_semantic_effect_with_parseable_paths() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let outcome = execute_one(&mut state, client, "source-file 'configs/work file.wmux'");

        assert!(outcome.ok);
        assert_eq!(
            outcome.effects.as_slice(),
            [CommandEffect::SourceFile {
                path: std::path::PathBuf::from("configs/work file.wmux"),
                depth: 0,
                ancestors: std::sync::Arc::from([]),
            }]
        );
        assert!(parse_command_text("source-file")
            .unwrap_err()
            .to_string()
            .contains("requires a path"));
    }

    #[test]
    fn send_keys_and_send_prefix_emit_exact_targeted_bytes() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 80, 24);
        state.attach_client(client, created.session).unwrap();

        let literal = execute_one(&mut state, client, "send-keys -t :0.0 -l 'λ x'");
        assert_eq!(
            literal.effects.as_slice(),
            [super::CommandEffect::PaneInput {
                pane: created.pane,
                bytes: "λ x".as_bytes().to_vec(),
            }]
        );
        let literal_parts = execute_one(&mut state, client, "send -l one ' two'");
        assert_eq!(
            literal_parts.effects.as_slice(),
            [super::CommandEffect::PaneInput {
                pane: created.pane,
                bytes: b"one two".to_vec(),
            }]
        );

        let named = execute_one(&mut state, client, "send-keys -N 2 -t :0.0 Enter C-a");
        assert_eq!(
            named.effects.as_slice(),
            [super::CommandEffect::PaneInput {
                pane: created.pane,
                bytes: b"\r\x01\r\x01".to_vec(),
            }]
        );

        let prefix = execute_one(&mut state, client, "send-prefix -t :0.0");
        assert_eq!(
            prefix.effects.as_slice(),
            [super::CommandEffect::PaneInput {
                pane: created.pane,
                bytes: vec![0x02],
            }]
        );
    }

    #[test]
    fn switch_refresh_and_confirmation_are_client_scoped() {
        let mut state = ServerState::new();
        let first = state.add_client();
        let second = state.add_client();
        let alpha = state.create_session("alpha", 80, 24);
        let beta = state.create_session("beta", 80, 24);
        let gamma = state.create_session("gamma", 80, 24);
        state.attach_client(first, alpha.session).unwrap();
        state.attach_client(second, alpha.session).unwrap();

        assert!(execute_one(&mut state, first, "switch-client -n").ok);
        assert_eq!(state.clients[&first].attached_session, Some(beta.session));
        assert_eq!(state.clients[&second].attached_session, Some(alpha.session));
        assert!(execute_one(&mut state, first, "switch-client -p").ok);
        assert_eq!(state.clients[&first].attached_session, Some(alpha.session));
        assert!(execute_one(&mut state, first, "switch-client -t gamma").ok);
        assert_eq!(state.clients[&first].attached_session, Some(gamma.session));
        assert!(execute_one(&mut state, first, "switch-client -l").ok);
        assert_eq!(state.clients[&first].attached_session, Some(alpha.session));

        assert_eq!(
            execute_one(&mut state, first, "refresh-client")
                .effects
                .as_slice(),
            [super::CommandEffect::RefreshClient { client: first }]
        );

        let confirmation = execute_one(
            &mut state,
            first,
            "confirm-before -p 'kill pane? (y/n)' kill-pane",
        );
        assert!(matches!(
            confirmation.effects.as_slice(),
            [super::CommandEffect::Confirm { client, prompt, commands }]
                if *client == first && prompt == "kill pane? (y/n)" && commands.len() == 1
        ));
    }

    #[test]
    fn send_and_confirmation_limits_reject_before_state_mutation() {
        for invalid in [
            "send-keys -N 0 x",
            "send-keys -N 10001 x",
            "send-keys -N nope x",
            "send-keys -l",
            "switch-client -n -p",
            "refresh-client extra",
            "confirm-before",
        ] {
            assert!(parse_command_text(invalid).is_err(), "accepted {invalid:?}");
        }

        let oversized_prompt = "x".repeat(4 * 1024 + 1);
        assert!(parse_command_argv(&[
            "confirm-before".to_string(),
            "-p".to_string(),
            oversized_prompt,
            "kill-pane".to_string(),
        ])
        .is_err());

        assert!(parse_command_text(&format!("send-keys -l -N 10000 {}", "x".repeat(105))).is_err());
        assert!(parse_command_text(&format!("send-keys -l -N 10000 {}", "x".repeat(104))).is_ok());
    }

    #[test]
    fn bind_unbind_and_list_keys_use_parsed_command_lists() {
        let bind = parse_command_text("bind-key -r -T prefix C-j 'select-pane -D'").unwrap();
        assert!(matches!(
            &bind[0],
            Command::BindKey {
                table,
                key,
                repeatable: true,
                commands,
            } if table.as_str() == "prefix" && *key == KeyCode::ctrl('j') && commands.len() == 1
        ));

        let mut state = ServerState::new();
        let client = state.add_client();
        execute_text(
            &mut state,
            client,
            "bind-key -r -T prefix C-j 'select-pane -D'",
        )
        .unwrap();
        let listed = execute_text(&mut state, client, "list-keys -T prefix").unwrap();
        assert!(listed.contains("bind-key -r -T prefix C-j select-pane -D"));
        execute_text(&mut state, client, "unbind-key -T prefix C-j").unwrap();
        assert!(!execute_text(&mut state, client, "list-keys -T prefix")
            .unwrap()
            .contains("C-j"));
    }

    #[test]
    fn bind_alias_defaults_tables_replaces_and_lists_parseably() {
        let mut state = ServerState::new();
        let client = state.add_client();
        for command in [
            "bind q list-sessions",
            "bind-key -n C-a list-clients",
            "bind-key -T copy-mode x list-windows",
            "bind-key -T custom F2 'rename-window \"two words\"'",
            "bind-key -T custom F3 'list-sessions; list-clients'",
            "bind q list-panes",
        ] {
            execute_text(&mut state, client, command).unwrap();
        }

        let prefix = execute_text(&mut state, client, "list-keys -T prefix").unwrap();
        assert_eq!(prefix.matches(" -T prefix q ").count(), 1);
        assert!(prefix.contains(" -T prefix q list-panes"));
        assert!(execute_text(&mut state, client, "list-keys -T root")
            .unwrap()
            .contains(" -T root C-a list-clients"));
        assert!(execute_text(&mut state, client, "list-keys -T copy-mode")
            .unwrap()
            .contains(" -T copy-mode x list-windows"));

        let all = execute_text(&mut state, client, "list-keys").unwrap();
        for line in all.lines() {
            let parsed = parse_command_text(line).unwrap();
            if line.contains(" -T custom F3 ") {
                assert!(matches!(
                    &parsed[0],
                    Command::BindKey { commands, .. } if commands.len() == 2
                ));
            }
        }
        let table_positions = [" -T root ", " -T prefix ", " -T copy-mode ", " -T custom "]
            .map(|table| all.find(table).unwrap());
        assert!(table_positions.windows(2).all(|pair| pair[0] < pair[1]));
    }

    #[test]
    fn unbind_all_is_table_scoped_and_binding_errors_are_bounded() {
        let mut state = ServerState::new();
        let client = state.add_client();
        execute_text(&mut state, client, "bind-key -n q list-sessions").unwrap();
        execute_text(&mut state, client, "unbind -a").unwrap();

        assert!(execute_text(&mut state, client, "list-keys -T prefix")
            .unwrap()
            .is_empty());
        assert!(execute_text(&mut state, client, "list-keys -T root")
            .unwrap()
            .contains(" -T root q list-sessions"));

        for invalid in [
            "bind-key -n -T prefix q list-sessions",
            "bind-key NotAKey list-sessions",
            "bind-key q",
            "unbind-key",
            "unbind-key -a q",
            "unbind-key -n -T prefix q",
            "list-keys -T ''",
        ] {
            assert!(parse_command_text(invalid).is_err(), "accepted {invalid:?}");
        }
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
