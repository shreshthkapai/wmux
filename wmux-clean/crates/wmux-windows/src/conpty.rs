use std::{
    collections::BTreeMap,
    ffi::c_void,
    fs::File,
    io::{self, Write},
    mem,
    os::windows::{ffi::OsStrExt, io::FromRawHandle},
    ptr,
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc, Arc,
    },
    thread,
};
use tokio::{
    io::AsyncReadExt,
    net::windows::named_pipe::NamedPipeServer,
    runtime::Handle as TokioHandle,
    sync::{mpsc as async_mpsc, watch},
};
use wmux_platform::{PlatformError, PlatformEvent, PlatformPaneId, TerminalSize};

/// At most one MiB of unread 16 KiB chunks may accumulate per pane.
/// A full queue blocks only that pane's reader, preserving terminal bytes and
/// applying backpressure without allowing a noisy pane to consume all memory.
pub const OUTPUT_QUEUE_CHUNKS: usize = 64;
const OUTPUT_CHUNK_BYTES: usize = 16 * 1024;
static PIPE_SEQUENCE: AtomicU64 = AtomicU64::new(1);

pub type PlatformEventReceiver = async_mpsc::Receiver<PlatformEvent>;
pub type PlatformNotifier = Arc<dyn Fn(PlatformPaneId) + Send + Sync>;

#[derive(Debug)]
pub struct ConptyPane {
    process_id: u32,
    input: Option<mpsc::Sender<Vec<u8>>>,
    closing: watch::Sender<bool>,
    pseudo_console: Option<PseudoConsole>,
    _pty_input_read: OwnedHandle,
    job: OwnedHandle,
    terminated: bool,
}

unsafe impl Send for ConptyPane {}

impl ConptyPane {
    pub fn process_id(&self) -> u32 {
        self.process_id
    }

    pub fn write_input(&self, bytes: Vec<u8>) -> io::Result<()> {
        self.input
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::BrokenPipe, "ConPTY input is closed"))?
            .send(bytes)
            .map_err(|_| io::Error::new(io::ErrorKind::BrokenPipe, "ConPTY input writer stopped"))
    }

    pub fn resize(&mut self, size: TerminalSize) -> io::Result<()> {
        let pseudo_console = self
            .pseudo_console
            .as_ref()
            .ok_or_else(|| io::Error::new(io::ErrorKind::BrokenPipe, "ConPTY is closed"))?;
        resize_pseudo_console(pseudo_console.raw, size)
    }

    pub fn terminate(&mut self, exit_code: u32) {
        if self.terminated {
            return;
        }
        self.terminated = true;
        self.input.take();
        let _ = self.closing.send(true);
        let _ = unsafe { TerminateJobObject(self.job.raw, exit_code) };
        self.pseudo_console.take();
    }

    /// Close the server-owned ConPTY endpoints after the process waiter has
    /// observed exit. The output reader remains in forwarding mode so bytes
    /// already buffered by ConPTY are delivered before EOF closes its sender.
    pub fn finish_after_process_exit(&mut self) {
        self.input.take();
        self.pseudo_console.take();
    }
}

impl Drop for ConptyPane {
    fn drop(&mut self) {
        self.terminate(1);
    }
}

pub fn spawn_shell(
    pane: PlatformPaneId,
    size: TerminalSize,
    env_overrides: &[(String, String)],
    runtime: &TokioHandle,
    notify: PlatformNotifier,
) -> io::Result<(ConptyPane, PlatformEventReceiver)> {
    let (pty_input_read, input_write) = create_pipe_pair()?;
    let (output_read, pty_output_write) = create_overlapped_output_pipe(pane)?;
    set_handle_inherit(input_write.raw, false)?;

    let pseudo_console = create_pseudo_console(size, pty_input_read.raw, pty_output_write.raw)?;

    let mut startup = StartupInfoEx::new()?;
    startup.attach_pseudo_console(pseudo_console.raw)?;
    startup.set_invalid_standard_handles();

    let mut command_line = default_shell_command_line();
    let mut environment = environment_block(env_overrides);
    let mut process_info = ProcessInformation::default();
    let created = unsafe {
        CreateProcessW(
            ptr::null(),
            command_line.as_mut_ptr(),
            ptr::null_mut(),
            ptr::null_mut(),
            0,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
            environment.as_mut_ptr().cast(),
            ptr::null(),
            &mut startup.inner.startup_info,
            &mut process_info,
        )
    };
    if created == 0 {
        return Err(io::Error::last_os_error());
    }

    let process_handle = OwnedHandle::new(process_info.process)?;
    let thread_handle = OwnedHandle::new(process_info.thread)?;

    let job = create_kill_on_close_job()?;
    assign_process_to_job(&job, process_handle.raw)?;
    drop(pty_output_write);

    let resumed = unsafe { ResumeThread(thread_handle.raw) };
    if resumed == u32::MAX {
        unsafe {
            TerminateProcess(process_handle.raw, 1);
        }
        return Err(io::Error::last_os_error());
    }
    drop(thread_handle);

    let (tx, rx) = async_mpsc::channel(OUTPUT_QUEUE_CHUNKS);
    let (closing, closing_rx) = watch::channel(false);
    spawn_output_reader(
        pane,
        output_read,
        tx.clone(),
        closing_rx,
        runtime,
        notify.clone(),
    );
    spawn_process_waiter(pane, process_handle, tx, notify);
    let (input_tx, input_rx) = mpsc::channel();
    spawn_input_writer(pane, input_write.into_file(), input_rx);

    Ok((
        ConptyPane {
            process_id: process_info.process_id,
            input: Some(input_tx),
            closing,
            pseudo_console: Some(pseudo_console),
            _pty_input_read: pty_input_read,
            job,
            terminated: false,
        },
        rx,
    ))
}

fn default_shell_command_line() -> Vec<u16> {
    let shell = std::env::var("WMUX_SHELL")
        .or_else(|_| std::env::var("ComSpec"))
        .unwrap_or_else(|_| "cmd.exe".to_string());
    wide_null(&shell)
}

fn environment_block(overrides: &[(String, String)]) -> Vec<u16> {
    let mut env = BTreeMap::new();
    for (key, value) in std::env::vars_os() {
        let key = key.to_string_lossy().to_string();
        if key.is_empty() || key.contains('=') || key.contains('\0') {
            continue;
        }
        env.insert(
            key.to_uppercase(),
            (key, value.to_string_lossy().to_string()),
        );
    }
    for (key, value) in overrides {
        if key.is_empty() || key.contains('=') || key.contains('\0') {
            continue;
        }
        env.insert(key.to_uppercase(), (key.clone(), value.clone()));
    }

    let mut block = Vec::new();
    for (_, (key, value)) in env {
        block.extend(std::ffi::OsStr::new(&format!("{key}={value}")).encode_wide());
        block.push(0);
    }
    block.push(0);
    block
}

fn spawn_output_reader(
    pane: PlatformPaneId,
    output: OwnedHandle,
    tx: async_mpsc::Sender<PlatformEvent>,
    mut closing: watch::Receiver<bool>,
    runtime: &TokioHandle,
    notify: PlatformNotifier,
) {
    runtime.spawn(async move {
        let raw = output.into_raw();
        let mut output = match unsafe { NamedPipeServer::from_raw_handle(raw.cast()) } {
            Ok(output) => output,
            Err(error) => {
                eprintln!(
                    "conpty output IOCP registration error for pane {}: {error}",
                    pane.raw()
                );
                emit_platform_event(
                    &tx,
                    &notify,
                    PlatformEvent::BackendError {
                        pane,
                        error: PlatformError::from_io("register ConPTY output", error),
                    },
                )
                .await;
                return;
            }
        };
        let mut buffer = [0_u8; OUTPUT_CHUNK_BYTES];
        let mut drain_only = false;
        loop {
            let read = if drain_only {
                output.read(&mut buffer).await
            } else {
                tokio::select! {
                    changed = closing.changed() => {
                        if changed.is_err() || *closing.borrow() {
                            drain_only = true;
                        }
                        continue;
                    }
                    read = output.read(&mut buffer) => read,
                }
            };
            match read {
                Ok(0) => {
                    break;
                }
                Ok(n) => {
                    if drain_only {
                        continue;
                    }
                    let event = PlatformEvent::PtyOutput {
                        pane,
                        bytes: buffer[..n].to_vec(),
                    };
                    tokio::select! {
                        result = tx.send(event) => {
                            if result.is_err() {
                                break;
                            }
                            notify(pane);
                        }
                        changed = closing.changed() => {
                            if changed.is_err() || *closing.borrow() {
                                drain_only = true;
                            }
                        }
                    }
                }
                Err(error) => {
                    if !drain_only {
                        eprintln!(
                            "conpty output reader error for pane {}: {error}",
                            pane.raw()
                        );
                        emit_platform_event(
                            &tx,
                            &notify,
                            PlatformEvent::BackendError {
                                pane,
                                error: PlatformError::from_io("read ConPTY output", error),
                            },
                        )
                        .await;
                    }
                    break;
                }
            }
        }
    });
}

async fn emit_platform_event(
    tx: &async_mpsc::Sender<PlatformEvent>,
    notify: &PlatformNotifier,
    event: PlatformEvent,
) {
    let pane = match &event {
        PlatformEvent::PtyOutput { pane, .. }
        | PlatformEvent::PtyExited { pane, .. }
        | PlatformEvent::PtyClosed { pane }
        | PlatformEvent::BackendError { pane, .. } => *pane,
    };
    if tx.send(event).await.is_ok() {
        notify(pane);
    }
}

fn spawn_input_writer(pane: PlatformPaneId, mut input: File, rx: mpsc::Receiver<Vec<u8>>) {
    thread::spawn(move || {
        while let Ok(bytes) = rx.recv() {
            if let Err(error) = input.write_all(&bytes).and_then(|_| input.flush()) {
                eprintln!("conpty input writer error for pane {}: {error}", pane.raw());
                break;
            }
        }
    });
}

fn spawn_process_waiter(
    pane: PlatformPaneId,
    process: OwnedHandle,
    tx: async_mpsc::Sender<PlatformEvent>,
    notify: PlatformNotifier,
) {
    let process = process.into_raw() as usize;
    thread::spawn(move || {
        let process = process as Handle;
        unsafe {
            WaitForSingleObject(process, INFINITE);
        }
        let mut exit_code = 1;
        unsafe {
            GetExitCodeProcess(process, &mut exit_code);
            CloseHandle(process);
        }
        if tx
            .blocking_send(PlatformEvent::PtyExited {
                pane,
                exit_code: Some(exit_code),
            })
            .is_ok()
        {
            notify(pane);
        }
    });
}

fn create_overlapped_output_pipe(pane: PlatformPaneId) -> io::Result<(OwnedHandle, OwnedHandle)> {
    let sequence = PIPE_SEQUENCE.fetch_add(1, Ordering::Relaxed);
    let name = format!(
        r"\\.\pipe\wmux-conpty-{}-{}-{sequence}",
        std::process::id(),
        pane.raw()
    );
    let wide_name = wide_null(&name);
    let server = unsafe {
        CreateNamedPipeW(
            wide_name.as_ptr(),
            PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_WAIT,
            1,
            0,
            64 * 1024,
            0,
            ptr::null_mut(),
        )
    };
    let server = OwnedHandle::new(server)?;
    let client = unsafe {
        CreateFileW(
            wide_name.as_ptr(),
            GENERIC_WRITE,
            0,
            ptr::null_mut(),
            OPEN_EXISTING,
            0,
            ptr::null_mut(),
        )
    };
    let client = OwnedHandle::new(client)?;
    Ok((server, client))
}

fn create_pipe_pair() -> io::Result<(OwnedHandle, OwnedHandle)> {
    let mut read = ptr::null_mut();
    let mut write = ptr::null_mut();
    let mut attributes = SecurityAttributes {
        length: mem::size_of::<SecurityAttributes>() as Dword,
        security_descriptor: ptr::null_mut(),
        inherit_handle: 1,
    };
    let ok = unsafe {
        CreatePipe(
            &mut read,
            &mut write,
            (&mut attributes as *mut SecurityAttributes).cast(),
            0,
        )
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok((OwnedHandle::new(read)?, OwnedHandle::new(write)?))
}

fn set_handle_inherit(handle: Handle, inherit: bool) -> io::Result<()> {
    let flags = if inherit { HANDLE_FLAG_INHERIT } else { 0 };
    let ok = unsafe { SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn create_pseudo_console(
    size: TerminalSize,
    input: Handle,
    output: Handle,
) -> io::Result<PseudoConsole> {
    let mut raw = ptr::null_mut();
    let hr = unsafe { CreatePseudoConsole(coord_from_size(size), input, output, 0, &mut raw) };
    if failed(hr) {
        Err(io::Error::from_raw_os_error(hr))
    } else {
        Ok(PseudoConsole { raw })
    }
}

fn resize_pseudo_console(raw: Handle, size: TerminalSize) -> io::Result<()> {
    let hr = unsafe { ResizePseudoConsole(raw, coord_from_size(size)) };
    if failed(hr) {
        Err(io::Error::from_raw_os_error(hr))
    } else {
        Ok(())
    }
}

fn coord_from_size(size: TerminalSize) -> Coord {
    Coord {
        x: size.cols.max(1) as i16,
        y: size.rows.max(1) as i16,
    }
}

fn create_kill_on_close_job() -> io::Result<OwnedHandle> {
    let raw = unsafe { CreateJobObjectW(ptr::null_mut(), ptr::null()) };
    let job = OwnedHandle::new(raw)?;

    let mut limits = JobObjectExtendedLimitInformation::default();
    limits.basic_limit_information.limit_flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    let ok = unsafe {
        SetInformationJobObject(
            job.raw,
            JOB_OBJECT_INFO_CLASS_EXTENDED_LIMIT_INFORMATION,
            &mut limits as *mut _ as *mut c_void,
            mem::size_of::<JobObjectExtendedLimitInformation>() as u32,
        )
    };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(job)
}

fn assign_process_to_job(job: &OwnedHandle, process: Handle) -> io::Result<()> {
    let ok = unsafe { AssignProcessToJobObject(job.raw, process) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn wide_null(value: &str) -> Vec<u16> {
    std::ffi::OsStr::new(value)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn failed(hr: i32) -> bool {
    hr < 0
}

impl OwnedHandle {
    fn into_file(self) -> File {
        let raw = self.into_raw();
        unsafe { File::from_raw_handle(raw.cast()) }
    }
}

#[derive(Debug)]
struct OwnedHandle {
    raw: Handle,
}

unsafe impl Send for OwnedHandle {}

impl OwnedHandle {
    fn new(raw: Handle) -> io::Result<Self> {
        if raw.is_null() || raw == INVALID_HANDLE_VALUE {
            Err(io::Error::last_os_error())
        } else {
            Ok(Self { raw })
        }
    }

    fn into_raw(self) -> Handle {
        let raw = self.raw;
        mem::forget(self);
        raw
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.raw);
        }
    }
}

#[derive(Debug)]
struct PseudoConsole {
    raw: Handle,
}

unsafe impl Send for PseudoConsole {}

impl Drop for PseudoConsole {
    fn drop(&mut self) {
        unsafe {
            ClosePseudoConsole(self.raw);
        }
    }
}

struct StartupInfoEx {
    inner: StartupInfoExW,
    attribute_storage: Vec<usize>,
}

impl StartupInfoEx {
    fn new() -> io::Result<Self> {
        let mut size = 0_usize;
        unsafe {
            InitializeProcThreadAttributeList(ptr::null_mut(), 1, 0, &mut size);
        }
        if size == 0 {
            return Err(io::Error::last_os_error());
        }

        let word_len = size.div_ceil(mem::size_of::<usize>());
        let mut attribute_storage = vec![0_usize; word_len];
        let attribute_list = attribute_storage.as_mut_ptr().cast();
        let ok = unsafe { InitializeProcThreadAttributeList(attribute_list, 1, 0, &mut size) };
        if ok == 0 {
            return Err(io::Error::last_os_error());
        }

        let mut inner = StartupInfoExW::default();
        inner.startup_info.cb = mem::size_of::<StartupInfoExW>() as u32;
        inner.attribute_list = attribute_list;

        Ok(Self {
            inner,
            attribute_storage,
        })
    }

    fn attach_pseudo_console(&mut self, pseudo_console: Handle) -> io::Result<()> {
        let ok = unsafe {
            UpdateProcThreadAttribute(
                self.inner.attribute_list,
                0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                pseudo_console,
                mem::size_of::<Handle>(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        };
        if ok == 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok(())
        }
    }

    fn set_invalid_standard_handles(&mut self) {
        self.inner.startup_info.flags |= STARTF_USESTDHANDLES;
        self.inner.startup_info.std_input = INVALID_HANDLE_VALUE;
        self.inner.startup_info.std_output = INVALID_HANDLE_VALUE;
        self.inner.startup_info.std_error = INVALID_HANDLE_VALUE;
    }
}

impl Drop for StartupInfoEx {
    fn drop(&mut self) {
        unsafe {
            DeleteProcThreadAttributeList(self.inner.attribute_list);
        }
        let _ = self.attribute_storage.len();
    }
}

type Handle = *mut c_void;
type Bool = i32;
type Dword = u32;
type Hresult = i32;
type JobObjectInfoClass = i32;

const INVALID_HANDLE_VALUE: Handle = !0_usize as Handle;
const GENERIC_WRITE: Dword = 0x4000_0000;
const OPEN_EXISTING: Dword = 3;
const PIPE_ACCESS_INBOUND: Dword = 0x0000_0001;
const PIPE_TYPE_BYTE: Dword = 0x0000_0000;
const PIPE_WAIT: Dword = 0x0000_0000;
const FILE_FLAG_OVERLAPPED: Dword = 0x4000_0000;
const FILE_FLAG_FIRST_PIPE_INSTANCE: Dword = 0x0008_0000;
const EXTENDED_STARTUPINFO_PRESENT: Dword = 0x0008_0000;
const CREATE_SUSPENDED: Dword = 0x0000_0004;
const CREATE_UNICODE_ENVIRONMENT: Dword = 0x0000_0400;
const STARTF_USESTDHANDLES: Dword = 0x0000_0100;
const HANDLE_FLAG_INHERIT: Dword = 0x0000_0001;
const PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE: usize = 0x0002_0016;
const INFINITE: Dword = 0xffff_ffff;
const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: Dword = 0x0000_2000;
const JOB_OBJECT_INFO_CLASS_EXTENDED_LIMIT_INFORMATION: JobObjectInfoClass = 9;
#[cfg(test)]
const SYNCHRONIZE: Dword = 0x0010_0000;
#[cfg(test)]
const WAIT_TIMEOUT: Dword = 258;

#[repr(C)]
#[derive(Clone, Copy)]
struct Coord {
    x: i16,
    y: i16,
}

#[repr(C)]
#[derive(Default)]
struct ProcessInformation {
    process: Handle,
    thread: Handle,
    process_id: Dword,
    thread_id: Dword,
}

#[repr(C)]
#[derive(Default)]
struct StartupInfoW {
    cb: Dword,
    reserved: *mut u16,
    desktop: *mut u16,
    title: *mut u16,
    x: Dword,
    y: Dword,
    x_size: Dword,
    y_size: Dword,
    x_count_chars: Dword,
    y_count_chars: Dword,
    fill_attribute: Dword,
    flags: Dword,
    show_window: u16,
    reserved2_count: u16,
    reserved2: *mut u8,
    std_input: Handle,
    std_output: Handle,
    std_error: Handle,
}

#[repr(C)]
struct SecurityAttributes {
    length: Dword,
    security_descriptor: *mut c_void,
    inherit_handle: Bool,
}

#[repr(C)]
#[derive(Default)]
struct StartupInfoExW {
    startup_info: StartupInfoW,
    attribute_list: *mut c_void,
}

#[repr(C)]
#[derive(Default)]
struct IoCounters {
    read_operation_count: u64,
    write_operation_count: u64,
    other_operation_count: u64,
    read_transfer_count: u64,
    write_transfer_count: u64,
    other_transfer_count: u64,
}

#[repr(C)]
#[derive(Default)]
struct JobObjectBasicLimitInformation {
    per_process_user_time_limit: i64,
    per_job_user_time_limit: i64,
    limit_flags: Dword,
    minimum_working_set_size: usize,
    maximum_working_set_size: usize,
    active_process_limit: Dword,
    affinity: usize,
    priority_class: Dword,
    scheduling_class: Dword,
}

#[repr(C)]
#[derive(Default)]
struct JobObjectExtendedLimitInformation {
    basic_limit_information: JobObjectBasicLimitInformation,
    io_info: IoCounters,
    process_memory_limit: usize,
    job_memory_limit: usize,
    peak_process_memory_used: usize,
    peak_job_memory_used: usize,
}

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
    fn CreateFileW(
        lpFileName: *const u16,
        dwDesiredAccess: Dword,
        dwShareMode: Dword,
        lpSecurityAttributes: *mut c_void,
        dwCreationDisposition: Dword,
        dwFlagsAndAttributes: Dword,
        hTemplateFile: Handle,
    ) -> Handle;
    fn CreatePipe(
        hReadPipe: *mut Handle,
        hWritePipe: *mut Handle,
        lpPipeAttributes: *mut c_void,
        nSize: Dword,
    ) -> Bool;
    fn SetHandleInformation(hObject: Handle, dwMask: Dword, dwFlags: Dword) -> Bool;
    fn CreatePseudoConsole(
        size: Coord,
        hInput: Handle,
        hOutput: Handle,
        dwFlags: Dword,
        phPC: *mut Handle,
    ) -> Hresult;
    fn ResizePseudoConsole(hPC: Handle, size: Coord) -> Hresult;
    fn ClosePseudoConsole(hPC: Handle);
    fn InitializeProcThreadAttributeList(
        lpAttributeList: *mut c_void,
        dwAttributeCount: Dword,
        dwFlags: Dword,
        lpSize: *mut usize,
    ) -> Bool;
    fn UpdateProcThreadAttribute(
        lpAttributeList: *mut c_void,
        dwFlags: Dword,
        attribute: usize,
        lpValue: *mut c_void,
        cbSize: usize,
        lpPreviousValue: *mut c_void,
        lpReturnSize: *mut usize,
    ) -> Bool;
    fn DeleteProcThreadAttributeList(lpAttributeList: *mut c_void);
    fn CreateProcessW(
        lpApplicationName: *const u16,
        lpCommandLine: *mut u16,
        lpProcessAttributes: *mut c_void,
        lpThreadAttributes: *mut c_void,
        bInheritHandles: Bool,
        dwCreationFlags: Dword,
        lpEnvironment: *mut c_void,
        lpCurrentDirectory: *const u16,
        lpStartupInfo: *mut StartupInfoW,
        lpProcessInformation: *mut ProcessInformation,
    ) -> Bool;
    fn CreateJobObjectW(lpJobAttributes: *mut c_void, lpName: *const u16) -> Handle;
    fn SetInformationJobObject(
        hJob: Handle,
        jobObjectInformationClass: JobObjectInfoClass,
        lpJobObjectInformation: *mut c_void,
        cbJobObjectInformationLength: Dword,
    ) -> Bool;
    fn AssignProcessToJobObject(hJob: Handle, hProcess: Handle) -> Bool;
    fn TerminateJobObject(hJob: Handle, uExitCode: u32) -> Bool;
    fn TerminateProcess(hProcess: Handle, uExitCode: u32) -> Bool;
    fn ResumeThread(hThread: Handle) -> Dword;
    fn WaitForSingleObject(hHandle: Handle, dwMilliseconds: Dword) -> Dword;
    fn GetExitCodeProcess(hProcess: Handle, lpExitCode: *mut Dword) -> Bool;
    #[cfg(test)]
    fn OpenProcess(dwDesiredAccess: Dword, bInheritHandle: Bool, dwProcessId: Dword) -> Handle;
    fn CloseHandle(hObject: Handle) -> Bool;
}

#[cfg(test)]
mod tests {
    use super::{
        environment_block, spawn_shell, CloseHandle, OpenProcess, PlatformNotifier,
        WaitForSingleObject, SYNCHRONIZE, WAIT_TIMEOUT,
    };
    use std::sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    };
    use std::time::Duration;
    use tokio::runtime::Builder;
    use wmux_platform::{PlatformEvent, PlatformPaneId, TerminalSize};

    #[test]
    fn environment_block_applies_overrides_and_ends_with_double_nul() {
        let block = environment_block(&[
            ("TERM".to_string(), "tmux-256color".to_string()),
            ("WMUX_PANE".to_string(), "7".to_string()),
        ]);
        let rendered = String::from_utf16_lossy(&block);

        assert!(rendered.contains("TERM=tmux-256color\0"));
        assert!(rendered.contains("WMUX_PANE=7\0"));
        assert_eq!(block.last(), Some(&0));
        assert_eq!(block.get(block.len() - 2), Some(&0));
    }

    #[test]
    fn conpty_output_roundtrips_through_iocp() {
        let runtime = Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .unwrap();
        let notifications = Arc::new(AtomicUsize::new(0));
        let notification_count = Arc::clone(&notifications);
        let notify: PlatformNotifier = Arc::new(move |_| {
            notification_count.fetch_add(1, Ordering::Relaxed);
        });
        let pane_id = PlatformPaneId::new(99_001);
        let (mut pane, mut events) = spawn_shell(
            pane_id,
            TerminalSize::new(80, 24),
            &[],
            runtime.handle(),
            notify,
        )
        .unwrap();
        pane.write_input(b"echo WMUX_IOCP_ROUNDTRIP\r".to_vec())
            .unwrap();

        let output = runtime.block_on(async {
            tokio::time::timeout(Duration::from_secs(5), async {
                let mut output = Vec::new();
                while let Some(event) = events.recv().await {
                    if let PlatformEvent::PtyOutput { bytes, .. } = event {
                        output.extend_from_slice(&bytes);
                        if output
                            .windows(b"WMUX_IOCP_ROUNDTRIP".len())
                            .any(|window| window == b"WMUX_IOCP_ROUNDTRIP")
                        {
                            return output;
                        }
                    }
                }
                output
            })
            .await
            .expect("timed out waiting for ConPTY IOCP output")
        });

        pane.terminate(0);
        assert!(output
            .windows(b"WMUX_IOCP_ROUNDTRIP".len())
            .any(|window| window == b"WMUX_IOCP_ROUNDTRIP"));
        assert!(notifications.load(Ordering::Relaxed) > 0);
    }

    #[test]
    fn terminating_a_pane_kills_its_descendant_process_tree() {
        let runtime = Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .unwrap();
        let notify: PlatformNotifier = Arc::new(|_| {});
        let pane_id = PlatformPaneId::new(99_002);
        let (mut pane, mut events) = spawn_shell(
            pane_id,
            TerminalSize::new(120, 30),
            &[],
            runtime.handle(),
            notify,
        )
        .unwrap();
        pane.write_input(
            concat!(
                "powershell -NoProfile -Command \"",
                "$p=Start-Process powershell -ArgumentList '-NoProfile','-Command',",
                "'Start-Sleep -Seconds 60' -PassThru; ",
                "Write-Output ('WMUX_CHILD_' + $p.Id); Wait-Process -Id $p.Id\"\r"
            )
            .as_bytes()
            .to_vec(),
        )
        .unwrap();

        let child = runtime.block_on(async {
            tokio::time::timeout(Duration::from_secs(10), async {
                let mut output = Vec::new();
                while let Some(event) = events.recv().await {
                    if let PlatformEvent::PtyOutput { bytes, .. } = event {
                        output.extend_from_slice(&bytes);
                        if let Some(pid) = marker_pid(&output, b"WMUX_CHILD_") {
                            return pid;
                        }
                    }
                }
                panic!("ConPTY closed before child pid was reported")
            })
            .await
            .expect("timed out waiting for descendant process id")
        });
        assert!(process_is_running(child));

        pane.terminate(99);
        pane.terminate(99);
        assert!(pane.write_input(b"ignored".to_vec()).is_err());
        assert!(pane.resize(TerminalSize::new(80, 24)).is_err());
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while process_is_running(child) && std::time::Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(20));
        }
        assert!(
            !process_is_running(child),
            "descendant process {child} survived pane termination"
        );
    }

    #[test]
    fn process_exit_and_conpty_eof_close_the_event_stream_once() {
        let runtime = Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .unwrap();
        let notify: PlatformNotifier = Arc::new(|_| {});
        let pane_id = PlatformPaneId::new(99_003);
        let (mut pane, mut events) = spawn_shell(
            pane_id,
            TerminalSize::new(80, 24),
            &[],
            runtime.handle(),
            notify,
        )
        .unwrap();
        pane.write_input(b"exit 23\r".to_vec()).unwrap();

        let exits = runtime.block_on(async {
            tokio::time::timeout(Duration::from_secs(5), async {
                let mut exits = Vec::new();
                while let Some(event) = events.recv().await {
                    if let PlatformEvent::PtyExited { exit_code, .. } = event {
                        exits.push(exit_code);
                        pane.finish_after_process_exit();
                    }
                }
                exits
            })
            .await
            .expect("ConPTY event stream remained open after process exit and EOF")
        });

        assert_eq!(exits, vec![Some(23)]);
    }

    fn marker_pid(bytes: &[u8], marker: &[u8]) -> Option<u32> {
        for offset in 0..=bytes.len().saturating_sub(marker.len()) {
            if bytes.get(offset..offset + marker.len()) != Some(marker) {
                continue;
            }
            let digits = bytes[offset + marker.len()..]
                .iter()
                .copied()
                .take_while(u8::is_ascii_digit)
                .collect::<Vec<_>>();
            if !digits.is_empty() {
                return std::str::from_utf8(&digits).ok()?.parse().ok();
            }
        }
        None
    }

    fn process_is_running(pid: u32) -> bool {
        let process = unsafe { OpenProcess(SYNCHRONIZE, 0, pid) };
        if process.is_null() {
            return false;
        }
        let status = unsafe { WaitForSingleObject(process, 0) };
        unsafe {
            CloseHandle(process);
        }
        status == WAIT_TIMEOUT
    }
}
