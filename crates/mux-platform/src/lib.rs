//! OS-neutral platform trait surface.
//!
//! Implementations live in platform-specific crates. Core state depends on
//! semantic events, not Windows or Unix handles.

pub mod ipc;
pub mod process;
pub mod pty;
pub mod terminal;

pub use ipc::{ConnectionId, IpcBackend, IpcEvent, PeerIdentity, ServerEndpoint};
pub use process::{ExitStatus, TerminateMode};
pub use pty::{BackendPaneId, PtyBackend, PtyEvent, SpawnPaneRequest, SpawnedPane};
pub use terminal::{TerminalClientBackend, TerminalClientHandle, TerminalEvent, TerminalSize};
