use smallvec::SmallVec;
use std::borrow::Borrow;

use super::{Command, CommandEffect, CommandOutcome, QueuedCommand};
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
            | Command::AttachSession { .. } => effects.push(CommandEffect::RefreshClient {
                client: queued.client,
            }),
            Command::StartServer
            | Command::ListClients
            | Command::ListSessions
            | Command::ListWindows { .. }
            | Command::ListPanes { .. } => {}
        }
    }

    CommandOutcome {
        ok: result.ok,
        message: result.message,
        effects,
    }
}
