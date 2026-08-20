//! Windows ConPTY pane backend.
//!
//! The server owns these objects. Core state only sees pane ids and semantic
//! events; raw Windows handles stay in this platform crate.

use mux_core::PaneId;
use mux_platform::terminal::TerminalSize;
use std::{
    ffi::c_void,
    fs::File,
    io::{self, Read, Write},
    mem,
    os::windows::{ffi::OsStrExt, io::FromRawHandle},
    ptr,
    sync::mpsc,
    thread,
};

#[derive(Debug, Default)]
pub struct ConptyBackend;

#[derive(Debug)]
pub struct ConptyPane {
    pane_id: PaneId,
    process_id: u32,
    input: File,
    pseudo_console: PseudoConsole,
    _pty_input_read: OwnedHandle,
    job: OwnedHandle,
}

impl ConptyPane {
    pub fn pane_id(&self) -> PaneId {
        self.pane_id
    }

    pub fn process_id(&self) -> u32 {
        self.process_id
    }

    pub fn write_input(&mut self, bytes: &[u8]) -> io::Result<()> {
        self.input.write_all(bytes)
    }

    pub fn resize(&mut self, size: TerminalSize) -> io::Result<()> {
        resize_pseudo_console(self.pseudo_console.raw, size)
    }

    pub fn terminate(&mut self, exit_code: u32) {
        let _ = unsafe { TerminateJobObject(self.job.raw, exit_code) };
    }
}

impl Drop for ConptyPane {
    fn drop(&mut self) {
        let _ = unsafe { TerminateJobObject(self.job.raw, 1) };
    }
}

#[derive(Debug)]
pub enum ConptyEvent {
    Output { pane_id: PaneId, bytes: Vec<u8> },
    Exited { pane_id: PaneId, exit_code: u32 },
    Closed { pane_id: PaneId },
    Error { pane_id: PaneId, message: String },
}

pub fn spawn_shell_pane(
    pane_id: PaneId,
    size: TerminalSize,
    event_tx: mpsc::Sender<ConptyEvent>,
) -> io::Result<ConptyPane> {
    let (pty_input_read, input_write) = create_pipe_pair()?;
    let (output_read, pty_output_write) = create_pipe_pair()?;
    set_handle_inherit(input_write.raw, false)?;
    set_handle_inherit(output_read.raw, false)?;

    let pseudo_console = create_pseudo_console(size, pty_input_read.raw, pty_output_write.raw)?;

    let mut startup = StartupInfoEx::new()?;
    startup.attach_pseudo_console(pseudo_console.raw)?;
    startup.set_output_handles(pty_output_write.raw, pty_output_write.raw);

    let mut command_line = default_shell_command_line();
    let mut process_info = ProcessInformation::default();
    let created = unsafe {
        CreateProcessW(
            ptr::null(),
            command_line.as_mut_ptr(),
            ptr::null_mut(),
            ptr::null_mut(),
            1,
            EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
            ptr::null_mut(),
            ptr::null(),
            &mut startup.inner.StartupInfo,
            &mut process_info,
        )
    };
    if created == 0 {
        return Err(io::Error::last_os_error());
    }

    let process_handle = OwnedHandle::new(process_info.hProcess)?;
    let thread_handle = OwnedHandle::new(process_info.hThread)?;

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

    spawn_output_reader(pane_id, output_read.into_file(), event_tx.clone());
    spawn_process_waiter(pane_id, process_handle, event_tx);

    Ok(ConptyPane {
        pane_id,
        process_id: process_info.dwProcessId,
        input: input_write.into_file(),
        pseudo_console,
        _pty_input_read: pty_input_read,
        job,
    })
}

fn default_shell_command_line() -> Vec<u16> {
    let shell = std::env::var("WMUX_SHELL").unwrap_or_else(|_| "cmd.exe".to_string());
    wide_null(&shell)
}

fn spawn_output_reader(pane_id: PaneId, mut output: File, event_tx: mpsc::Sender<ConptyEvent>) {
    thread::spawn(move || {
        let mut buffer = [0_u8; 8192];
        loop {
            match output.read(&mut buffer) {
                Ok(0) => {
                    let _ = event_tx.send(ConptyEvent::Closed { pane_id });
                    break;
                }
                Ok(n) => {
                    let _ = event_tx.send(ConptyEvent::Output {
                        pane_id,
                        bytes: buffer[..n].to_vec(),
                    });
                }
                Err(error) => {
                    let _ = event_tx.send(ConptyEvent::Error {
                        pane_id,
                        message: error.to_string(),
                    });
                    break;
                }
            }
        }
    });
}

fn spawn_process_waiter(
    pane_id: PaneId,
    process: OwnedHandle,
    event_tx: mpsc::Sender<ConptyEvent>,
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
        let _ = event_tx.send(ConptyEvent::Exited { pane_id, exit_code });
    });
}

fn create_pipe_pair() -> io::Result<(OwnedHandle, OwnedHandle)> {
    let mut read = ptr::null_mut();
    let mut write = ptr::null_mut();
    let mut attributes = SecurityAttributes {
        nLength: mem::size_of::<SecurityAttributes>() as Dword,
        lpSecurityDescriptor: ptr::null_mut(),
        bInheritHandle: 1,
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
    let hr = unsafe {
        CreatePseudoConsole(
            coord_from_size(size),
            input,
            output,
            0,
            &mut raw as *mut Handle,
        )
    };
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
        x: size.columns.max(1) as i16,
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

#[derive(Debug)]
struct PipeHandle {
    raw: OwnedHandle,
}

impl PipeHandle {
    fn into_file(self) -> File {
        let raw = self.raw.into_raw();
        unsafe { File::from_raw_handle(raw.cast()) }
    }
}

impl From<OwnedHandle> for PipeHandle {
    fn from(raw: OwnedHandle) -> Self {
        Self { raw }
    }
}

impl OwnedHandle {
    fn into_file(self) -> File {
        PipeHandle::from(self).into_file()
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
        inner.StartupInfo.cb = mem::size_of::<StartupInfoExW>() as u32;
        inner.lpAttributeList = attribute_list;

        Ok(Self {
            inner,
            attribute_storage,
        })
    }

    fn attach_pseudo_console(&mut self, pseudo_console: Handle) -> io::Result<()> {
        let ok = unsafe {
            UpdateProcThreadAttribute(
                self.inner.lpAttributeList,
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

    fn set_output_handles(&mut self, output: Handle, error: Handle) {
        self.inner.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        self.inner.StartupInfo.hStdInput = ptr::null_mut();
        self.inner.StartupInfo.hStdOutput = output;
        self.inner.StartupInfo.hStdError = error;
    }
}

impl Drop for StartupInfoEx {
    fn drop(&mut self) {
        unsafe {
            DeleteProcThreadAttributeList(self.inner.lpAttributeList);
        }
        let _ = self.attribute_storage.len();
    }
}

type Handle = *mut c_void;
type Bool = i32;
type Dword = u32;
type Hresult = i32;

const INVALID_HANDLE_VALUE: Handle = !0_usize as Handle;
const EXTENDED_STARTUPINFO_PRESENT: Dword = 0x0008_0000;
const CREATE_SUSPENDED: Dword = 0x0000_0004;
const CREATE_UNICODE_ENVIRONMENT: Dword = 0x0000_0400;
const STARTF_USESTDHANDLES: Dword = 0x0000_0100;
const HANDLE_FLAG_INHERIT: Dword = 0x0000_0001;
const PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE: usize = 0x0002_0016;
const INFINITE: Dword = 0xffff_ffff;
const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: Dword = 0x0000_2000;
const JOB_OBJECT_INFO_CLASS_EXTENDED_LIMIT_INFORMATION: JobObjectInfoClass = 9;

#[repr(C)]
#[derive(Clone, Copy)]
struct Coord {
    x: i16,
    y: i16,
}

#[repr(C)]
#[derive(Default)]
#[allow(non_snake_case)]
struct ProcessInformation {
    hProcess: Handle,
    hThread: Handle,
    dwProcessId: Dword,
    dwThreadId: Dword,
}

#[repr(C)]
#[derive(Default)]
#[allow(non_snake_case)]
struct StartupInfoW {
    cb: Dword,
    lpReserved: *mut u16,
    lpDesktop: *mut u16,
    lpTitle: *mut u16,
    dwX: Dword,
    dwY: Dword,
    dwXSize: Dword,
    dwYSize: Dword,
    dwXCountChars: Dword,
    dwYCountChars: Dword,
    dwFillAttribute: Dword,
    dwFlags: Dword,
    wShowWindow: u16,
    cbReserved2: u16,
    lpReserved2: *mut u8,
    hStdInput: Handle,
    hStdOutput: Handle,
    hStdError: Handle,
}

#[repr(C)]
#[allow(non_snake_case)]
struct SecurityAttributes {
    nLength: Dword,
    lpSecurityDescriptor: *mut c_void,
    bInheritHandle: Bool,
}

#[repr(C)]
#[derive(Default)]
#[allow(non_snake_case)]
struct StartupInfoExW {
    StartupInfo: StartupInfoW,
    lpAttributeList: *mut c_void,
}

type JobObjectInfoClass = i32;

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
    fn CloseHandle(hObject: Handle) -> Bool;
}
