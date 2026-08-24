use std::collections::BTreeMap;

use crate::{CommandList, CommandSource, QueuedCommand};

pub const MAX_JOBS: usize = 64;
pub const MAX_JOB_OUTPUT_BYTES: usize = 1024 * 1024;

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct JobId(u64);

impl JobId {
    pub const fn new(value: u64) -> Self {
        Self(value)
    }
    pub const fn raw(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum JobContinuation {
    RunShell,
    IfShell {
        if_true: CommandList,
        if_false: Option<CommandList>,
    },
}

#[derive(Debug)]
pub struct Job {
    pub id: JobId,
    pub command: String,
    pub background: bool,
    pub client: crate::ClientId,
    pub source: CommandSource,
    pub continuation: JobContinuation,
    pub owner: Option<QueuedCommand>,
    output: Vec<u8>,
    output_truncated: bool,
    exit_code: Option<u32>,
}

impl Job {
    pub fn output(&self) -> &[u8] {
        &self.output
    }
    pub const fn output_truncated(&self) -> bool {
        self.output_truncated
    }
    pub const fn exit_code(&self) -> Option<u32> {
        self.exit_code
    }
}

#[derive(Debug, Default)]
pub struct JobStore {
    next_id: u64,
    jobs: BTreeMap<JobId, Job>,
}

impl JobStore {
    pub fn start(
        &mut self,
        command: String,
        background: bool,
        continuation: JobContinuation,
        queued: QueuedCommand,
    ) -> Result<JobId, String> {
        if self.jobs.len() >= MAX_JOBS {
            return Err("too many active jobs (limit 64)".to_string());
        }
        self.next_id = self
            .next_id
            .checked_add(1)
            .ok_or_else(|| "job ID space exhausted".to_string())?;
        let id = JobId::new(self.next_id);
        self.jobs.insert(
            id,
            Job {
                id,
                command,
                background,
                client: queued.client,
                source: queued.source,
                continuation,
                owner: (!background).then_some(queued),
                output: Vec::new(),
                output_truncated: false,
                exit_code: None,
            },
        );
        Ok(id)
    }

    pub fn append_output(&mut self, id: JobId, bytes: &[u8]) -> bool {
        let Some(job) = self.jobs.get_mut(&id) else {
            return false;
        };
        let available = MAX_JOB_OUTPUT_BYTES.saturating_sub(job.output.len());
        let accepted = available.min(bytes.len());
        job.output.extend_from_slice(&bytes[..accepted]);
        job.output_truncated |= accepted != bytes.len();
        true
    }

    pub fn mark_exited(&mut self, id: JobId, exit_code: Option<u32>) -> bool {
        let Some(job) = self.jobs.get_mut(&id) else {
            return false;
        };
        job.exit_code = exit_code;
        true
    }

    pub fn finish(&mut self, id: JobId) -> Option<Job> {
        self.jobs.remove(&id)
    }
    pub fn contains(&self, id: JobId) -> bool {
        self.jobs.contains_key(&id)
    }
    pub fn owns_sequence(&self, sequence: u64) -> bool {
        self.jobs.values().any(|job| {
            job.owner
                .as_ref()
                .is_some_and(|owner| owner.sequence == sequence)
        })
    }
    pub fn ids(&self) -> impl Iterator<Item = JobId> + '_ {
        self.jobs.keys().copied()
    }
    pub fn len(&self) -> usize {
        self.jobs.len()
    }
    pub fn is_empty(&self) -> bool {
        self.jobs.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::{JobContinuation, JobStore, MAX_JOBS, MAX_JOB_OUTPUT_BYTES};
    use crate::{parse_command_text, ClientId, CommandSource, QueuedCommand};

    fn queued(sequence: u64) -> QueuedCommand {
        QueuedCommand {
            invocation: sequence,
            sequence,
            client: ClientId::new(1),
            command: parse_command_text("display-message test").unwrap()[0].clone(),
            source: CommandSource::ClientRequest,
            final_in_list: true,
        }
    }

    #[test]
    fn jobs_are_bounded_and_capture_output_without_unbounded_growth() {
        let mut jobs = JobStore::default();
        let first = jobs
            .start("first".into(), false, JobContinuation::RunShell, queued(1))
            .unwrap();
        assert!(jobs.append_output(first, &vec![b'x'; MAX_JOB_OUTPUT_BYTES + 1]));
        let finished = jobs.finish(first).unwrap();
        assert_eq!(finished.output().len(), MAX_JOB_OUTPUT_BYTES);
        assert!(finished.output_truncated());

        for sequence in 1..=MAX_JOBS as u64 {
            jobs.start(
                sequence.to_string(),
                true,
                JobContinuation::RunShell,
                queued(sequence),
            )
            .unwrap();
        }
        assert_eq!(
            jobs.start(
                "overflow".into(),
                true,
                JobContinuation::RunShell,
                queued(100)
            ),
            Err("too many active jobs (limit 64)".into())
        );
    }
}
