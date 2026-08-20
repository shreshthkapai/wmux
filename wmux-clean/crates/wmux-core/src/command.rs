use std::collections::VecDeque;

use crate::{ClientId, PaneId, ResizeDirection, ServerState, SessionId, SplitDirection};

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Command {
    StartServer,
    KillServer,
    ListClients,
    ListSessions,
    ListWindows {
        target: Option<String>,
    },
    ListPanes {
        target: Option<String>,
    },
    NewSession {
        name: Option<String>,
        group: Option<String>,
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
        target: WindowTarget,
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
        target: Option<String>,
    },
    AttachSession {
        target: Option<String>,
    },
    CopyMode,
    DetachClient,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum WindowTarget {
    Index(u16),
    Next,
    Previous,
    Last,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PaneTarget {
    Active,
    Last,
    Direction(ResizeDirection),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ResizeTarget {
    Direction(ResizeDirection),
    Zoom,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct QueuedCommand {
    pub sequence: u64,
    pub client: ClientId,
    pub command: Command,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandResult {
    pub sequence: u64,
    pub ok: bool,
    pub message: String,
    pub attached_pane: Option<PaneId>,
}

#[derive(Debug, Default)]
pub struct CommandQueue {
    next_sequence: u64,
    pending: VecDeque<QueuedCommand>,
}

impl CommandQueue {
    pub fn push(&mut self, client: ClientId, command: Command) -> u64 {
        self.next_sequence += 1;
        let sequence = self.next_sequence;
        self.pending.push_back(QueuedCommand {
            sequence,
            client,
            command,
        });
        sequence
    }

    pub fn pop(&mut self) -> Option<QueuedCommand> {
        self.pending.pop_front()
    }

    pub fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }
}

pub fn execute(state: &mut ServerState, queued: QueuedCommand) -> CommandResult {
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
            let Some(session) = state.find_session(target.as_deref()) else {
                return error_result(queued.sequence, "no matching session");
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
            let Some(session) = state.find_session(target.as_deref()) else {
                return error_result(queued.sequence, "no matching session");
            };
            let Some(window) = state.active_window_for_session(session) else {
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
            if let Some(existing) = state.find_session(Some(&name)) {
                if attach_if_exists {
                    return attach_result(state, queued.sequence, queued.client, existing);
                }
                return error_result(queued.sequence, format!("duplicate session: {name}"));
            }
            let created = if let Some(group) = group {
                let Some(target) = state.find_session(Some(&group)) else {
                    return error_result(queued.sequence, "no matching session group target");
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
                return error_result(queued.sequence, "session has no active window");
            };
            let target = state.windows.get(&window).map(|window| window.active_pane);
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let selected = match target {
                WindowTarget::Index(index) => state.select_window(session, index),
                WindowTarget::Next => state.select_next_window(session, false),
                WindowTarget::Previous => state.select_next_window(session, true),
                WindowTarget::Last => state.select_last_window(session),
            };
            match selected {
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
                return error_result(queued.sequence, "session has no active window");
            };
            let selected = match target {
                PaneTarget::Active => state.windows.get(&window).map(|window| window.active_pane),
                PaneTarget::Last => state.select_last_pane(window),
                PaneTarget::Direction(direction) => state.select_adjacent_pane(window, direction),
            };
            match selected {
                Some(pane) => {
                    let _ = state.attach_client(queued.client, session);
                    CommandResult {
                        sequence: queued.sequence,
                        ok: true,
                        message: String::new(),
                        attached_pane: Some(pane),
                    }
                }
                None => error_result(queued.sequence, "no matching pane"),
            }
        }
        Command::ResizePane { target, amount } => {
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(pane) = state.active_pane_for_session(session) else {
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
            let Some(session) = attached_or_first_session(state, queued.client) else {
                return error_result(queued.sequence, "no sessions");
            };
            let Some(window) = state.active_window_for_session(session) else {
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
            let Some(session) = state.find_session(target.as_deref()) else {
                return error_result(queued.sequence, "no matching session");
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
            let Some(session) = state.find_session(target.as_deref()) else {
                return CommandResult {
                    sequence: queued.sequence,
                    ok: false,
                    message: "no matching session".to_string(),
                    attached_pane: None,
                };
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

fn error_result(sequence: u64, message: impl Into<String>) -> CommandResult {
    CommandResult {
        sequence,
        ok: false,
        message: message.into(),
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

fn attached_or_first_session(state: &ServerState, client: ClientId) -> Option<SessionId> {
    state
        .clients
        .get(&client)
        .and_then(|client| client.attached_session)
        .or_else(|| state.sessions.keys().next().copied())
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandParseError(pub String);

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
        [] => Err(CommandParseError(format!("unknown command: {name}"))),
        _ => Err(CommandParseError(format!(
            "ambiguous command: {name}, could be: {}",
            matches.join(", ")
        ))),
    }
}

pub fn parse_command(argv: &[String]) -> Result<Command, CommandParseError> {
    let Some(requested_command) = argv.first().map(String::as_str) else {
        return Err(CommandParseError("empty command".to_string()));
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
            target: flag_value(argv, "-t"),
        }),
        "list-panes" => Ok(Command::ListPanes {
            target: flag_value(argv, "-t"),
        }),
        "detach-client" => Ok(Command::DetachClient),
        "copy-mode" => {
            validate_arguments(argv, &["-u"], &[], 0)?;
            Ok(Command::CopyMode)
        }
        "new-session" => {
            let name = flag_value(argv, "-s");
            let group = flag_value(argv, "-t");
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
            let target = flag_value(argv, "-t");
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
                WindowTarget::Next
            } else if has_flag(argv, "-p") {
                WindowTarget::Previous
            } else if has_flag(argv, "-l") {
                WindowTarget::Last
            } else {
                let index = flag_value(argv, "-t")
                    .and_then(|value| parse_window_index(&value))
                    .or_else(|| argv.get(1).and_then(|value| parse_window_index(value)))
                    .unwrap_or(0);
                WindowTarget::Index(index)
            };
            Ok(Command::SelectWindow { target })
        }
        "next-window" => Ok(Command::SelectWindow {
            target: WindowTarget::Next,
        }),
        "previous-window" => Ok(Command::SelectWindow {
            target: WindowTarget::Previous,
        }),
        "last-window" => Ok(Command::SelectWindow {
            target: WindowTarget::Last,
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
                PaneTarget::Last
            } else {
                PaneTarget::Active
            },
        }),
        "last-pane" => Ok(Command::SelectPane {
            target: PaneTarget::Last,
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
                target: flag_value(argv, "-t"),
            })
        }
        _ => unreachable!("resolved command is missing a parser: {command}"),
    }
}

fn flag_value(argv: &[String], flag: &str) -> Option<String> {
    argv.windows(2)
        .find_map(|pair| (pair[0] == flag).then(|| pair[1].clone()))
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
                return Err(CommandParseError(format!(
                    "missing argument for {argument}"
                )));
            }
            index += 2;
        } else if flags.contains(&argument) {
            index += 1;
        } else if argument.starts_with('-') {
            return Err(CommandParseError(format!("unknown option: {argument}")));
        } else {
            positionals += 1;
            if positionals > max_positionals {
                return Err(CommandParseError(format!(
                    "too many arguments for {}",
                    argv[0]
                )));
            }
            index += 1;
        }
    }

    Ok(())
}

fn parse_window_index(value: &str) -> Option<u16> {
    value.rsplit(':').next()?.parse::<u16>().ok()
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
    use super::{execute, parse_command, Command, CommandQueue};
    use crate::{build_window_scene, render_full_scene, RenderState, ServerState, SplitDirection};

    #[test]
    fn command_queue_executes_deterministically() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let mut queue = CommandQueue::default();

        let first = queue.push(
            client,
            Command::NewSession {
                name: Some("a".to_string()),
                group: None,
                attach: true,
                attach_if_exists: false,
                cols: 80,
                rows: 24,
            },
        );
        let second = queue.push(client, Command::StartServer);

        assert_eq!(queue.pop().unwrap().sequence, first);
        let result = execute(
            &mut state,
            super::QueuedCommand {
                sequence: first,
                client,
                command: Command::NewSession {
                    name: Some("a".to_string()),
                    group: None,
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        assert!(result.ok);
        assert!(result.attached_pane.is_some());
        assert_eq!(queue.pop().unwrap().sequence, second);
    }

    #[test]
    fn new_session_with_existing_name_errors() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let first = execute(
            &mut state,
            super::QueuedCommand {
                sequence: 1,
                client,
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
                sequence: 2,
                client,
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
        assert!(second.attached_pane.is_none());
    }

    #[test]
    fn new_session_attach_if_exists_reuses_existing_session() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let first = execute(
            &mut state,
            super::QueuedCommand {
                sequence: 1,
                client,
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
                sequence: 2,
                client,
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
        assert_eq!(first.attached_pane, second.attached_pane);
    }

    #[test]
    fn detached_new_session_returns_created_pane_for_server_spawn() {
        let mut state = ServerState::new();
        let client = state.add_client();

        let result = execute(
            &mut state,
            super::QueuedCommand {
                sequence: 1,
                client,
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
        assert!(result.attached_pane.is_some());
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
                sequence: 1,
                client,
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
                sequence: 2,
                client,
                command: Command::NewWindow {
                    name: Some("edit".to_string()),
                },
            },
        );
        assert!(new_window.ok);

        let split = execute(
            &mut state,
            super::QueuedCommand {
                sequence: 3,
                client,
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
                sequence: 4,
                client,
                command: Command::ListWindows { target: None },
            },
        );
        assert!(windows.message.contains("*1: edit panes=2"));

        let panes = execute(
            &mut state,
            super::QueuedCommand {
                sequence: 5,
                client,
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
                sequence: 1,
                client,
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
                sequence: 2,
                client,
                command: Command::NewSession {
                    name: Some("G2".to_string()),
                    group: Some("G1".to_string()),
                    attach: true,
                    attach_if_exists: false,
                    cols: 80,
                    rows: 24,
                },
            },
        );
        assert!(second.ok);
        let g1 = state.find_session(Some("G1")).unwrap();
        let g2 = state.find_session(Some("G2")).unwrap();
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
                group: Some("base".to_string()),
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
                target: Some("test".to_string())
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
                target: Some("test".to_string())
            }
        );
        assert_eq!(
            parse_command(&["lsp".to_string(), "-t".to_string(), "test".to_string()]).unwrap(),
            Command::ListPanes {
                target: Some("test".to_string())
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
            super::resolve_command_name("kill-s").unwrap_err().0,
            "ambiguous command: kill-s, could be: kill-server, kill-session"
        );
        assert_eq!(
            super::resolve_command_name("status").unwrap_err().0,
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
            .0,
            "unknown option: -s"
        );
        assert_eq!(
            parse_command(&["kill-session".to_string(), "test".to_string()])
                .unwrap_err()
                .0,
            "too many arguments for kill-session"
        );
        assert_eq!(
            parse_command(&["kill-session".to_string(), "-t".to_string()])
                .unwrap_err()
                .0,
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
                sequence: 1,
                client,
                command: Command::KillSession {
                    target: Some("test".to_string()),
                },
            },
        );

        assert!(result.ok);
        assert!(result.message.is_empty());
        assert!(state.find_session(Some("test")).is_none());
    }
}
