use super::{execute_legacy, CommandResult, QueuedCommand};
use crate::ServerState;

pub fn execute(state: &mut ServerState, queued: QueuedCommand) -> CommandResult {
    execute_legacy(state, queued)
}
