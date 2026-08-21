use crate::{PlatformError, PlatformResult, TerminalSize};
use std::{ffi::OsString, path::PathBuf, sync::Arc};

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct PlatformPaneId(u64);

impl PlatformPaneId {
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    /// Returns the stable wmux token used for adapter lookup.
    ///
    /// This value is never a native handle, descriptor, or process ID.
    pub const fn raw(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandSpec {
    pub program: OsString,
    pub args: Vec<OsString>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpawnPane {
    pub pane: PlatformPaneId,
    pub size: TerminalSize,
    pub command: Option<CommandSpec>,
    pub cwd: Option<PathBuf>,
    pub environment: Vec<(OsString, OsString)>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TerminationMode {
    Graceful,
    Force,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PlatformRequest {
    SpawnPane(SpawnPane),
    WritePane {
        pane: PlatformPaneId,
        bytes: Vec<u8>,
    },
    ResizePane {
        pane: PlatformPaneId,
        size: TerminalSize,
    },
    TerminatePane {
        pane: PlatformPaneId,
        mode: TerminationMode,
    },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PlatformEvent {
    PtyOutput {
        pane: PlatformPaneId,
        bytes: Vec<u8>,
    },
    PtyExited {
        pane: PlatformPaneId,
        exit_code: Option<u32>,
    },
    /// No event for `pane` may follow this event.
    PtyClosed { pane: PlatformPaneId },
    BackendError {
        pane: PlatformPaneId,
        error: PlatformError,
    },
}

pub type PtyEvent = PlatformEvent;
pub type PlatformNotifier = Arc<dyn Fn(PlatformPaneId) + Send + Sync>;

/// Owns native pane mechanics behind semantic requests and nonblocking events.
pub trait PtyBackend: Send {
    fn submit(&mut self, request: PlatformRequest) -> PlatformResult<()>;

    fn try_next_event(&mut self, pane: PlatformPaneId) -> PlatformResult<Option<PlatformEvent>>;
}
