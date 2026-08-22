use smallvec::SmallVec;
use std::{borrow::Borrow, sync::Arc};

use super::{Command, CommandEffect, CommandOutcome, QueuedCommand, MAX_SEND_BYTES};
use crate::ServerState;

pub fn execute(state: &mut ServerState, queued: impl Borrow<QueuedCommand>) -> CommandOutcome {
    let queued = queued.borrow();
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
            Command::DetachClient => effects.push(CommandEffect::DetachClient {
                client: queued.client,
            }),
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
            Command::NewSession { attach, .. } => {
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::EnsurePane { pane });
                }
                if *attach {
                    effects.push(CommandEffect::RefreshClient {
                        client: queued.client,
                    });
                }
            }
            Command::NewWindow { .. } | Command::SplitWindow { .. } => {
                if let Some(pane) = result.attached_pane {
                    effects.push(CommandEffect::EnsurePane { pane });
                }
                effects.push(CommandEffect::RefreshClient {
                    client: queued.client,
                });
            }
            Command::SelectWindow { .. }
            | Command::SelectPane { .. }
            | Command::ResizePane { .. }
            | Command::RenameWindow { .. }
            | Command::RotateWindow { .. }
            | Command::SwapPane { .. }
            | Command::KillPane
            | Command::KillWindow
            | Command::KillSession { .. }
            | Command::AttachSession { .. }
            | Command::SwitchClient { .. }
            | Command::RefreshClient => effects.push(CommandEffect::RefreshClient {
                client: queued.client,
            }),
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
            | Command::DisplayMessage { .. }
            | Command::SetBuffer {
                clipboard: false, ..
            }
            | Command::ShowBuffer { .. }
            | Command::ListBuffers
            | Command::DeleteBuffer { .. } => {}
        }
    }

    CommandOutcome {
        ok: result.ok,
        message: result.message,
        effects,
    }
}
