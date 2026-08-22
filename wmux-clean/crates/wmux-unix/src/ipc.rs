use std::{
    ffi::OsString,
    fs::{self, File, OpenOptions},
    io::{self, Read, Write},
    os::unix::{
        fs::{FileTypeExt, MetadataExt, OpenOptionsExt, PermissionsExt},
        io::AsRawFd,
        net::{UnixListener, UnixStream},
    },
    path::{Path, PathBuf},
};

pub(crate) struct UnixEndpoint {
    directory: PathBuf,
    socket: PathBuf,
    lock: PathBuf,
    owner_uid: libc::uid_t,
}

impl UnixEndpoint {
    pub(crate) fn current_user() -> io::Result<Self> {
        let owner_uid = unsafe { libc::geteuid() };
        let directory = runtime_directory(
            std::env::var_os("XDG_RUNTIME_DIR"),
            std::env::temp_dir(),
            owner_uid,
        );
        Self::from_runtime_directory(directory)
    }

    pub(crate) fn from_runtime_directory(directory: PathBuf) -> io::Result<Self> {
        if directory.as_os_str().is_empty() || !directory.is_absolute() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "runtime directory must be an absolute path",
            ));
        }
        let socket = directory.join("wmux.sock");
        let lock = directory.join("wmux.lock");
        Ok(Self {
            directory,
            socket,
            lock,
            owner_uid: unsafe { libc::geteuid() },
        })
    }

    pub(crate) fn socket_path(&self) -> &Path {
        &self.socket
    }

    pub(crate) fn owner_identity(&self) -> wmux_platform::PeerIdentity {
        identity_from_uid(self.owner_uid)
    }

    pub(crate) fn bind(&self) -> io::Result<BoundEndpoint> {
        self.prepare_directory()?;
        let mut lock_file = match self.create_lock() {
            Ok(file) => file,
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                self.recover_stale_endpoint()?;
                self.create_lock()?
            }
            Err(error) => return Err(error),
        };
        writeln!(lock_file, "{}", std::process::id())?;
        fs::set_permissions(&self.lock, fs::Permissions::from_mode(0o600))?;
        let listener = match UnixListener::bind(&self.socket) {
            Ok(listener) => listener,
            Err(error) => {
                let _ = fs::remove_file(&self.lock);
                return Err(error);
            }
        };
        if let Err(error) = fs::set_permissions(&self.socket, fs::Permissions::from_mode(0o600)) {
            let _ = fs::remove_file(&self.socket);
            let _ = fs::remove_file(&self.lock);
            return Err(error);
        }
        if let Err(error) = listener.set_nonblocking(true) {
            let _ = fs::remove_file(&self.socket);
            let _ = fs::remove_file(&self.lock);
            return Err(error);
        }
        let socket_path = OwnedPath::capture(self.socket.clone())?;
        let lock_path = OwnedPath::capture(self.lock.clone())?;
        Ok(BoundEndpoint {
            listener: Some(listener),
            async_listener: None,
            _lock_file: lock_file,
            socket_path,
            lock_path,
        })
    }

    fn prepare_directory(&self) -> io::Result<()> {
        match fs::symlink_metadata(&self.directory) {
            Ok(metadata) => {
                if !metadata.file_type().is_dir() || metadata.uid() != self.owner_uid {
                    return Err(io::Error::new(
                        io::ErrorKind::PermissionDenied,
                        "runtime directory is not an owner-controlled directory",
                    ));
                }
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {
                fs::create_dir(&self.directory)?;
            }
            Err(error) => return Err(error),
        }
        fs::set_permissions(&self.directory, fs::Permissions::from_mode(0o700))
    }

    fn create_lock(&self) -> io::Result<File> {
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
            .open(&self.lock)
    }

    fn recover_stale_endpoint(&self) -> io::Result<()> {
        let lock_metadata = fs::symlink_metadata(&self.lock)?;
        if !lock_metadata.file_type().is_file()
            || lock_metadata.uid() != self.owner_uid
            || lock_metadata.mode() & 0o077 != 0
        {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "server lock is not an owner-only regular file",
            ));
        }

        let mut lock = OpenOptions::new()
            .read(true)
            .custom_flags(libc::O_CLOEXEC | libc::O_NOFOLLOW)
            .open(&self.lock)?;
        let mut owner = String::new();
        lock.read_to_string(&mut owner)?;
        let owner_pid = owner.trim().parse::<libc::pid_t>().map_err(|_| {
            io::Error::new(
                io::ErrorKind::PermissionDenied,
                "server lock has no valid PID",
            )
        })?;
        if process_is_alive(owner_pid) || UnixStream::connect(&self.socket).is_ok() {
            return Err(io::Error::new(
                io::ErrorKind::AlreadyExists,
                "server endpoint has a live owner",
            ));
        }

        match fs::symlink_metadata(&self.socket) {
            Ok(metadata)
                if metadata.file_type().is_socket()
                    && metadata.uid() == self.owner_uid
                    && metadata.mode() & 0o077 == 0 =>
            {
                fs::remove_file(&self.socket)?;
            }
            Ok(_) => {
                return Err(io::Error::new(
                    io::ErrorKind::PermissionDenied,
                    "stale endpoint is not an owner-only socket",
                ));
            }
            Err(error) if error.kind() == io::ErrorKind::NotFound => {}
            Err(error) => return Err(error),
        }
        fs::remove_file(&self.lock)
    }
}

fn runtime_directory(
    xdg_runtime: Option<OsString>,
    temporary: PathBuf,
    owner_uid: libc::uid_t,
) -> PathBuf {
    xdg_runtime
        .map(PathBuf::from)
        .filter(|path| runtime_root_is_private(path, owner_uid))
        .map(|path| path.join("wmux"))
        .unwrap_or_else(|| temporary.join(format!("wmux-{owner_uid}")))
}

fn runtime_root_is_private(path: &Path, owner_uid: libc::uid_t) -> bool {
    if !path.is_absolute() {
        return false;
    }
    fs::symlink_metadata(path).is_ok_and(|metadata| {
        metadata.file_type().is_dir() && metadata.uid() == owner_uid && metadata.mode() & 0o022 == 0
    })
}

fn process_is_alive(pid: libc::pid_t) -> bool {
    if pid <= 0 {
        return false;
    }
    let result = unsafe { libc::kill(pid, 0) };
    result == 0 || io::Error::last_os_error().raw_os_error() == Some(libc::EPERM)
}

fn identity_from_uid(uid: libc::uid_t) -> wmux_platform::PeerIdentity {
    wmux_platform::PeerIdentity::from_token((uid as u64).to_be_bytes())
}

#[cfg(target_os = "linux")]
fn peer_identity(stream: &tokio::net::UnixStream) -> io::Result<wmux_platform::PeerIdentity> {
    let mut credentials = std::mem::MaybeUninit::<libc::ucred>::uninit();
    let mut length = std::mem::size_of::<libc::ucred>() as libc::socklen_t;
    let result = unsafe {
        libc::getsockopt(
            stream.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_PEERCRED,
            credentials.as_mut_ptr().cast(),
            &mut length,
        )
    };
    if result != 0 {
        return Err(io::Error::last_os_error());
    }
    if length as usize != std::mem::size_of::<libc::ucred>() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "SO_PEERCRED returned an unexpected credential size",
        ));
    }
    let credentials = unsafe { credentials.assume_init() };
    Ok(identity_from_uid(credentials.uid))
}

#[cfg(target_os = "macos")]
fn peer_identity(stream: &tokio::net::UnixStream) -> io::Result<wmux_platform::PeerIdentity> {
    let mut uid = 0;
    let mut gid = 0;
    let result = unsafe { libc::getpeereid(stream.as_raw_fd(), &mut uid, &mut gid) };
    if result != 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(identity_from_uid(uid))
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
fn peer_identity(_stream: &tokio::net::UnixStream) -> io::Result<wmux_platform::PeerIdentity> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "Unix peer credentials are implemented only on Linux and macOS",
    ))
}

pub(crate) struct BoundEndpoint {
    listener: Option<UnixListener>,
    async_listener: Option<tokio::net::UnixListener>,
    _lock_file: File,
    socket_path: OwnedPath,
    lock_path: OwnedPath,
}

impl BoundEndpoint {
    pub(crate) async fn accept(
        &mut self,
    ) -> io::Result<(tokio::net::UnixStream, wmux_platform::PeerIdentity)> {
        if self.async_listener.is_none() {
            let listener = self.listener.take().ok_or_else(|| {
                io::Error::other("Unix listener is unavailable during async conversion")
            })?;
            self.async_listener = Some(tokio::net::UnixListener::from_std(listener)?);
        }
        let listener = self
            .async_listener
            .as_ref()
            .expect("async listener is initialized above");
        let (stream, _) = listener.accept().await?;
        let peer = peer_identity(&stream)?;
        Ok((stream, peer))
    }
}

impl Drop for BoundEndpoint {
    fn drop(&mut self) {
        self.socket_path.remove_if_same();
        self.lock_path.remove_if_same();
    }
}

struct OwnedPath {
    path: PathBuf,
    device: u64,
    inode: u64,
}

impl OwnedPath {
    fn capture(path: PathBuf) -> io::Result<Self> {
        let metadata = fs::symlink_metadata(&path)?;
        Ok(Self {
            path,
            device: metadata.dev(),
            inode: metadata.ino(),
        })
    }

    fn remove_if_same(&self) {
        let Ok(metadata) = fs::symlink_metadata(&self.path) else {
            return;
        };
        if metadata.dev() == self.device && metadata.ino() == self.inode {
            let _ = fs::remove_file(&self.path);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{runtime_directory, UnixEndpoint};
    use std::{
        fs,
        os::unix::fs::PermissionsExt,
        path::{Path, PathBuf},
        process,
        time::{SystemTime, UNIX_EPOCH},
    };
    use wmux_platform::PeerIdentity;

    struct TestDirectory(PathBuf);

    impl TestDirectory {
        fn new() -> Self {
            let nonce = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("test clock is after the Unix epoch")
                .as_nanos();
            let path = std::env::temp_dir()
                .join(format!("wmux-unix-endpoint-test-{}-{nonce}", process::id()));
            Self(path)
        }

        fn path(&self) -> &Path {
            &self.0
        }
    }

    impl Drop for TestDirectory {
        fn drop(&mut self) {
            if self.0.file_name().is_some_and(|name| {
                name.to_string_lossy()
                    .starts_with("wmux-unix-endpoint-test-")
            }) {
                let _ = fs::remove_dir_all(&self.0);
            }
        }
    }

    #[test]
    fn current_user_prefers_only_a_private_xdg_runtime_root() {
        let root = TestDirectory::new();
        fs::create_dir(root.path()).expect("test root is created");
        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700))
            .expect("test root is private");
        let temporary = root.path().join("fallback");
        let owner_uid = unsafe { libc::geteuid() };

        assert_eq!(
            runtime_directory(
                Some(root.path().as_os_str().to_owned()),
                temporary.clone(),
                owner_uid,
            ),
            root.path().join("wmux")
        );

        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o777))
            .expect("test root becomes public");
        assert_eq!(
            runtime_directory(
                Some(root.path().as_os_str().to_owned()),
                temporary.clone(),
                owner_uid,
            ),
            temporary.join(format!("wmux-{owner_uid}"))
        );
        assert_eq!(
            runtime_directory(Some("relative".into()), temporary.clone(), owner_uid),
            temporary.join(format!("wmux-{owner_uid}"))
        );
        let error = match UnixEndpoint::from_runtime_directory(PathBuf::from("relative")) {
            Ok(_) => panic!("relative endpoint was accepted"),
            Err(error) => error,
        };
        assert_eq!(error.kind(), std::io::ErrorKind::InvalidInput);
    }

    #[test]
    fn endpoint_directory_and_socket_are_owner_only() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        let _bound = endpoint.bind().expect("endpoint binds");

        let directory_mode = fs::metadata(root.path())
            .expect("runtime directory exists")
            .permissions()
            .mode()
            & 0o777;
        let socket_mode = fs::metadata(endpoint.socket_path())
            .expect("socket exists")
            .permissions()
            .mode()
            & 0o777;

        assert_eq!(directory_mode, 0o700);
        assert_eq!(socket_mode, 0o600);
    }

    #[test]
    fn live_endpoint_rejects_a_second_server() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        let _owner = endpoint.bind().expect("first server binds");
        fs::remove_file(endpoint.socket_path()).expect("socket path can be unlinked");

        let error = match endpoint.bind() {
            Ok(_) => panic!("a second server replaced the live endpoint"),
            Err(error) => error,
        };

        assert_eq!(error.kind(), std::io::ErrorKind::AlreadyExists);
    }

    #[test]
    fn stale_owned_socket_is_recovered() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        fs::create_dir_all(root.path()).expect("runtime directory is created");
        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700))
            .expect("runtime directory is private");
        let stale_listener = std::os::unix::net::UnixListener::bind(endpoint.socket_path())
            .expect("stale socket is created");
        drop(stale_listener);
        fs::set_permissions(endpoint.socket_path(), fs::Permissions::from_mode(0o600))
            .expect("stale socket is private");
        fs::write(&endpoint.lock, b"999999999\n").expect("stale lock is created");
        fs::set_permissions(&endpoint.lock, fs::Permissions::from_mode(0o600))
            .expect("stale lock is private");

        let _bound = endpoint.bind().expect("dead owner is recovered");

        assert!(endpoint.socket_path().exists());
        assert!(endpoint.lock.exists());
    }

    #[test]
    fn listener_drop_removes_its_owned_socket_and_lock() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        let bound = endpoint.bind().expect("endpoint binds");
        assert!(endpoint.socket.exists());
        assert!(endpoint.lock.exists());

        drop(bound);

        assert!(!endpoint.socket.exists());
        assert!(!endpoint.lock.exists());
    }

    #[test]
    fn listener_drop_does_not_remove_replacement_files() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        let bound = endpoint.bind().expect("endpoint binds");
        fs::remove_file(&endpoint.socket).expect("owned socket is removed");
        fs::remove_file(&endpoint.lock).expect("owned lock is removed");
        let replacement = std::os::unix::net::UnixListener::bind(&endpoint.socket)
            .expect("replacement socket binds");
        fs::set_permissions(&endpoint.socket, fs::Permissions::from_mode(0o600))
            .expect("replacement socket is private");
        fs::write(&endpoint.lock, b"999999999\n").expect("replacement lock is created");
        fs::set_permissions(&endpoint.lock, fs::Permissions::from_mode(0o600))
            .expect("replacement lock is private");

        drop(bound);

        assert!(endpoint.socket.exists());
        assert!(endpoint.lock.exists());
        drop(replacement);
    }

    #[tokio::test]
    async fn accepted_peer_identity_comes_from_the_native_socket() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        let mut bound = endpoint.bind().expect("endpoint binds");
        let connect = tokio::net::UnixStream::connect(endpoint.socket_path());
        let accept = bound.accept();

        let (client, accepted) = tokio::join!(connect, accept);
        let _client = client.expect("client connects");
        let (_server, peer) = accepted.expect("server accepts authenticated peer");
        let expected = PeerIdentity::from_token((unsafe { libc::geteuid() } as u64).to_be_bytes());

        assert_eq!(peer, expected);
    }

    #[test]
    fn stale_socket_symlink_is_rejected_without_removal() {
        let root = TestDirectory::new();
        let endpoint = UnixEndpoint::from_runtime_directory(root.path().to_path_buf())
            .expect("endpoint is valid");
        fs::create_dir_all(root.path()).expect("runtime directory is created");
        fs::set_permissions(root.path(), fs::Permissions::from_mode(0o700))
            .expect("runtime directory is private");
        let victim = root.path().join("victim");
        fs::write(&victim, b"do not remove").expect("victim is created");
        std::os::unix::fs::symlink(&victim, &endpoint.socket)
            .expect("socket-path symlink is created");
        fs::write(&endpoint.lock, b"999999999\n").expect("stale lock is created");
        fs::set_permissions(&endpoint.lock, fs::Permissions::from_mode(0o600))
            .expect("stale lock is private");

        let error = match endpoint.bind() {
            Ok(_) => panic!("a socket-path symlink was replaced"),
            Err(error) => error,
        };

        assert_eq!(error.kind(), std::io::ErrorKind::PermissionDenied);
        assert!(endpoint.socket.symlink_metadata().is_ok());
        assert_eq!(fs::read(victim).expect("victim remains"), b"do not remove");
    }
}
