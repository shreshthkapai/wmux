use std::{
    collections::BTreeMap,
    ffi::{c_void, OsString},
    fs::File,
    io::{self, Read},
    mem,
    os::windows::{ffi::OsStrExt, io::FromRawHandle},
    ptr,
    sync::mpsc::{self, Receiver, SyncSender, TryRecvError},
    thread,
};
use windows_sys::Win32::System::Threading::{
    CreateProcessW, DeleteProcThreadAttributeList, GetExitCodeProcess,
    InitializeProcThreadAttributeList, ResumeThread, TerminateProcess, UpdateProcThreadAttribute,
    WaitForSingleObject,
};
use wmux_platform::{
    JobBackend, JobEvent, JobNotifier, JobRequest, PlatformError, PlatformErrorKind, PlatformJobId,
    PlatformResult, SpawnJob,
};

const JOB_EVENT_QUEUE_CHUNKS: usize = 64;
const OUTPUT_CHUNK_BYTES: usize = 16 * 1024;
const CREATE_NEW_PROCESS_GROUP: u32 = 0x0000_0200;
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const CREATE_SUSPENDED: u32 = 0x0000_0004;
const CREATE_UNICODE_ENVIRONMENT: u32 = 0x0000_0400;
const EXTENDED_STARTUPINFO_PRESENT: u32 = 0x0008_0000;
const STARTF_USESTDHANDLES: u32 = 0x0000_0100;
const HANDLE_FLAG_INHERIT: u32 = 0x0000_0001;
const DUPLICATE_SAME_ACCESS: u32 = 0x0000_0002;
const JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE: u32 = 0x0000_2000;
const JOB_OBJECT_EXTENDED_LIMIT_INFORMATION: i32 = 9;
const PROC_THREAD_ATTRIBUTE_HANDLE_LIST: usize = 0x0002_0002;
const INFINITE: u32 = 0xffff_ffff;
const INVALID_HANDLE_VALUE: Handle = !0_usize as Handle;

type Handle = *mut c_void;

struct WindowsJob {
    job_object: OwnedHandle,
    events: Receiver<JobEvent>,
    exited: bool,
    pending_close: bool,
}

pub(crate) struct WindowsJobBackend {
    notifier: JobNotifier,
    jobs: BTreeMap<PlatformJobId, WindowsJob>,
}

impl WindowsJobBackend {
    pub(crate) fn new(notifier: JobNotifier) -> Self {
        Self {
            notifier,
            jobs: BTreeMap::new(),
        }
    }

    fn spawn(&mut self, request: SpawnJob) -> PlatformResult<()> {
        if self.jobs.contains_key(&request.job) {
            return Err(PlatformError::new(
                PlatformErrorKind::AlreadyRunning,
                "spawn shell job",
                format!("platform job {} already exists", request.job.raw()),
            ));
        }

        let shell = std::env::var_os("ComSpec").unwrap_or_else(|| "cmd.exe".into());
        let (output_read, output_write) = create_pipe_pair()
            .map_err(|error| PlatformError::from_io("create shell job output pipe", error))?;
        set_handle_inherit(output_read.raw, false)
            .map_err(|error| PlatformError::from_io("protect shell job output pipe", error))?;
        let error_write = duplicate_inheritable(output_write.raw)
            .map_err(|error| PlatformError::from_io("duplicate shell job output pipe", error))?;
        let job_object = create_kill_on_close_job()
            .map_err(|error| PlatformError::from_io("create shell job object", error))?;

        let mut startup = StartupInfoEx::new(&[output_write.raw, error_write.raw])
            .map_err(|error| PlatformError::from_io("configure shell job handles", error))?;
        startup.inner.startup_info.flags = STARTF_USESTDHANDLES;
        startup.inner.startup_info.std_input = INVALID_HANDLE_VALUE;
        startup.inner.startup_info.std_output = output_write.raw;
        startup.inner.startup_info.std_error = error_write.raw;
        let application = wide_null(&shell);
        let mut command_line = wide_null(OsString::from(format!(
            "wmux-job.exe /D /S /C {}",
            request.command
        )));
        let mut environment = environment_block(&request.environment);
        let current_directory = request.cwd.as_ref().map(|path| wide_null(path.as_os_str()));
        let current_directory_ptr = current_directory
            .as_ref()
            .map_or(ptr::null(), |path| path.as_ptr());
        let mut process_info = ProcessInformation::default();
        let created = unsafe {
            CreateProcessW(
                application.as_ptr(),
                command_line.as_mut_ptr(),
                ptr::null_mut(),
                ptr::null_mut(),
                1,
                CREATE_NO_WINDOW
                    | CREATE_NEW_PROCESS_GROUP
                    | CREATE_SUSPENDED
                    | CREATE_UNICODE_ENVIRONMENT
                    | EXTENDED_STARTUPINFO_PRESENT,
                environment.as_mut_ptr().cast(),
                current_directory_ptr,
                (&raw const startup.inner.startup_info).cast(),
                (&raw mut process_info).cast(),
            )
        };
        if created == 0 {
            return Err(PlatformError::from_io(
                "spawn shell job",
                io::Error::last_os_error(),
            ));
        }
        let process_handle = OwnedHandle::new(process_info.process)
            .map_err(|error| PlatformError::from_io("own shell job process", error))?;
        let thread_handle = OwnedHandle::new(process_info.thread)
            .map_err(|error| PlatformError::from_io("own shell job thread", error))?;
        drop(output_write);
        drop(error_write);
        if let Err(error) = assign_process_to_job(&job_object, process_handle.raw) {
            unsafe {
                TerminateProcess(process_handle.raw, 1);
            }
            return Err(PlatformError::from_io(
                "assign shell job process tree",
                error,
            ));
        }
        if unsafe { ResumeThread(thread_handle.raw) } == u32::MAX {
            unsafe {
                TerminateProcess(process_handle.raw, 1);
            }
            return Err(PlatformError::from_io(
                "resume shell job",
                io::Error::last_os_error(),
            ));
        }
        drop(thread_handle);

        let (tx, rx) = mpsc::sync_channel(JOB_EVENT_QUEUE_CHUNKS);
        spawn_worker(
            request.job,
            output_read.into_file(),
            process_handle,
            tx,
            self.notifier.clone(),
        );
        self.jobs.insert(
            request.job,
            WindowsJob {
                job_object,
                events: rx,
                exited: false,
                pending_close: false,
            },
        );
        Ok(())
    }

    fn job_mut(&mut self, job: PlatformJobId) -> PlatformResult<&mut WindowsJob> {
        self.jobs.get_mut(&job).ok_or_else(|| {
            PlatformError::new(
                PlatformErrorKind::NotFound,
                "access shell job",
                format!("platform job {} does not exist", job.raw()),
            )
        })
    }
}

impl JobBackend for WindowsJobBackend {
    fn submit(&mut self, request: JobRequest) -> PlatformResult<()> {
        match request {
            JobRequest::Spawn(request) => self.spawn(request),
            JobRequest::Terminate { job } => {
                let state = self.job_mut(job)?;
                if unsafe { TerminateJobObject(state.job_object.raw, 1) } == 0 {
                    return Err(PlatformError::from_io(
                        "terminate shell job",
                        io::Error::last_os_error(),
                    ));
                }
                Ok(())
            }
        }
    }

    fn try_next_event(&mut self, job: PlatformJobId) -> PlatformResult<Option<JobEvent>> {
        if self.jobs.get(&job).is_some_and(|state| state.pending_close) {
            self.jobs.remove(&job);
            return Ok(Some(JobEvent::Closed { job }));
        }
        loop {
            let state = self.job_mut(job)?;
            match state.events.try_recv() {
                Ok(JobEvent::Exited { exit_code, .. }) if !state.exited => {
                    state.exited = true;
                    return Ok(Some(JobEvent::Exited { job, exit_code }));
                }
                Ok(JobEvent::Exited { .. }) => continue,
                Ok(JobEvent::Output { bytes, .. }) => {
                    return Ok(Some(JobEvent::Output { job, bytes }))
                }
                Ok(JobEvent::BackendError { error, .. }) => {
                    return Ok(Some(JobEvent::BackendError { job, error }))
                }
                Ok(JobEvent::Closed { .. }) => continue,
                Err(TryRecvError::Empty) => return Ok(None),
                Err(TryRecvError::Disconnected) if !state.exited => {
                    state.exited = true;
                    state.pending_close = true;
                    return Ok(Some(JobEvent::Exited {
                        job,
                        exit_code: None,
                    }));
                }
                Err(TryRecvError::Disconnected) => {
                    self.jobs.remove(&job);
                    return Ok(Some(JobEvent::Closed { job }));
                }
            }
        }
    }
}

fn spawn_worker(
    job: PlatformJobId,
    mut output: File,
    process: OwnedHandle,
    tx: SyncSender<JobEvent>,
    notifier: JobNotifier,
) {
    thread::spawn(move || {
        let mut buffer = vec![0; OUTPUT_CHUNK_BYTES];
        loop {
            match output.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => {
                    if tx
                        .send(JobEvent::Output {
                            job,
                            bytes: buffer[..count].to_vec(),
                        })
                        .is_err()
                    {
                        process.terminate(1);
                        return;
                    }
                    notifier(job);
                }
                Err(error) if error.kind() == io::ErrorKind::Interrupted => continue,
                Err(error) => {
                    let _ = tx.send(JobEvent::BackendError {
                        job,
                        error: PlatformError::from_io("read shell job output", error),
                    });
                    notifier(job);
                    break;
                }
            }
        }
        let exit_code = process.wait_exit();
        if tx.send(JobEvent::Exited { job, exit_code }).is_ok() {
            notifier(job);
        }
    });
}

fn wide_null(value: impl AsRef<std::ffi::OsStr>) -> Vec<u16> {
    value
        .as_ref()
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

fn environment_block(overrides: &[(OsString, OsString)]) -> Vec<u16> {
    let mut environment = BTreeMap::<String, (OsString, OsString)>::new();
    for (key, value) in std::env::vars_os().chain(overrides.iter().cloned()) {
        let normalized = key.to_string_lossy().to_uppercase();
        if normalized.is_empty() || normalized.contains('=') || normalized.contains('\0') {
            continue;
        }
        environment.insert(normalized, (key, value));
    }
    let mut block = Vec::new();
    for (_, (key, value)) in environment {
        block.extend(key.encode_wide());
        block.push('=' as u16);
        block.extend(value.encode_wide());
        block.push(0);
    }
    block.push(0);
    block
}

struct StartupInfoEx {
    inner: StartupInfoExW,
    attribute_storage: Vec<usize>,
    handles: Vec<Handle>,
}

impl StartupInfoEx {
    fn new(handles: &[Handle]) -> io::Result<Self> {
        let mut size = 0;
        unsafe {
            InitializeProcThreadAttributeList(ptr::null_mut(), 1, 0, &mut size);
        }
        if size == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut attribute_storage = vec![0_usize; size.div_ceil(mem::size_of::<usize>())];
        let attribute_list = attribute_storage.as_mut_ptr().cast();
        if unsafe { InitializeProcThreadAttributeList(attribute_list, 1, 0, &mut size) } == 0 {
            return Err(io::Error::last_os_error());
        }
        let mut handles = handles.to_vec();
        let mut inner = StartupInfoExW::default();
        inner.startup_info.cb = mem::size_of::<StartupInfoExW>() as u32;
        inner.attribute_list = attribute_list;
        if unsafe {
            UpdateProcThreadAttribute(
                attribute_list,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles.as_mut_ptr().cast(),
                handles.len() * mem::size_of::<Handle>(),
                ptr::null_mut(),
                ptr::null_mut(),
            )
        } == 0
        {
            unsafe {
                DeleteProcThreadAttributeList(attribute_list);
            }
            return Err(io::Error::last_os_error());
        }
        Ok(Self {
            inner,
            attribute_storage,
            handles,
        })
    }
}

impl Drop for StartupInfoEx {
    fn drop(&mut self) {
        unsafe {
            DeleteProcThreadAttributeList(self.inner.attribute_list);
        }
        let _ = (self.attribute_storage.len(), self.handles.len());
    }
}

#[derive(Debug)]
struct OwnedHandle {
    raw: Handle,
}

unsafe impl Send for OwnedHandle {}

impl OwnedHandle {
    fn new(raw: Handle) -> io::Result<Self> {
        if raw.is_null() || raw as isize == -1 {
            Err(io::Error::last_os_error())
        } else {
            Ok(Self { raw })
        }
    }

    fn into_file(self) -> File {
        let raw = self.raw;
        mem::forget(self);
        unsafe { File::from_raw_handle(raw.cast()) }
    }

    fn terminate(&self, exit_code: u32) {
        unsafe {
            TerminateProcess(self.raw, exit_code);
        }
    }

    fn wait_exit(&self) -> Option<u32> {
        unsafe {
            WaitForSingleObject(self.raw, INFINITE);
        }
        let mut exit_code = 1;
        if unsafe { GetExitCodeProcess(self.raw, &mut exit_code) } == 0 {
            None
        } else {
            Some(exit_code)
        }
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.raw);
        }
    }
}

fn create_pipe_pair() -> io::Result<(OwnedHandle, OwnedHandle)> {
    let mut read = ptr::null_mut();
    let mut write = ptr::null_mut();
    let mut attributes = SecurityAttributes {
        length: mem::size_of::<SecurityAttributes>() as u32,
        security_descriptor: ptr::null_mut(),
        inherit_handle: 1,
    };
    if unsafe {
        CreatePipe(
            &mut read,
            &mut write,
            (&mut attributes as *mut SecurityAttributes).cast(),
            0,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok((OwnedHandle::new(read)?, OwnedHandle::new(write)?))
}

fn set_handle_inherit(handle: Handle, inherit: bool) -> io::Result<()> {
    let flags = if inherit { HANDLE_FLAG_INHERIT } else { 0 };
    if unsafe { SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags) } == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn duplicate_inheritable(handle: Handle) -> io::Result<OwnedHandle> {
    let process = unsafe { GetCurrentProcess() };
    let mut duplicate = ptr::null_mut();
    if unsafe {
        DuplicateHandle(
            process,
            handle,
            process,
            &mut duplicate,
            0,
            1,
            DUPLICATE_SAME_ACCESS,
        )
    } == 0
    {
        Err(io::Error::last_os_error())
    } else {
        OwnedHandle::new(duplicate)
    }
}

fn create_kill_on_close_job() -> io::Result<OwnedHandle> {
    let job = OwnedHandle::new(unsafe { CreateJobObjectW(ptr::null_mut(), ptr::null()) })?;
    let mut limits = JobObjectExtendedLimitInformation::default();
    limits.basic_limit_information.limit_flags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if unsafe {
        SetInformationJobObject(
            job.raw,
            JOB_OBJECT_EXTENDED_LIMIT_INFORMATION,
            (&mut limits as *mut JobObjectExtendedLimitInformation).cast(),
            mem::size_of::<JobObjectExtendedLimitInformation>() as u32,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    Ok(job)
}

fn assign_process_to_job(job: &OwnedHandle, process: Handle) -> io::Result<()> {
    if unsafe { AssignProcessToJobObject(job.raw, process) } == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

#[repr(C)]
struct SecurityAttributes {
    length: u32,
    security_descriptor: *mut c_void,
    inherit_handle: i32,
}

#[repr(C)]
#[derive(Default)]
struct ProcessInformation {
    process: Handle,
    thread: Handle,
    process_id: u32,
    thread_id: u32,
}

#[repr(C)]
#[derive(Default)]
struct StartupInfoW {
    cb: u32,
    reserved: *mut u16,
    desktop: *mut u16,
    title: *mut u16,
    x: u32,
    y: u32,
    x_size: u32,
    y_size: u32,
    x_count_chars: u32,
    y_count_chars: u32,
    fill_attribute: u32,
    flags: u32,
    show_window: u16,
    reserved2_count: u16,
    reserved2: *mut u8,
    std_input: Handle,
    std_output: Handle,
    std_error: Handle,
}

#[repr(C)]
#[derive(Default)]
struct StartupInfoExW {
    startup_info: StartupInfoW,
    attribute_list: *mut c_void,
}

#[repr(C)]
#[derive(Default)]
struct BasicLimitInformation {
    per_process_user_time_limit: i64,
    per_job_user_time_limit: i64,
    limit_flags: u32,
    minimum_working_set_size: usize,
    maximum_working_set_size: usize,
    active_process_limit: u32,
    affinity: usize,
    priority_class: u32,
    scheduling_class: u32,
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
struct JobObjectExtendedLimitInformation {
    basic_limit_information: BasicLimitInformation,
    io_info: IoCounters,
    process_memory_limit: usize,
    job_memory_limit: usize,
    peak_process_memory_used: usize,
    peak_job_memory_used: usize,
}

#[link(name = "kernel32")]
extern "system" {
    fn CreatePipe(read: *mut Handle, write: *mut Handle, attributes: *mut c_void, size: u32)
        -> i32;
    fn SetHandleInformation(object: Handle, mask: u32, flags: u32) -> i32;
    fn GetCurrentProcess() -> Handle;
    fn DuplicateHandle(
        source_process: Handle,
        source: Handle,
        target_process: Handle,
        target: *mut Handle,
        access: u32,
        inherit: i32,
        options: u32,
    ) -> i32;
    fn CreateJobObjectW(attributes: *mut c_void, name: *const u16) -> Handle;
    fn SetInformationJobObject(
        job: Handle,
        class: i32,
        information: *mut c_void,
        length: u32,
    ) -> i32;
    fn AssignProcessToJobObject(job: Handle, process: Handle) -> i32;
    fn TerminateJobObject(job: Handle, exit_code: u32) -> i32;
    fn CloseHandle(object: Handle) -> i32;
}

#[cfg(test)]
mod tests {
    use super::WindowsJobBackend;
    use std::{
        fs,
        sync::Arc,
        thread,
        time::{Duration, Instant, SystemTime, UNIX_EPOCH},
    };
    use wmux_platform::{JobBackend, JobEvent, JobRequest, PlatformJobId, SpawnJob};

    #[test]
    fn native_job_combines_output_applies_context_and_closes_after_exit() {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let cwd = std::env::temp_dir().join(format!("wmux-job-{}-{nonce}", std::process::id()));
        fs::create_dir(&cwd).unwrap();
        let mut backend = WindowsJobBackend::new(Arc::new(|_| {}));
        let job = PlatformJobId::new(7);
        backend
            .submit(JobRequest::Spawn(SpawnJob {
                job,
                command: "echo %WMUX_JOB_VALUE% & cd & echo stderr 1>&2".to_string(),
                cwd: Some(cwd.clone()),
                environment: vec![("WMUX_JOB_VALUE".into(), "native".into())],
            }))
            .unwrap();

        let deadline = Instant::now() + Duration::from_secs(5);
        let mut output = Vec::new();
        let mut exited = None;
        loop {
            match backend.try_next_event(job).unwrap() {
                Some(JobEvent::Output { bytes, .. }) => output.extend(bytes),
                Some(JobEvent::Exited { exit_code, .. }) => exited = Some(exit_code),
                Some(JobEvent::Closed { .. }) => break,
                Some(JobEvent::BackendError { error, .. }) => panic!("job error: {error}"),
                None => {
                    assert!(Instant::now() < deadline, "job did not close");
                    thread::sleep(Duration::from_millis(5));
                }
            }
        }
        let output = String::from_utf8_lossy(&output).to_lowercase();
        assert_eq!(exited, Some(Some(0)));
        assert!(output.contains("native"));
        assert!(output.contains("stderr"));
        assert!(output.contains(&cwd.to_string_lossy().to_lowercase()));
        fs::remove_dir(cwd).unwrap();
    }
}
