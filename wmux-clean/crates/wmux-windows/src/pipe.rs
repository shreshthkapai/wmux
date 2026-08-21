use std::{
    env,
    fs::File,
    io,
    path::{Path, PathBuf},
};

// Windows documents a 256-character named-pipe limit. Reserve sixteen
// characters below it so callers do not rely on a transport boundary limit.
const PIPE_NAME_LIMIT: usize = 240;
const PIPE_PREFIX: &str = r"\\.\pipe\";
const ENDPOINT_PREFIX: &str = "wmux-clean-";
const LOCK_PATH_LIMIT: usize = 260;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UserSid {
    bytes: Vec<u8>,
    text: String,
}

impl UserSid {
    pub fn as_str(&self) -> &str {
        &self.text
    }

    pub(crate) fn peer_identity(&self) -> wmux_platform::PeerIdentity {
        wmux_platform::PeerIdentity::from_token(&self.bytes)
    }

    #[cfg(test)]
    fn well_known_local_system_for_test() -> Self {
        Self {
            bytes: vec![1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0],
            text: "S-1-5-18".to_string(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Endpoint {
    pipe_name: String,
    lock_path: PathBuf,
    owner_sid: UserSid,
}

impl Endpoint {
    pub fn current_user() -> io::Result<Self> {
        let instance = env::var("WMUX_INSTANCE").ok();
        Self::from_owner_sid(UserSid::current_process()?, instance.as_deref())
    }

    pub fn for_instance(instance: &str) -> io::Result<Self> {
        Self::from_owner_sid(UserSid::current_process()?, Some(instance))
    }

    fn from_owner_sid(owner_sid: UserSid, instance: Option<&str>) -> io::Result<Self> {
        let instance = instance.filter(|value| !value.is_empty());
        let base_name = format!("{ENDPOINT_PREFIX}{}", owner_sid.as_str());
        let suffix = instance
            .map(|value| instance_suffix(value, PIPE_PREFIX.len() + base_name.len()))
            .transpose()?
            .unwrap_or_default();
        let pipe_name = format!("{PIPE_PREFIX}{base_name}{suffix}");
        if pipe_name.len() > PIPE_NAME_LIMIT {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "wmux named-pipe endpoint exceeds the 240-character safety limit",
            ));
        }
        let lock_path = env::temp_dir().join(lock_file_name(&owner_sid, instance));
        if lock_path.to_string_lossy().encode_utf16().count() + 1 > LOCK_PATH_LIMIT {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "wmux lock path exceeds the Windows MAX_PATH safety limit",
            ));
        }
        Ok(Self {
            pipe_name,
            lock_path,
            owner_sid,
        })
    }

    pub fn pipe_name(&self) -> &str {
        &self.pipe_name
    }

    pub fn owner_sid(&self) -> &UserSid {
        &self.owner_sid
    }

    pub fn lock_path(&self) -> &Path {
        &self.lock_path
    }
}

#[derive(Debug)]
pub struct ServerLock {
    _file: File,
}

impl ServerLock {
    pub fn acquire(endpoint: &Endpoint) -> io::Result<Self> {
        let file = imp::acquire_lock(&endpoint.lock_path)?;
        Ok(Self { _file: file })
    }
}

impl Drop for ServerLock {
    fn drop(&mut self) {
        // The held lock handle is opened with DELETE_ON_CLOSE. The OS removes
        // exactly this handle's file when `_file` closes; it never unlinks a
        // later replacement by pathname.
    }
}

pub fn is_running(endpoint: &Endpoint) -> bool {
    connect(endpoint).is_ok()
}

fn sanitize(value: &str) -> String {
    let mut out = String::new();
    for ch in value.chars() {
        if ch.is_ascii_alphanumeric() || ch == '-' || ch == '_' {
            out.push(ch);
        } else {
            out.push('_');
        }
    }
    if out.is_empty() {
        "default".to_string()
    } else {
        out
    }
}

fn instance_suffix(value: &str, pipe_name_len_before_suffix: usize) -> io::Result<String> {
    const DIGEST_HEX_BYTES: usize = 16;
    const DIGEST_ONLY_SUFFIX_BYTES: usize = 1 + DIGEST_HEX_BYTES;
    const PREFIXED_SUFFIX_FIXED_BYTES: usize = 2 + DIGEST_HEX_BYTES;
    let available = PIPE_NAME_LIMIT
        .checked_sub(pipe_name_len_before_suffix)
        .ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::InvalidInput,
                "wmux named-pipe endpoint exceeds the 240-character safety limit",
            )
        })?;
    let digest = format!("{:016x}", stable_hash(value.as_bytes()));
    if available < DIGEST_ONLY_SUFFIX_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "wmux named-pipe endpoint leaves no room for an instance digest",
        ));
    }
    if available == DIGEST_ONLY_SUFFIX_BYTES {
        return Ok(format!("-{digest}"));
    }
    let mut prefix = sanitize(value);
    prefix.truncate(available - PREFIXED_SUFFIX_FIXED_BYTES);
    Ok(format!("-{prefix}-{digest}"))
}

fn lock_file_name(owner_sid: &UserSid, instance: Option<&str>) -> String {
    let owner_key = stable_hash(&owner_sid.bytes);
    let instance_key = stable_hash(instance.unwrap_or_default().as_bytes());
    format!("wmux-clean-lock-{owner_key:016x}-{instance_key:016x}.lock")
}

fn stable_hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf2_9ce4_8422_2325_u64, |hash, byte| {
        (hash ^ u64::from(*byte)).wrapping_mul(0x0000_0100_0000_01b3)
    })
}

#[cfg(windows)]
mod imp {
    use super::{Endpoint, UserSid};
    use std::{
        ffi::{c_void, OsStr},
        fs::{File, OpenOptions},
        io,
        mem::size_of,
        os::windows::{
            ffi::OsStrExt,
            fs::OpenOptionsExt,
            io::{AsRawHandle, FromRawHandle},
        },
        path::Path,
        ptr,
    };
    use tokio::net::windows::named_pipe::{
        ClientOptions, NamedPipeClient, NamedPipeServer, ServerOptions,
    };
    use windows_sys::Win32::{
        Foundation::{CloseHandle, LocalFree, HANDLE, INVALID_HANDLE_VALUE},
        Security::Authorization::{
            ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
        },
        Security::{
            GetLengthSid, GetTokenInformation, RevertToSelf, TokenUser, SECURITY_ATTRIBUTES,
            TOKEN_QUERY, TOKEN_USER,
        },
        System::{
            Pipes::ImpersonateNamedPipeClient,
            Threading::{GetCurrentProcess, GetCurrentThread, OpenProcessToken, OpenThreadToken},
        },
    };

    const SDDL_REVISION_1: u32 = 1;
    const GENERIC_READ: u32 = 0x8000_0000;
    const GENERIC_WRITE: u32 = 0x4000_0000;
    const OPEN_EXISTING: u32 = 3;
    const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;
    const DELETE: u32 = 0x0001_0000;
    const FILE_FLAG_DELETE_ON_CLOSE: u32 = 0x0400_0000;

    #[link(name = "kernel32")]
    extern "system" {
        fn CreateFileW(
            file_name: *const u16,
            desired_access: u32,
            share_mode: u32,
            security_attributes: *mut c_void,
            creation_disposition: u32,
            flags_and_attributes: u32,
            template_file: HANDLE,
        ) -> HANDLE;
    }

    impl UserSid {
        pub(super) fn current_process() -> io::Result<Self> {
            let mut handle = ptr::null_mut();
            if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut handle) } == 0 {
                return Err(io::Error::last_os_error());
            }
            let handle = OwnedHandle(handle);
            Self::from_token(handle.0)
        }

        fn from_token(token: HANDLE) -> io::Result<Self> {
            let mut needed = 0_u32;
            let _ =
                unsafe { GetTokenInformation(token, TokenUser, ptr::null_mut(), 0, &mut needed) };
            if needed == 0 {
                return Err(io::Error::last_os_error());
            }
            let mut buffer = vec![0_u8; needed as usize];
            if unsafe {
                GetTokenInformation(
                    token,
                    TokenUser,
                    buffer.as_mut_ptr().cast(),
                    needed,
                    &mut needed,
                )
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
            let token_user = unsafe { ptr::read_unaligned(buffer.as_ptr().cast::<TOKEN_USER>()) };
            let sid = token_user.User.Sid;
            if sid.is_null() {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "token has no user SID",
                ));
            }
            // `GetLengthSid` requires a valid non-null SID pointer.
            let sid_len = unsafe { GetLengthSid(sid) };
            if sid_len == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "token has an invalid user SID",
                ));
            }
            let bytes =
                unsafe { std::slice::from_raw_parts(sid.cast::<u8>(), sid_len as usize) }.to_vec();
            let text = sid_to_string(sid)?;
            Ok(Self { bytes, text })
        }
    }

    struct OwnedHandle(HANDLE);

    impl Drop for OwnedHandle {
        fn drop(&mut self) {
            if !self.0.is_null() && self.0 != INVALID_HANDLE_VALUE {
                let _ = unsafe { CloseHandle(self.0) };
            }
        }
    }

    fn sid_to_string(sid: *mut c_void) -> io::Result<String> {
        let mut raw = ptr::null_mut();
        if unsafe { ConvertSidToStringSidW(sid, &mut raw) } == 0 {
            return Err(io::Error::last_os_error());
        }
        let result = unsafe {
            let len = (0..).take_while(|&index| *raw.add(index) != 0).count();
            String::from_utf16(std::slice::from_raw_parts(raw, len))
                .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid SID text"))
        };
        let _ = unsafe { LocalFree(raw.cast()) };
        result
    }

    pub struct ServerPipeFactory {
        endpoint: Endpoint,
        security: SecurityDescriptor,
        created_first_instance: bool,
    }

    impl ServerPipeFactory {
        pub fn new(endpoint: Endpoint) -> io::Result<Self> {
            Ok(Self {
                security: SecurityDescriptor::from_owner(endpoint.owner_sid())?,
                endpoint,
                created_first_instance: false,
            })
        }

        pub fn create(&mut self) -> io::Result<NamedPipeServer> {
            let mut options = ServerOptions::new();
            options
                .first_pipe_instance(!self.created_first_instance)
                .reject_remote_clients(true);
            let server = unsafe {
                options.create_with_security_attributes_raw(
                    self.endpoint.pipe_name(),
                    (&mut self.security.attributes as *mut SECURITY_ATTRIBUTES).cast(),
                )
            }?;
            self.created_first_instance = true;
            Ok(server)
        }

        #[cfg(test)]
        pub(super) fn sddl(&self) -> &str {
            // The test-only inspection surface names the exact descriptor used
            // to build `security`; production keeps only the native descriptor.
            // It is never sourced from a mutable identity field.
            &self.security.security_sddl
        }
    }

    struct SecurityDescriptor {
        descriptor: *mut c_void,
        attributes: SECURITY_ATTRIBUTES,
        #[cfg(test)]
        security_sddl: String,
    }

    // The descriptor is exclusively owned by `ServerPipeFactory`. Moving the
    // factory transfers that ownership; no thread shares or concurrently uses
    // the raw allocation.
    unsafe impl Send for SecurityDescriptor {}

    impl SecurityDescriptor {
        fn from_owner(owner: &UserSid) -> io::Result<Self> {
            let sddl = format!("D:P(A;;GA;;;{})", owner.as_str());
            let wide = wide_null(&sddl);
            let mut descriptor = ptr::null_mut();
            if unsafe {
                ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    wide.as_ptr(),
                    SDDL_REVISION_1,
                    &mut descriptor,
                    ptr::null_mut(),
                )
            } == 0
            {
                return Err(io::Error::last_os_error());
            }
            Ok(Self {
                descriptor,
                attributes: SECURITY_ATTRIBUTES {
                    nLength: size_of::<SECURITY_ATTRIBUTES>() as u32,
                    lpSecurityDescriptor: descriptor,
                    bInheritHandle: 0,
                },
                #[cfg(test)]
                security_sddl: sddl,
            })
        }
    }

    impl Drop for SecurityDescriptor {
        fn drop(&mut self) {
            if !self.descriptor.is_null() {
                let _ = unsafe { LocalFree(self.descriptor) };
            }
        }
    }

    pub fn connect_async(endpoint: &Endpoint) -> io::Result<NamedPipeClient> {
        ClientOptions::new().open(endpoint.pipe_name())
    }

    pub fn acquire_lock(path: &Path) -> io::Result<File> {
        match open_lock(path, true) {
            Ok(file) => Ok(file),
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                // A live wmux lock is share-mode-zero and this open fails. A
                // stale file has no owner, can be opened exclusively, and is
                // atomically scheduled for deletion with this new ownership
                // handle. No connection failure is treated as stale evidence.
                open_lock(path, false).map_err(|error| {
                    if matches!(error.raw_os_error(), Some(5 | 32)) {
                        io::Error::new(
                            io::ErrorKind::AlreadyExists,
                            "wmux clean server lock is held",
                        )
                    } else {
                        error
                    }
                })
            }
            Err(error) => Err(error),
        }
    }

    fn open_lock(path: &Path, create_new: bool) -> io::Result<File> {
        let mut options = OpenOptions::new();
        options
            .write(true)
            .create_new(create_new)
            .access_mode(GENERIC_WRITE | DELETE)
            .share_mode(0)
            .custom_flags(FILE_FLAG_DELETE_ON_CLOSE);
        options.open(path)
    }

    pub fn connect(endpoint: &Endpoint) -> io::Result<File> {
        let name = wide_null(endpoint.pipe_name());
        let handle = unsafe {
            CreateFileW(
                name.as_ptr(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                ptr::null_mut(),
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            Err(io::Error::last_os_error())
        } else {
            Ok(unsafe { File::from_raw_handle(handle.cast()) })
        }
    }

    pub fn verify_client(server: &NamedPipeServer, expected: &UserSid) -> io::Result<()> {
        let guard = ImpersonationGuard::begin(server)?;
        let actual = (|| {
            let mut token = ptr::null_mut();
            if unsafe { OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, 1, &mut token) } == 0 {
                return Err(io::Error::last_os_error());
            }
            let token = OwnedHandle(token);
            UserSid::from_token(token.0)
        })();
        let reverted = guard.revert();
        let actual = actual.map_err(permission_denied)?;
        reverted.map_err(permission_denied)?;
        if actual == *expected {
            Ok(())
        } else {
            Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                "named-pipe client SID does not match endpoint owner",
            ))
        }
    }

    struct ImpersonationGuard {
        active: bool,
    }

    impl ImpersonationGuard {
        fn begin(server: &NamedPipeServer) -> io::Result<Self> {
            if unsafe { ImpersonateNamedPipeClient(server.as_raw_handle().cast()) } == 0 {
                return Err(permission_denied(io::Error::last_os_error()));
            }
            Ok(Self { active: true })
        }

        fn revert(mut self) -> io::Result<()> {
            if unsafe { RevertToSelf() } == 0 {
                return Err(io::Error::last_os_error());
            }
            self.active = false;
            Ok(())
        }
    }

    impl Drop for ImpersonationGuard {
        fn drop(&mut self) {
            if self.active {
                let _ = unsafe { RevertToSelf() };
            }
        }
    }

    fn permission_denied(error: io::Error) -> io::Error {
        io::Error::new(io::ErrorKind::PermissionDenied, error)
    }

    fn wide_null(value: &str) -> Vec<u16> {
        OsStr::new(value)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }
}

#[cfg(not(windows))]
mod imp {
    use super::{Endpoint, UserSid};
    use std::{fs::File, io, path::Path};

    impl UserSid {
        pub(super) fn current_process() -> io::Result<Self> {
            Err(io::Error::new(io::ErrorKind::Unsupported, "Windows only"))
        }
    }

    pub fn connect(_endpoint: &Endpoint) -> io::Result<File> {
        Err(io::Error::new(io::ErrorKind::Unsupported, "Windows only"))
    }

    pub fn acquire_lock(_path: &Path) -> io::Result<File> {
        Err(io::Error::new(io::ErrorKind::Unsupported, "Windows only"))
    }
}

#[cfg(windows)]
pub use imp::ServerPipeFactory;
#[cfg(windows)]
pub use tokio::net::windows::named_pipe::{NamedPipeClient, NamedPipeServer};

#[cfg(windows)]
pub fn connect_async(endpoint: &Endpoint) -> io::Result<NamedPipeClient> {
    imp::connect_async(endpoint)
}

pub fn connect(endpoint: &Endpoint) -> io::Result<File> {
    imp::connect(endpoint)
}

#[cfg(windows)]
pub fn verify_client(server: &NamedPipeServer, expected: &UserSid) -> io::Result<()> {
    imp::verify_client(server, expected)
}

#[cfg(test)]
mod tests {
    use super::{connect_async, Endpoint, ServerLock, ServerPipeFactory, UserSid};
    use std::sync::{Mutex, OnceLock};
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    #[test]
    fn endpoint_current_user_uses_a_windows_sid() {
        let endpoint = Endpoint::current_user().unwrap();
        assert!(endpoint.owner_sid().as_str().starts_with("S-1-"));
        assert!(endpoint.pipe_name().contains(endpoint.owner_sid().as_str()));
    }

    #[test]
    fn endpoint_current_user_ignores_attacker_controlled_username() {
        let before = Endpoint::current_user().unwrap().pipe_name().to_owned();
        with_username_env("attacker-controlled", || {
            assert_eq!(Endpoint::current_user().unwrap().pipe_name(), before);
        });
    }

    #[test]
    fn endpoint_instance_suffix_is_sanitized_and_has_a_unique_lock_path() {
        let endpoint = Endpoint::for_instance("one/two:three").unwrap();
        assert!(endpoint.pipe_name().contains("one_two_three-"));
        assert!(endpoint.lock_path.file_name().is_some_and(|name| {
            let name = name.to_string_lossy();
            name.starts_with("wmux-clean-lock-") && name.ends_with(".lock")
        }));
    }

    #[test]
    fn endpoint_instance_encoding_distinguishes_previously_colliding_values() {
        // Mutation caught: collapsing distinct WMUX_INSTANCE values into the
        // same endpoint and server-lock namespace.
        let slash = Endpoint::for_instance("a/b").unwrap();
        let colon = Endpoint::for_instance("a:b").unwrap();
        assert_ne!(slash.pipe_name(), colon.pipe_name());
        assert_ne!(slash.lock_path, colon.lock_path);
    }

    #[test]
    fn endpoint_bounds_maximal_sid_names_without_losing_instance_identity() {
        // Mutation caught: budgeting the instance suffix alone and allowing a
        // maximal valid SID to overflow the pipe or lock-file limits.
        let owner = maximal_sid_for_test();
        let first_instance = "x".repeat(1_000);
        let second_instance = format!("{}y", "x".repeat(999));
        let first = Endpoint::from_owner_sid(owner.clone(), Some(&first_instance)).unwrap();
        let second = Endpoint::from_owner_sid(owner, Some(&second_instance)).unwrap();

        assert!(first.pipe_name().len() <= 240);
        assert!(first
            .lock_path
            .file_name()
            .is_some_and(|name| name.to_string_lossy().len() <= 255));
        assert!(first.lock_path.to_string_lossy().encode_utf16().count() < 260);
        assert_ne!(first.pipe_name(), second.pipe_name());
        assert_ne!(first.lock_path, second.lock_path);
    }

    #[test]
    fn server_lock_recovers_a_stale_lock_file() {
        let endpoint = Endpoint::for_instance(&unique("stale-lock")).unwrap();
        std::fs::write(&endpoint.lock_path, b"stale").unwrap();
        let lock = ServerLock::acquire(&endpoint).unwrap();
        assert!(endpoint.lock_path.exists());
        drop(lock);
        assert!(!endpoint.lock_path.exists());
    }

    #[test]
    fn server_lock_rejects_a_second_live_owner() {
        // Mutation caught: treating any failed pipe connection as a stale lock
        // and deleting an active server's marker.
        let endpoint = Endpoint::for_instance(&unique("live-lock")).unwrap();
        let first = ServerLock::acquire(&endpoint).unwrap();
        let error = ServerLock::acquire(&endpoint).unwrap_err();
        assert_eq!(error.kind(), std::io::ErrorKind::AlreadyExists);
        assert!(endpoint.lock_path.exists());
        drop(first);
        assert!(!endpoint.lock_path.exists());
    }

    #[test]
    fn server_lock_prevents_replacement_while_its_owner_is_live() {
        // Mutation caught: allowing an active lock pathname to be replaced, so
        // its later Drop can unlink another owner's lock.
        let endpoint = Endpoint::for_instance(&unique("lock-replacement")).unwrap();
        let lock = ServerLock::acquire(&endpoint).unwrap();
        assert!(std::fs::remove_file(&endpoint.lock_path).is_err());
        drop(lock);
        std::fs::write(&endpoint.lock_path, b"replacement").unwrap();
        assert!(endpoint.lock_path.exists());
        std::fs::remove_file(&endpoint.lock_path).unwrap();
    }

    #[test]
    fn factory_sddl_grants_only_the_owner_sid() {
        let endpoint = Endpoint::for_instance(&unique("sddl")).unwrap();
        let factory = ServerPipeFactory::new(endpoint.clone()).unwrap();
        assert_eq!(
            factory.sddl(),
            format!("D:P(A;;GA;;;{})", endpoint.owner_sid().as_str())
        );
        assert!(!factory.sddl().contains("WD"));
        assert!(!factory.sddl().contains("AN"));
    }

    #[tokio::test]
    async fn factory_rejects_a_preexisting_first_pipe_instance() {
        let endpoint = Endpoint::for_instance(&unique("first-instance")).unwrap();
        let _competing = tokio::net::windows::named_pipe::ServerOptions::new()
            .create(endpoint.pipe_name())
            .unwrap();
        assert!(ServerPipeFactory::new(endpoint).unwrap().create().is_err());
    }

    #[tokio::test]
    async fn authenticated_pipe_is_full_duplex_and_rejects_a_different_sid() {
        let endpoint = Endpoint::for_instance(&unique("authenticated-pipe")).unwrap();
        let mut factory = ServerPipeFactory::new(endpoint.clone()).unwrap();
        let mut server = factory.create().unwrap();
        let mut client = connect_async(&endpoint).unwrap();
        server.connect().await.unwrap();
        super::verify_client(&server, endpoint.owner_sid()).unwrap();
        assert!(
            super::verify_client(&server, &UserSid::well_known_local_system_for_test()).is_err()
        );

        client.write_all(b"client").await.unwrap();
        let mut from_client = [0_u8; 6];
        server.read_exact(&mut from_client).await.unwrap();
        assert_eq!(&from_client, b"client");

        server.write_all(b"server").await.unwrap();
        let mut from_server = [0_u8; 6];
        client.read_exact(&mut from_server).await.unwrap();
        assert_eq!(&from_server, b"server");
    }

    fn unique(label: &str) -> String {
        format!(
            "{label}-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        )
    }

    fn maximal_sid_for_test() -> UserSid {
        let subauthorities = std::iter::repeat_n("4294967295", 15)
            .collect::<Vec<_>>()
            .join("-");
        UserSid {
            bytes: vec![0; 68],
            text: format!("S-1-281474976710655-{subauthorities}"),
        }
    }

    fn with_username_env<T>(value: &str, action: impl FnOnce() -> T) -> T {
        static ENV_LOCK: OnceLock<Mutex<()>> = OnceLock::new();
        let _guard = ENV_LOCK.get_or_init(|| Mutex::new(())).lock().unwrap();
        let previous_username = std::env::var_os("USERNAME");
        let previous_user = std::env::var_os("USER");
        unsafe {
            std::env::set_var("USERNAME", value);
            std::env::set_var("USER", value);
        }
        let result = action();
        unsafe {
            match previous_username {
                Some(previous) => std::env::set_var("USERNAME", previous),
                None => std::env::remove_var("USERNAME"),
            }
            match previous_user {
                Some(previous) => std::env::set_var("USER", previous),
                None => std::env::remove_var("USER"),
            }
        }
        result
    }
}
