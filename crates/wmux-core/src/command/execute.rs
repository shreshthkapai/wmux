use smallvec::SmallVec;
use std::{borrow::Borrow, sync::Arc};

use super::{
    Command, CommandEffect, CommandOutcome, QueuedCommand, MAX_COMMAND_PROMPT_BYTES, MAX_SEND_BYTES,
};
use crate::{FormatContext, FormatEngine, ServerState};

pub fn execute(state: &mut ServerState, queued: impl Borrow<QueuedCommand>) -> CommandOutcome {
    let queued = queued.borrow();
    let closing_notification = closing_notification(state, queued);
    let result = super::execute_state_command(state, queued.clone());
    debug_assert_eq!(result.sequence, queued.sequence);
    let mut effects = SmallVec::new();

    if result.ok {
        match &queued.command {
            Command::KillServer => effects.push(CommandEffect::Shutdown {
                requester: queued.client,
            }),
            Command::CopyMode => effects.push(CommandEffect::EnterCopyMode {
                client: queued.client,
            }),
            Command::DetachClient => {
                effects.push(CommandEffect::Notify {
                    event: crate::HookEvent::ClientDetached,
                    target: crate::OptionTarget::Client(queued.client),
                });
                effects.push(CommandEffect::DetachClient {
                    client: queued.client,
                });
            }
            Command::SendKeys { keys, repeat, .. } => {
                let bytes_per_repeat = keys.iter().map(|key| key.encoded_len()).sum::<usize>();
                let total = bytes_per_repeat.checked_mul(usize::from(*repeat));
                if total.is_none_or(|total| total > MAX_SEND_BYTES) {
                    return CommandOutcome {
                        ok: false,
                        message: "send-keys encoded output exceeds 1048576 bytes".to_string(),
                        effects,
                    };
                }
                let mut bytes = Vec::with_capacity(total.expect("checked above"));
                for _ in 0..*repeat {
                    for key in keys {
                        key.append_bytes(&mut bytes);
                    }
                }
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::PaneInput { pane, bytes });
                }
            }
            Command::SendPrefix { .. } => {
                let mut bytes = Vec::with_capacity(8);
                state.key_tables.prefix().append_terminal_bytes(&mut bytes);
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::PaneInput { pane, bytes });
                }
            }
            Command::ConfirmBefore { prompt, commands } => {
                effects.push(CommandEffect::Confirm {
                    client: queued.client,
                    prompt: prompt.clone(),
                    commands: commands.clone(),
                });
            }
            Command::CommandPrompt {
                prompt,
                input,
                template,
            } => {
                let context = FormatContext::for_client(queued.client);
                let expanded = (|| {
                    Ok::<_, String>((
                        FormatEngine::expand(state, context, prompt)
                            .map_err(|error| error.to_string())?,
                        FormatEngine::expand(state, context, input)
                            .map_err(|error| error.to_string())?,
                        FormatEngine::expand(state, context, template)
                            .map_err(|error| error.to_string())?,
                    ))
                })();
                let (prompt, input, template) = match expanded {
                    Ok(expanded) => expanded,
                    Err(message) => {
                        return CommandOutcome {
                            ok: false,
                            message,
                            effects,
                        };
                    }
                };
                if [prompt.len(), input.len(), template.len()]
                    .into_iter()
                    .any(|len| len > MAX_COMMAND_PROMPT_BYTES)
                {
                    return CommandOutcome {
                        ok: false,
                        message: "expanded command prompt field exceeds 4096 bytes".to_string(),
                        effects,
                    };
                }
                let validation = template.replace("%%", &super::quote_argument(""));
                if let Err(error) = super::parse_command_text(&validation) {
                    return CommandOutcome {
                        ok: false,
                        message: format!("invalid command prompt template: {error}"),
                        effects,
                    };
                }
                effects.push(CommandEffect::Prompt {
                    client: queued.client,
                    prompt,
                    input,
                    template,
                });
            }
            Command::SetBuffer {
                data,
                clipboard: true,
                ..
            } => effects.push(CommandEffect::Clipboard {
                client: queued.client,
                bytes: Arc::from(data.clone()),
            }),
            Command::LoadBuffer { name, path } => effects.push(CommandEffect::ReadBufferFile {
                path: path.clone(),
                name: name.clone(),
            }),
            Command::SaveBuffer { name, path, append } => {
                let bytes = state
                    .paste_buffers
                    .get(name.as_deref())
                    .expect("state execution validated the selected buffer")
                    .shared_data();
                effects.push(CommandEffect::WriteBufferFile {
                    path: path.clone(),
                    bytes,
                    append: *append,
                });
            }
            Command::PasteBuffer {
                name,
                delete,
                bracketed,
                ..
            } => {
                let buffer = state
                    .paste_buffers
                    .get(name.as_deref())
                    .expect("state execution validated the selected buffer");
                let selected = buffer.name().to_string();
                let bytes = buffer.shared_data();
                if *delete {
                    state.paste_buffers.remove(&selected);
                }
                effects.push(CommandEffect::PastePane {
                    pane: result
                        .attached_pane
                        .expect("state execution resolved the target pane"),
                    bytes,
                    bracketed: *bracketed,
                });
            }
            Command::SourceFile {
                path,
                depth,
                ancestors,
            } => effects.push(CommandEffect::SourceFile {
                path: path.clone(),
                depth: *depth,
                ancestors: Arc::clone(ancestors),
            }),
            Command::RunShell {
                background,
                command,
            } => effects.push(CommandEffect::StartJob {
                command: command.clone(),
                background: *background,
                continuation: crate::JobContinuation::RunShell,
            }),
            Command::IfShell {
                background,
                shell_command,
                if_true,
                if_false,
            } => effects.push(CommandEffect::StartJob {
                command: shell_command.clone(),
                background: *background,
                continuation: crate::JobContinuation::IfShell {
                    if_true: if_true.clone(),
                    if_false: if_false.clone(),
                },
            }),
            Command::NewSession { attach, .. } => {
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::EnsurePane { pane });
                    if let Some(window) = state.panes.get(&pane).map(|pane| pane.window) {
                        if let Some(session) = state
                            .winlinks
                            .values()
                            .find(|winlink| winlink.window == window)
                            .map(|winlink| winlink.session)
                        {
                            effects.push(CommandEffect::Notify {
                                event: crate::HookEvent::SessionCreated,
                                target: crate::OptionTarget::Session(session),
                            });
                        }
                    }
                }
                if *attach {
                    effects.push(CommandEffect::RefreshClient {
                        client: queued.client,
                    });
                }
            }
            Command::NewWindow { .. } => {
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::EnsurePane { pane });
                    if let Some(window) = state.panes.get(&pane).map(|pane| pane.window) {
                        effects.push(CommandEffect::Notify {
                            event: crate::HookEvent::WindowCreated,
                            target: crate::OptionTarget::Window(window),
                        });
                    }
                }
                effects.push(CommandEffect::RefreshClient {
                    client: queued.client,
                });
            }
            Command::SplitWindow { .. } => {
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::EnsurePane { pane });
                    effects.push(CommandEffect::Notify {
                        event: crate::HookEvent::PaneCreated,
                        target: crate::OptionTarget::Pane(pane),
                    });
                }
                effects.push(CommandEffect::RefreshClient {
                    client: queued.client,
                });
            }
            Command::SelectWindow { .. }
            | Command::SelectPane { .. }
            | Command::ResizePane { .. }
            | Command::RenameWindow { .. }
            | Command::RenameSession { .. }
            | Command::RotateWindow { .. }
            | Command::SwapPane { .. }
            | Command::AttachSession { .. }
            | Command::SwitchClient { .. }
            | Command::RefreshClient => effects.push(CommandEffect::RefreshClient {
                client: queued.client,
            }),
            Command::KillPane | Command::KillWindow | Command::KillSession { .. } => {
                effects.push(CommandEffect::RefreshClient {
                    client: queued.client,
                });
                if let Some((event, target)) = closing_notification {
                    effects.push(CommandEffect::Notify { event, target });
                }
            }
            Command::StartServer
            | Command::ListClients
            | Command::ListSessions
            | Command::ListWindows { .. }
            | Command::ListPanes { .. }
            | Command::BindKey { .. }
            | Command::UnbindKey { .. }
            | Command::ListKeys { .. }
            | Command::SetOption { .. }
            | Command::ShowOptions { .. }
            | Command::SetHook { .. }
            | Command::ShowHooks { .. }
            | Command::DisplayMessage { .. }
            | Command::SetBuffer {
                clipboard: false, ..
            }
            | Command::ShowBuffer { .. }
            | Command::ListBuffers => {}
            Command::DeleteBuffer { .. } => effects.push(CommandEffect::Notify {
                event: crate::HookEvent::BufferDeleted,
                target: crate::OptionTarget::Server,
            }),
        }
        match &queued.command {
            Command::SetBuffer { .. } => effects.push(CommandEffect::Notify {
                event: crate::HookEvent::BufferChanged,
                target: crate::OptionTarget::Server,
            }),
            Command::PasteBuffer { delete: true, .. } => effects.push(CommandEffect::Notify {
                event: crate::HookEvent::BufferDeleted,
                target: crate::OptionTarget::Server,
            }),
            Command::AttachSession { .. } => effects.push(CommandEffect::Notify {
                event: crate::HookEvent::ClientAttached,
                target: crate::OptionTarget::Client(queued.client),
            }),
            _ => {}
        }
    }

    CommandOutcome {
        ok: result.ok,
        message: result.message,
        effects,
    }
}

fn closing_notification(
    state: &ServerState,
    queued: &QueuedCommand,
) -> Option<(crate::HookEvent, crate::OptionTarget)> {
    let (event, kind) = match &queued.command {
        Command::KillPane => (crate::HookEvent::PaneClosed, crate::TargetKind::Pane),
        Command::KillWindow => (crate::HookEvent::WindowClosed, crate::TargetKind::Window),
        Command::KillSession { .. } => {
            (crate::HookEvent::SessionClosed, crate::TargetKind::Session)
        }
        _ => return None,
    };
    let target = match &queued.command {
        Command::KillSession { target } => target.as_ref(),
        _ => None,
    };
    let resolved = super::resolve_command_target(state, queued.client, kind, target).ok()?;
    let target = match kind {
        crate::TargetKind::Session => crate::OptionTarget::Session(resolved.session?),
        crate::TargetKind::Window => crate::OptionTarget::Window(resolved.window?),
        crate::TargetKind::Pane => crate::OptionTarget::Pane(resolved.pane?),
        crate::TargetKind::Client => crate::OptionTarget::Client(resolved.client),
    };
    Some((event, target))
}
