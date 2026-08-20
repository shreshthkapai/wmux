use std::{
    env,
    fs::{File, OpenOptions},
    io,
    path::PathBuf,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UserSid {
    bytes: Vec<u8>,
    text: String,
}

impl UserSid {
    pub fn as_str(&self) -> &str {
        &self.text
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
        let suffix = instance
            .filter(|value| !value.is_empty())
            .map(sanitize)
            .map(|value| format!("-{value}"))
            .unwrap_or_default();
        let name = format!("wmux-clean-{}{suffix}", owner_sid.as_str());
        Ok(Self {
            pipe_name: format!(r"\\.\pipe\{name}"),
            lock_path: env::temp_dir().join(format!("{name}.lock")),
            owner_sid,
        })
    }

    pub fn pipe_name(&self) -> &str {
        &self.pipe_name
    }

    pub fn owner_sid(&self) -> &UserSid {
        &self.owner_sid
    }
}

#[derive(Debug)]
pub struct ServerLock {
    path: PathBuf,
    _file: File,
}

impl ServerLock {
    pub fn acquire(endpoint: &Endpoint) -> io::Result<Self> {
        match OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&endpoint.lock_path)
        {
            Ok(file) => Ok(Self {
                path: endpoint.lock_path.clone(),
                _file: file,
            }),
            Err(error) if error.kind() == io::ErrorKind::AlreadyExists => {
                if connect(endpoint).is_ok() {
                    Err(io::Error::new(
                        io::ErrorKind::AlreadyExists,
                        "wmux clean server is already running",
                    ))
                } else {
                    std::fs::remove_file(&endpoint.lock_path)?;
                    let file = OpenOptions::new()
                        .write(true)
                        .create_new(true)
                        .open(&endpoint.lock_path)?;
                    Ok(Self {
                        path: endpoint.lock_path.clone(),
                        _file: file,
                    })
                }
            }
            Err(error) => Err(error),
        }
    }
}

impl Drop for ServerLock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
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

#[cfg(windows)]
mod imp {
    use super::{Endpoint, UserSid};
    use std::{
        ffi::{c_void, OsStr},
        fs::File,
        io,
        mem::size_of,
        os::windows::{
            ffi::OsStrExt,
            io::{AsRawHandle, FromRawHandle},
        },
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
            let sid_len = unsafe { GetLengthSid(sid) };
            if sid.is_null() || sid_len == 0 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "token has no user SID",
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
    use std::{fs::File, io};

    impl UserSid {
        pub(super) fn current_process() -> io::Result<Self> {
            Err(io::Error::new(io::ErrorKind::Unsupported, "Windows only"))
        }
    }

    pub fn connect(_endpoint: &Endpoint) -> io::Result<File> {
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
        assert!(endpoint.pipe_name().ends_with("one_two_three"));
        assert!(endpoint
            .lock_path
            .file_name()
            .is_some_and(|name| name.to_string_lossy().ends_with("one_two_three.lock")));
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
