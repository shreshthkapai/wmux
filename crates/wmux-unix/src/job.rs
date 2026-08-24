use std::{
    collections::BTreeMap,
    fs::File,
    io::{self, Read},
    os::{fd::FromRawFd, unix::process::CommandExt},
    process::{Command, Stdio},
    sync::mpsc::{self, Receiver, SyncSender, TryRecvError},
    thread,
};
use wmux_platform::{
    JobBackend, JobEvent, JobNotifier, JobRequest, PlatformError, PlatformErrorKind, PlatformJobId,
    PlatformResult, SpawnJob,
};

use crate::process::{child_descriptor_limit, mark_non_stdio_descriptors_close_on_exec};

const JOB_EVENT_QUEUE_CHUNKS: usize = 64;
const OUTPUT_CHUNK_BYTES: usize = 16 * 1024;

struct UnixJob {
    process_group: libc::pid_t,
    events: Receiver<JobEvent>,
    exited: bool,
    pending_close: bool,
}

pub(crate) struct UnixJobBackend {
    notifier: JobNotifier,
    jobs: BTreeMap<PlatformJobId, UnixJob>,
}

impl UnixJobBackend {
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

        let (output_read, output_write) = combined_output_pipe()
            .map_err(|error| PlatformError::from_io("create shell job output pipe", error))?;
        let error_write = output_write
            .try_clone()
            .map_err(|error| PlatformError::from_io("duplicate shell job output pipe", error))?;
        let mut command = Command::new("/bin/sh");
        command.arg("-c").arg(&request.command);
        if let Some(cwd) = &request.cwd {
            command.current_dir(cwd);
        }
        command
            .envs(request.environment.iter().map(|(key, value)| (key, value)))
            .stdin(Stdio::null())
            .stdout(Stdio::from(output_write))
            .stderr(Stdio::from(error_write));
        let descriptor_limit = child_descriptor_limit();
        unsafe {
            command.pre_exec(move || {
                if libc::setpgid(0, 0) == -1 {
                    return Err(io::Error::last_os_error());
                }
                mark_non_stdio_descriptors_close_on_exec(descriptor_limit)
            });
        }
        let child = command
            .spawn()
            .map_err(|error| PlatformError::from_io("spawn shell job", error))?;
        let process_group = child.id() as libc::pid_t;
        let (tx, rx) = mpsc::sync_channel(JOB_EVENT_QUEUE_CHUNKS);
        spawn_reader(request.job, output_read, tx.clone(), self.notifier.clone());
        spawn_waiter(request.job, child, tx, self.notifier.clone());
        self.jobs.insert(
            request.job,
            UnixJob {
                process_group,
                events: rx,
                exited: false,
                pending_close: false,
            },
        );
        Ok(())
    }

    fn job_mut(&mut self, job: PlatformJobId) -> PlatformResult<&mut UnixJob> {
        self.jobs.get_mut(&job).ok_or_else(|| {
            PlatformError::new(
                PlatformErrorKind::NotFound,
                "access shell job",
                format!("platform job {} does not exist", job.raw()),
            )
        })
    }
}

impl JobBackend for UnixJobBackend {
    fn submit(&mut self, request: JobRequest) -> PlatformResult<()> {
        match request {
            JobRequest::Spawn(request) => self.spawn(request),
            JobRequest::Terminate { job } => {
                let process_group = self.job_mut(job)?.process_group;
                signal_group(process_group, libc::SIGKILL)
                    .map_err(|error| PlatformError::from_io("terminate shell job", error))
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

impl Drop for UnixJobBackend {
    fn drop(&mut self) {
        for state in self.jobs.values() {
            let _ = signal_group(state.process_group, libc::SIGKILL);
        }
    }
}

fn spawn_reader(
    job: PlatformJobId,
    mut reader: impl Read + Send + 'static,
    tx: SyncSender<JobEvent>,
    notifier: JobNotifier,
) {
    thread::spawn(move || {
        let mut buffer = vec![0; OUTPUT_CHUNK_BYTES];
        loop {
            match reader.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => {
                    if tx
                        .send(JobEvent::Output {
                            job,
                            bytes: buffer[..count].to_vec(),
                        })
                        .is_err()
                    {
                        break;
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
    });
}

fn spawn_waiter(
    job: PlatformJobId,
    mut child: std::process::Child,
    tx: SyncSender<JobEvent>,
    notifier: JobNotifier,
) {
    thread::spawn(move || {
        let exit_code = child
            .wait()
            .ok()
            .and_then(|status| status.code())
            .map(|code| code as u32);
        if tx.send(JobEvent::Exited { job, exit_code }).is_ok() {
            notifier(job);
        }
    });
}

fn signal_group(process_group: libc::pid_t, signal: libc::c_int) -> io::Result<()> {
    if process_group <= 0 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "process group must be positive",
        ));
    }
    if unsafe { libc::kill(-process_group, signal) } == 0 {
        return Ok(());
    }
    let error = io::Error::last_os_error();
    if error.raw_os_error() == Some(libc::ESRCH) {
        Ok(())
    } else {
        Err(error)
    }
}

fn combined_output_pipe() -> io::Result<(File, File)> {
    let mut descriptors = [-1; 2];
    if unsafe { libc::pipe(descriptors.as_mut_ptr()) } == -1 {
        return Err(io::Error::last_os_error());
    }
    let read = unsafe { File::from_raw_fd(descriptors[0]) };
    let write = unsafe { File::from_raw_fd(descriptors[1]) };
    set_close_on_exec(&read)?;
    set_close_on_exec(&write)?;
    Ok((read, write))
}

fn set_close_on_exec(file: &File) -> io::Result<()> {
    use std::os::fd::AsRawFd;
    let descriptor = file.as_raw_fd();
    let flags = unsafe { libc::fcntl(descriptor, libc::F_GETFD) };
    if flags == -1
        || unsafe { libc::fcntl(descriptor, libc::F_SETFD, flags | libc::FD_CLOEXEC) } == -1
    {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::UnixJobBackend;
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
        let mut backend = UnixJobBackend::new(Arc::new(|_| {}));
        let job = PlatformJobId::new(7);
        backend
            .submit(JobRequest::Spawn(SpawnJob {
                job,
                command: "printf '%s\\n' \"$WMUX_JOB_VALUE\"; pwd; printf 'stderr\\n' >&2"
                    .to_string(),
                cwd: Some(cwd.clone()),
                environment: vec![("WMUX_JOB_VALUE".into(), "native".into())],
            }))
            .unwrap();

        let (output, exit_code) = collect_job(&mut backend, job);
        let output = String::from_utf8_lossy(&output);
        assert_eq!(exit_code, Some(0));
        assert!(output.contains("native\n"));
        assert!(output.contains("stderr\n"));
        assert!(output.contains(cwd.to_string_lossy().as_ref()));
        fs::remove_dir(cwd).unwrap();
    }

    #[test]
    fn terminating_a_job_kills_its_process_group_and_closes() {
        let mut backend = UnixJobBackend::new(Arc::new(|_| {}));
        let job = PlatformJobId::new(8);
        backend
            .submit(JobRequest::Spawn(SpawnJob {
                job,
                command: "sleep 30 & wait".to_string(),
                cwd: None,
                environment: Vec::new(),
            }))
            .unwrap();
        backend.submit(JobRequest::Terminate { job }).unwrap();
        let (_, exit_code) = collect_job(&mut backend, job);
        assert_ne!(exit_code, Some(0));
    }

    fn collect_job(backend: &mut UnixJobBackend, job: PlatformJobId) -> (Vec<u8>, Option<u32>) {
        let deadline = Instant::now() + Duration::from_secs(5);
        let mut output = Vec::new();
        let mut exit_code = None;
        loop {
            match backend.try_next_event(job).unwrap() {
                Some(JobEvent::Output { bytes, .. }) => output.extend(bytes),
                Some(JobEvent::Exited {
                    exit_code: code, ..
                }) => exit_code = code,
                Some(JobEvent::Closed { .. }) => return (output, exit_code),
                Some(JobEvent::BackendError { error, .. }) => panic!("job error: {error}"),
                None => {
                    assert!(Instant::now() < deadline, "job did not close");
                    thread::sleep(Duration::from_millis(5));
                }
            }
        }
    }
}
