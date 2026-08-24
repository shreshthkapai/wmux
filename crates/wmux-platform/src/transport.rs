use crate::{JobBackend, JobNotifier, PlatformNotifier, PlatformResult, PtyBackend};
use std::{ffi::OsString, fmt, future::Future, path::PathBuf, pin::Pin};
use tokio::io::{AsyncRead, AsyncWrite};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Endpoint {
    display: String,
}

impl Endpoint {
    pub fn new(display: impl Into<String>) -> Self {
        Self {
            display: display.into(),
        }
    }

    pub fn display(&self) -> &str {
        &self.display
    }
}

#[derive(Clone, Eq, Hash, PartialEq)]
pub struct PeerIdentity(Vec<u8>);

impl PeerIdentity {
    pub fn from_token(token: impl AsRef<[u8]>) -> Self {
        Self(token.as_ref().to_vec())
    }
}

impl fmt::Debug for PeerIdentity {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PeerIdentity")
            .field("token_bytes", &self.0.len())
            .finish()
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DaemonSpec {
    pub executable: PathBuf,
    pub arguments: Vec<OsString>,
    pub current_dir: PathBuf,
}

pub trait IpcStream: AsyncRead + AsyncWrite + Unpin + Send {}

impl<T> IpcStream for T where T: AsyncRead + AsyncWrite + Unpin + Send {}

pub type BoxedIpcStream = Box<dyn IpcStream>;
pub type PlatformFuture<'a, T> = Pin<Box<dyn Future<Output = PlatformResult<T>> + Send + 'a>>;

pub struct AcceptedConnection {
    pub stream: BoxedIpcStream,
    pub peer: PeerIdentity,
}

pub trait ServerListener: Send {
    fn endpoint(&self) -> &Endpoint;
    fn owner_identity(&self) -> &PeerIdentity;
    fn accept(&mut self) -> PlatformFuture<'_, AcceptedConnection>;
}

pub trait ServerPlatform: Send {
    fn bind(&mut self) -> PlatformResult<Box<dyn ServerListener>>;

    fn create_pty_backend(
        &mut self,
        notifier: PlatformNotifier,
    ) -> PlatformResult<Box<dyn PtyBackend>>;

    fn create_job_backend(&mut self, notifier: JobNotifier) -> PlatformResult<Box<dyn JobBackend>>;
}

pub trait ClientTransport: Send + Sync {
    fn endpoint(&self) -> &Endpoint;
    fn connect(&self) -> PlatformFuture<'_, BoxedIpcStream>;
    fn spawn_server(&self, spec: &DaemonSpec) -> PlatformResult<()>;
}
