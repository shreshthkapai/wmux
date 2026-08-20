use std::{
    env,
    fs::{File, OpenOptions},
    io,
    path::PathBuf,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Endpoint {
    pipe_name: String,
    lock_path: PathBuf,
}

impl Endpoint {
    pub fn current_user() -> Self {
        let user = env::var("USERNAME")
            .or_else(|_| env::var("USER"))
            .unwrap_or_else(|_| "unknown".to_string());
        let user = sanitize(&user);
        Self {
            pipe_name: format!(r"\\.\pipe\wmux-clean-{user}"),
            lock_path: env::temp_dir().join(format!("wmux-clean-{user}.lock")),
        }
    }

    pub fn pipe_name(&self) -> &str {
        &self.pipe_name
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
                    let _ = std::fs::remove_file(&endpoint.lock_path);
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
        "unknown".to_string()
    } else {
        out
    }
}

#[cfg(windows)]
mod imp {
    use super::Endpoint;
    use std::{
        ffi::c_void,
        fs::File,
        io,
        os::windows::{ffi::OsStrExt, io::FromRawHandle},
        ptr,
    };
    use tokio::net::windows::named_pipe::{
        ClientOptions, NamedPipeClient, NamedPipeServer, ServerOptions,
    };

    type Handle = *mut c_void;
    type Dword = u32;
    const INVALID_HANDLE_VALUE: Handle = !0_usize as Handle;
    const GENERIC_READ: Dword = 0x8000_0000;
    const GENERIC_WRITE: Dword = 0x4000_0000;
    const OPEN_EXISTING: Dword = 3;
    const FILE_ATTRIBUTE_NORMAL: Dword = 0x0000_0080;
    #[link(name = "kernel32")]
    extern "system" {
        fn CreateFileW(
            lpFileName: *const u16,
            dwDesiredAccess: Dword,
            dwShareMode: Dword,
            lpSecurityAttributes: *mut c_void,
            dwCreationDisposition: Dword,
            dwFlagsAndAttributes: Dword,
            hTemplateFile: Handle,
        ) -> Handle;
    }

    pub fn create_server(endpoint: &Endpoint) -> io::Result<NamedPipeServer> {
        ServerOptions::new().create(endpoint.pipe_name())
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

    fn wide_null(value: &str) -> Vec<u16> {
        std::ffi::OsStr::new(value)
            .encode_wide()
            .chain(std::iter::once(0))
            .collect()
    }
}

#[cfg(not(windows))]
mod imp {
    use super::Endpoint;
    use std::{fs::File, io};

    pub fn connect(_endpoint: &Endpoint) -> io::Result<File> {
        Err(io::Error::new(io::ErrorKind::Unsupported, "Windows only"))
    }
}

#[cfg(windows)]
pub use tokio::net::windows::named_pipe::{NamedPipeClient, NamedPipeServer};

#[cfg(windows)]
pub fn create_server(endpoint: &Endpoint) -> io::Result<NamedPipeServer> {
    imp::create_server(endpoint)
}

#[cfg(windows)]
pub fn connect_async(endpoint: &Endpoint) -> io::Result<NamedPipeClient> {
    imp::connect_async(endpoint)
}

pub fn connect(endpoint: &Endpoint) -> io::Result<File> {
    imp::connect(endpoint)
}

#[cfg(test)]
mod tests {
    use super::{create_server, imp, Endpoint};
    use tokio::io::{AsyncReadExt, AsyncWriteExt};

    #[test]
    fn endpoint_is_user_scoped() {
        assert!(Endpoint::current_user()
            .pipe_name()
            .starts_with(r"\\.\pipe\wmux-clean-"));
    }

    #[tokio::test]
    async fn overlapped_pipe_is_full_duplex() {
        let unique = format!(
            "wmux-async-test-{}-{:?}",
            std::process::id(),
            std::thread::current().id()
        );
        let endpoint = Endpoint {
            pipe_name: format!(r"\\.\pipe\{unique}"),
            lock_path: std::env::temp_dir().join(format!("{unique}.lock")),
        };
        let mut server = create_server(&endpoint).unwrap();
        let mut client = imp::connect_async(&endpoint).unwrap();
        server.connect().await.unwrap();

        client.write_all(b"client").await.unwrap();
        let mut from_client = [0_u8; 6];
        server.read_exact(&mut from_client).await.unwrap();
        assert_eq!(&from_client, b"client");

        server.write_all(b"server").await.unwrap();
        let mut from_server = [0_u8; 6];
        client.read_exact(&mut from_server).await.unwrap();
        assert_eq!(&from_server, b"server");
    }
}
