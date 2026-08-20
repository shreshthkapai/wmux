//! Windows named-pipe IPC backend.

use std::{
    env,
    fs::{File, OpenOptions},
    io,
    path::PathBuf,
};

#[derive(Debug, Default)]
pub struct NamedPipeBackend;

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct NamedPipeEndpoint {
    pipe_name: String,
    lock_path: PathBuf,
}

impl NamedPipeEndpoint {
    pub fn default_for_current_user() -> Self {
        let user = env::var("USERNAME")
            .or_else(|_| env::var("USER"))
            .unwrap_or_else(|_| "unknown".to_string());
        let user = sanitize_endpoint_part(&user);
        Self {
            pipe_name: format!(r"\\.\pipe\wmux-{user}"),
            lock_path: env::temp_dir().join(format!("wmux-{user}.lock")),
        }
    }

    pub fn pipe_name(&self) -> &str {
        &self.pipe_name
    }

    pub fn lock_path(&self) -> &PathBuf {
        &self.lock_path
    }

    pub fn remove_stale_lock(&self) -> io::Result<()> {
        match std::fs::remove_file(&self.lock_path) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(error),
        }
    }
}

#[derive(Debug)]
pub struct ServerLock {
    path: PathBuf,
    _file: File,
}

impl ServerLock {
    pub fn acquire(endpoint: &NamedPipeEndpoint) -> io::Result<Self> {
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
                        "wmux server is already running",
                    ))
                } else {
                    endpoint.remove_stale_lock()?;
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

    pub fn remove_now(&self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

impl Drop for ServerLock {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.path);
    }
}

pub fn is_server_running(endpoint: &NamedPipeEndpoint) -> bool {
    connect(endpoint).is_ok()
}

fn sanitize_endpoint_part(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
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
    use super::NamedPipeEndpoint;
    use std::{
        ffi::c_void,
        fs::File,
        io,
        os::windows::{
            ffi::OsStrExt,
            io::{AsRawHandle, FromRawHandle},
        },
        ptr,
    };

    type Handle = *mut c_void;
    type Dword = u32;
    type Bool = i32;

    const INVALID_HANDLE_VALUE: Handle = !0_usize as Handle;

    const GENERIC_READ: Dword = 0x8000_0000;
    const GENERIC_WRITE: Dword = 0x4000_0000;
    const OPEN_EXISTING: Dword = 3;
    const FILE_ATTRIBUTE_NORMAL: Dword = 0x0000_0080;

    const PIPE_ACCESS_DUPLEX: Dword = 0x0000_0003;
    const PIPE_TYPE_BYTE: Dword = 0x0000_0000;
    const PIPE_READMODE_BYTE: Dword = 0x0000_0000;
    const PIPE_WAIT: Dword = 0x0000_0000;
    const PIPE_UNLIMITED_INSTANCES: Dword = 255;

    const ERROR_PIPE_CONNECTED: Dword = 535;

    #[link(name = "kernel32")]
    extern "system" {
        fn CreateNamedPipeW(
            lpName: *const u16,
            dwOpenMode: Dword,
            dwPipeMode: Dword,
            nMaxInstances: Dword,
            nOutBufferSize: Dword,
            nInBufferSize: Dword,
            nDefaultTimeOut: Dword,
            lpSecurityAttributes: *mut c_void,
        ) -> Handle;

        fn ConnectNamedPipe(hNamedPipe: Handle, lpOverlapped: *mut c_void) -> Bool;

        fn CreateFileW(
            lpFileName: *const u16,
            dwDesiredAccess: Dword,
            dwShareMode: Dword,
            lpSecurityAttributes: *mut c_void,
            dwCreationDisposition: Dword,
            dwFlagsAndAttributes: Dword,
            hTemplateFile: Handle,
        ) -> Handle;

        fn GetLastError() -> Dword;
        fn CloseHandle(hObject: Handle) -> Bool;
        fn PeekNamedPipe(
            hNamedPipe: Handle,
            lpBuffer: *mut c_void,
            nBufferSize: Dword,
            lpBytesRead: *mut Dword,
            lpTotalBytesAvail: *mut Dword,
            lpBytesLeftThisMessage: *mut Dword,
        ) -> Bool;
    }

    pub fn accept(endpoint: &NamedPipeEndpoint) -> io::Result<File> {
        let name = wide_null(endpoint.pipe_name());
        let handle = unsafe {
            CreateNamedPipeW(
                name.as_ptr(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                64 * 1024,
                64 * 1024,
                0,
                ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }

        let connected = unsafe { ConnectNamedPipe(handle, ptr::null_mut()) };
        if connected == 0 {
            let error = unsafe { GetLastError() };
            if error != ERROR_PIPE_CONNECTED {
                unsafe {
                    CloseHandle(handle);
                }
                return Err(io::Error::last_os_error());
            }
        }

        let file = unsafe { File::from_raw_handle(handle.cast()) };
        Ok(file)
    }

    pub fn connect(endpoint: &NamedPipeEndpoint) -> io::Result<File> {
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

    pub fn has_pending_bytes(pipe: &File) -> io::Result<bool> {
        let mut available = 0;
        let ok = unsafe {
            PeekNamedPipe(
                pipe.as_raw_handle().cast(),
                ptr::null_mut(),
                0,
                ptr::null_mut(),
                &mut available,
                ptr::null_mut(),
            )
        };
        if ok == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(available > 0)
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
    use super::NamedPipeEndpoint;
    use std::{fs::File, io};

    pub fn accept(_endpoint: &NamedPipeEndpoint) -> io::Result<File> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "Windows named pipes are only available on Windows",
        ))
    }

    pub fn connect(_endpoint: &NamedPipeEndpoint) -> io::Result<File> {
        Err(io::Error::new(
            io::ErrorKind::Unsupported,
            "Windows named pipes are only available on Windows",
        ))
    }

    pub fn has_pending_bytes(_pipe: &File) -> io::Result<bool> {
        Ok(false)
    }
}

pub fn accept(endpoint: &NamedPipeEndpoint) -> io::Result<File> {
    imp::accept(endpoint)
}

pub fn connect(endpoint: &NamedPipeEndpoint) -> io::Result<File> {
    imp::connect(endpoint)
}

pub fn has_pending_bytes(pipe: &File) -> io::Result<bool> {
    imp::has_pending_bytes(pipe)
}

#[cfg(test)]
mod tests {
    use super::NamedPipeEndpoint;

    #[test]
    fn default_endpoint_is_user_scoped() {
        let endpoint = NamedPipeEndpoint::default_for_current_user();
        assert!(endpoint.pipe_name().starts_with(r"\\.\pipe\wmux-"));
        assert!(
            endpoint.lock_path().ends_with("wmux-unknown.lock")
                || endpoint.lock_path().exists()
                || endpoint.lock_path().to_string_lossy().contains("wmux-")
        );
    }
}
