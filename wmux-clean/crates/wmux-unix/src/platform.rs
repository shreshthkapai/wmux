use crate::{
    ipc::{BoundEndpoint, UnixEndpoint},
    process::spawn_daemon,
    pty::UnixPtyBackend,
};
use std::path::PathBuf;
use tokio::runtime::Handle as TokioHandle;
use wmux_platform::{
    AcceptedConnection, BoxedIpcStream, ClientTransport, DaemonSpec, Endpoint, JobBackend,
    JobNotifier, PeerIdentity, PlatformError, PlatformErrorKind, PlatformFuture, PlatformNotifier,
    PlatformResult, PtyBackend, ServerListener, ServerPlatform,
};

pub struct UnixServerPlatform {
    endpoint: UnixEndpoint,
}

impl UnixServerPlatform {
    pub fn current_user() -> PlatformResult<Self> {
        Ok(Self {
            endpoint: UnixEndpoint::current_user()
                .map_err(|error| PlatformError::from_io("discover Unix server endpoint", error))?,
        })
    }

    pub fn from_runtime_directory(directory: PathBuf) -> PlatformResult<Self> {
        Ok(Self {
            endpoint: UnixEndpoint::from_runtime_directory(directory)
                .map_err(|error| PlatformError::from_io("configure Unix server endpoint", error))?,
        })
    }
}

impl ServerPlatform for UnixServerPlatform {
    fn bind(&mut self) -> PlatformResult<Box<dyn ServerListener>> {
        let owner_identity = self.endpoint.owner_identity();
        let endpoint = Endpoint::new(self.endpoint.socket_path().display().to_string());
        let bound = self
            .endpoint
            .bind()
            .map_err(|error| PlatformError::from_io("bind Unix server endpoint", error))?;
        Ok(Box::new(UnixServerListener {
            bound,
            endpoint,
            owner_identity,
        }))
    }

    fn create_pty_backend(
        &mut self,
        notifier: PlatformNotifier,
    ) -> PlatformResult<Box<dyn PtyBackend>> {
        Ok(Box::new(UnixPtyBackend::new(
            TokioHandle::current(),
            notifier,
        )))
    }

    fn create_job_backend(&mut self, notifier: JobNotifier) -> PlatformResult<Box<dyn JobBackend>> {
        Ok(Box::new(crate::job::UnixJobBackend::new(notifier)))
    }
}

struct UnixServerListener {
    bound: BoundEndpoint,
    endpoint: Endpoint,
    owner_identity: PeerIdentity,
}

impl ServerListener for UnixServerListener {
    fn endpoint(&self) -> &Endpoint {
        &self.endpoint
    }

    fn owner_identity(&self) -> &PeerIdentity {
        &self.owner_identity
    }

    fn accept(&mut self) -> PlatformFuture<'_, AcceptedConnection> {
        Box::pin(async move {
            let (stream, peer) = self
                .bound
                .accept()
                .await
                .map_err(|error| PlatformError::from_io("accept Unix IPC client", error))?;
            if peer != self.owner_identity {
                return Err(PlatformError::new(
                    PlatformErrorKind::PermissionDenied,
                    "authenticate Unix IPC peer",
                    "peer identity does not match the endpoint owner",
                ));
            }
            Ok(AcceptedConnection {
                stream: Box::new(stream),
                peer,
            })
        })
    }
}

pub struct UnixClientTransport {
    endpoint: UnixEndpoint,
    semantic_endpoint: Endpoint,
}

impl UnixClientTransport {
    pub fn current_user() -> PlatformResult<Self> {
        let endpoint = UnixEndpoint::current_user()
            .map_err(|error| PlatformError::from_io("discover Unix server endpoint", error))?;
        Ok(Self::from_endpoint(endpoint))
    }

    pub fn from_runtime_directory(directory: PathBuf) -> PlatformResult<Self> {
        let endpoint = UnixEndpoint::from_runtime_directory(directory)
            .map_err(|error| PlatformError::from_io("configure Unix server endpoint", error))?;
        Ok(Self::from_endpoint(endpoint))
    }

    fn from_endpoint(endpoint: UnixEndpoint) -> Self {
        let semantic_endpoint = Endpoint::new(endpoint.socket_path().display().to_string());
        Self {
            endpoint,
            semantic_endpoint,
        }
    }
}

impl ClientTransport for UnixClientTransport {
    fn endpoint(&self) -> &Endpoint {
        &self.semantic_endpoint
    }

    fn connect(&self) -> PlatformFuture<'_, BoxedIpcStream> {
        Box::pin(async move {
            tokio::net::UnixStream::connect(self.endpoint.socket_path())
                .await
                .map(|stream| Box::new(stream) as BoxedIpcStream)
                .map_err(|error| PlatformError::from_io("connect Unix IPC client", error))
        })
    }

    fn spawn_server(&self, spec: &DaemonSpec) -> PlatformResult<()> {
        spawn_daemon(spec)
            .map_err(|error| PlatformError::from_io("start Unix server daemon", error))
    }
}

#[cfg(test)]
mod tests {
    use super::{UnixClientTransport, UnixServerListener, UnixServerPlatform};
    use crate::{ipc::UnixEndpoint, process::signal_group};
    use std::{
        ffi::OsString,
        fs,
        os::unix::fs::PermissionsExt,
        path::{Path, PathBuf},
        process::{self, Command},
        sync::atomic::{AtomicU64, Ordering},
        thread,
        time::{Duration, Instant},
    };
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use wmux_platform::{
        ClientTransport, DaemonSpec, PeerIdentity, ServerListener, ServerPlatform,
    };

    const DAEMON_HELPER_ENV: &str = "WMUX_UNIX_DAEMON_TEST_HELPER";
    const DAEMON_REPORT_ENV: &str = "WMUX_UNIX_DAEMON_TEST_REPORT";

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new(_label: &str) -> Self {
            static NEXT: AtomicU64 = AtomicU64::new(0);
            let nonce = NEXT.fetch_add(1, Ordering::Relaxed);
            let path = test_temporary_root().join(format!("wp-{:x}-{nonce:x}", process::id()));
            fs::create_dir(&path).expect("test directory is created");
            fs::set_permissions(&path, fs::Permissions::from_mode(0o700))
                .expect("test directory is private");
            Self(path)
        }

        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            if self
                .0
                .file_name()
                .is_some_and(|name| name.to_string_lossy().starts_with("wp-"))
            {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
    }

    #[cfg(target_os = "macos")]
    fn test_temporary_root() -> PathBuf {
        PathBuf::from("/tmp")
    }

    #[cfg(not(target_os = "macos"))]
    fn test_temporary_root() -> PathBuf {
        std::env::temp_dir()
    }

    #[test]
    fn unix_adapter_exposes_only_semantic_endpoint_and_identity() {
        let root = TestDirectory::new("contract");
        let mut server = UnixServerPlatform::from_runtime_directory(root.path().to_path_buf())
            .expect("server platform is constructed");
        let transport = UnixClientTransport::from_runtime_directory(root.path().to_path_buf())
            .expect("client transport is constructed");
        let listener = server.bind().expect("server endpoint binds");
        let owner_uid = unsafe { libc::geteuid() };
        let expected_identity = PeerIdentity::from_token((owner_uid as u64).to_be_bytes());

        assert_eq!(listener.endpoint(), transport.endpoint());
        assert_eq!(listener.owner_identity(), &expected_identity);
        assert!(listener.endpoint().display().ends_with("wmux.sock"));
        assert_eq!(
            format!("{:?}", listener.owner_identity()),
            "PeerIdentity { token_bytes: 8 }"
        );
    }

    #[tokio::test]
    async fn server_listener_rejects_a_peer_with_another_uid() {
        let root = TestDirectory::new("peer-rejection");
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        let socket_path = endpoint.socket_path().to_path_buf();
        let bound = endpoint.bind().expect("endpoint binds");
        let mut listener = UnixServerListener {
            bound,
            endpoint: wmux_platform::Endpoint::new(socket_path.display().to_string()),
            owner_identity: PeerIdentity::from_token(b"different native uid"),
        };

        let connect = tokio::net::UnixStream::connect(&socket_path);
        let accept = listener.accept();
        let (client, accepted) = tokio::join!(connect, accept);
        let _client = client.expect("client connects");
        let error = match accepted {
            Ok(_) => panic!("listener accepted a peer outside its owner identity"),
            Err(error) => error,
        };

        assert_eq!(
            error.kind(),
            wmux_platform::PlatformErrorKind::PermissionDenied
        );
    }

    #[test]
    fn daemon_server_survives_launcher_exit_and_has_no_terminal() {
        if std::env::var_os(DAEMON_HELPER_ENV).is_some() {
            let report = PathBuf::from(
                std::env::var_os(DAEMON_REPORT_ENV).expect("helper report path is provided"),
            );
            let transport = UnixClientTransport::from_runtime_directory(
                report.parent().expect("report has parent").join("endpoint"),
            )
            .expect("helper transport is constructed");
            transport
                .spawn_server(&DaemonSpec {
                    executable: PathBuf::from("/bin/sh"),
                    arguments: vec![
                        OsString::from("-c"),
                        OsString::from(concat!(
                            "printf '%s %s ' \"$$\" \"$(ps -o sid= -p $$)\" > \"$1\"; ",
                            "if [ ! -t 0 ] && [ ! -t 1 ] && [ ! -t 2 ]; ",
                            "then printf detached >> \"$1\"; else printf terminal >> \"$1\"; fi; ",
                            "sleep 30"
                        )),
                        OsString::from("wmux-daemon-test"),
                        report.into_os_string(),
                    ],
                    current_dir: std::env::current_dir().expect("helper cwd exists"),
                })
                .expect("daemon launches");
            return;
        }

        let root = TestDirectory::new("daemon");
        let report = root.path().join("daemon.txt");
        let status = Command::new(std::env::current_exe().expect("test executable is known"))
            .args([
                "--exact",
                "platform::tests::daemon_server_survives_launcher_exit_and_has_no_terminal",
                "--nocapture",
            ])
            .env(DAEMON_HELPER_ENV, "1")
            .env(DAEMON_REPORT_ENV, &report)
            .status()
            .expect("short-lived launcher runs");
        assert!(status.success(), "short-lived launcher succeeds");

        let report = wait_for_report(&report);
        let mut fields = report.split_whitespace();
        let daemon_pid = fields
            .next()
            .expect("daemon PID is reported")
            .parse::<libc::pid_t>()
            .expect("daemon PID is numeric");
        let daemon_session = fields
            .next()
            .expect("daemon session is reported")
            .parse::<libc::pid_t>()
            .expect("daemon session is numeric");
        let terminal = fields.next().expect("terminal state is reported");
        let mut cleanup = DaemonCleanup(daemon_pid);

        assert_eq!(daemon_session, daemon_pid);
        assert_ne!(daemon_session, unsafe { libc::getsid(0) });
        assert_eq!(terminal, "detached");
        assert!(process_is_alive(daemon_pid));
        cleanup.terminate();
    }

    #[tokio::test]
    async fn native_client_and_server_complete_a_detached_command() {
        let root = TestDirectory::new("command");
        let mut platform = UnixServerPlatform::from_runtime_directory(root.path().to_path_buf())
            .expect("server platform is constructed");
        let mut listener = platform.bind().expect("server binds");
        let transport = UnixClientTransport::from_runtime_directory(root.path().to_path_buf())
            .expect("client transport is constructed");

        let server = async {
            let mut accepted = listener.accept().await.expect("server accepts client");
            let mut request = [0_u8; 13];
            accepted
                .stream
                .read_exact(&mut request)
                .await
                .expect("server reads command");
            assert_eq!(&request, b"list-sessions");
            accepted
                .stream
                .write_all(b"command-ok")
                .await
                .expect("server writes response");
        };
        let client = async {
            let mut stream = transport.connect().await.expect("client connects");
            stream
                .write_all(b"list-sessions")
                .await
                .expect("client writes command");
            let mut response = [0_u8; 10];
            stream
                .read_exact(&mut response)
                .await
                .expect("client reads response");
            assert_eq!(&response, b"command-ok");
        };

        tokio::time::timeout(Duration::from_secs(5), async {
            tokio::join!(server, client);
        })
        .await
        .expect("native command exchange completes");
    }

    fn wait_for_report(path: &Path) -> String {
        let deadline = Instant::now() + Duration::from_secs(5);
        loop {
            if let Ok(report) = fs::read_to_string(path) {
                if report.split_whitespace().count() == 3 {
                    return report;
                }
            }
            assert!(Instant::now() < deadline, "daemon did not write its report");
            thread::sleep(Duration::from_millis(10));
        }
    }

    fn process_is_alive(pid: libc::pid_t) -> bool {
        let result = unsafe { libc::kill(pid, 0) };
        result == 0 || std::io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
    }

    struct DaemonCleanup(libc::pid_t);

    impl DaemonCleanup {
        fn terminate(&mut self) {
            let _ = signal_group(self.0, libc::SIGKILL);
            let deadline = Instant::now() + Duration::from_secs(5);
            while process_is_alive(self.0) && Instant::now() < deadline {
                thread::sleep(Duration::from_millis(10));
            }
            assert!(!process_is_alive(self.0), "daemon cleanup timed out");
            self.0 = 0;
        }
    }

    impl Drop for DaemonCleanup {
        fn drop(&mut self) {
            if self.0 > 0 {
                let _ = signal_group(self.0, libc::SIGKILL);
            }
        }
    }
}
