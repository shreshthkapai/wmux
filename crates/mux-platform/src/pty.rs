use mux_core::PaneId;

use crate::process::{ExitStatus, TerminateMode};
use crate::terminal::TerminalSize;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct BackendPaneId(pub u64);

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpawnPaneRequest {
    pub pane_id: PaneId,
    pub command: Vec<String>,
    pub cwd: Option<String>,
    pub size: TerminalSize,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpawnedPane {
    pub pane_id: PaneId,
    pub backend_id: BackendPaneId,
    pub process_id: Option<u32>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PtyEvent {
    Output { pane: PaneId, bytes: Vec<u8> },
    Exited { pane: PaneId, status: ExitStatus },
    Closed { pane: PaneId },
    Error { pane: PaneId, message: String },
}

pub trait PtyBackend {
    type Error;

    fn spawn_pane(&mut self, request: SpawnPaneRequest) -> Result<SpawnedPane, Self::Error>;
    fn write_pane_input(&mut self, pane: BackendPaneId, bytes: &[u8]) -> Result<(), Self::Error>;
    fn resize_pane(&mut self, pane: BackendPaneId, size: TerminalSize) -> Result<(), Self::Error>;
    fn close_pane_input(&mut self, pane: BackendPaneId) -> Result<(), Self::Error>;
    fn terminate_pane(
        &mut self,
        pane: BackendPaneId,
        mode: TerminateMode,
    ) -> Result<(), Self::Error>;
}
