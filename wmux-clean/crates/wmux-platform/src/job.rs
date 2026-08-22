use crate::{PlatformError, PlatformResult};
use std::{ffi::OsString, path::PathBuf, sync::Arc};

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct PlatformJobId(u64);

impl PlatformJobId {
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    /// Returns the stable wmux token used for adapter lookup.
    /// This value is never a native process identifier or handle.
    pub const fn raw(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpawnJob {
    pub job: PlatformJobId,
    pub command: String,
    pub cwd: Option<PathBuf>,
    pub environment: Vec<(OsString, OsString)>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum JobRequest {
    Spawn(SpawnJob),
    Terminate { job: PlatformJobId },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum JobEvent {
    Output {
        job: PlatformJobId,
        bytes: Vec<u8>,
    },
    Exited {
        job: PlatformJobId,
        exit_code: Option<u32>,
    },
    /// No event for `job` may follow this event.
    Closed {
        job: PlatformJobId,
    },
    BackendError {
        job: PlatformJobId,
        error: PlatformError,
    },
}

pub type JobNotifier = Arc<dyn Fn(PlatformJobId) + Send + Sync>;

/// Owns native non-interactive process mechanics behind semantic requests.
pub trait JobBackend: Send {
    fn submit(&mut self, request: JobRequest) -> PlatformResult<()>;
    fn try_next_event(&mut self, job: PlatformJobId) -> PlatformResult<Option<JobEvent>>;
}
