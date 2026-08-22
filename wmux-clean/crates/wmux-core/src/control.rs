use crate::{ClientId, JobId, PaneId, SessionId, WindowId};

pub const MAX_CONTROL_OUTPUT_BYTES: usize = 64 * 1024;
pub const MAX_CONTROL_TEXT_BYTES: usize = 1024 * 1024;
pub const MAX_CONTROL_NAME_BYTES: usize = 64 * 1024;

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ControlNotification {
    ClientAttached { client: ClientId },
    ClientDetached { client: ClientId },
    SessionCreated { session: SessionId },
    SessionClosed { session: SessionId },
    WindowCreated { window: WindowId },
    WindowClosed { window: WindowId },
    PaneCreated { pane: PaneId },
    PaneClosed { pane: PaneId },
    BufferChanged { name: Option<String> },
    BufferDeleted { name: Option<String> },
    JobFinished { job: JobId, exit_code: Option<u32> },
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ControlRecord {
    Ready,
    Begin { sequence: u64 },
    Output { pane: PaneId, bytes: Vec<u8> },
    Notification(ControlNotification),
    End { sequence: u64, output: String },
    Error { sequence: u64, message: String },
    Pause { pane: Option<PaneId> },
}

#[cfg(test)]
mod tests {
    use super::{ControlNotification, ControlRecord, MAX_CONTROL_OUTPUT_BYTES};
    use crate::{ClientId, JobId, PaneId};

    #[test]
    fn records_keep_stable_ids_and_arbitrary_output_bytes() {
        assert_eq!(
            ControlRecord::Output {
                pane: PaneId::new(9),
                bytes: vec![0, 0xff, b'\\', b'\n'],
            },
            ControlRecord::Output {
                pane: PaneId::new(9),
                bytes: vec![0, 0xff, b'\\', b'\n'],
            }
        );
        assert!(matches!(
            ControlNotification::ClientAttached { client: ClientId::new(4) },
            ControlNotification::ClientAttached { client } if client.raw() == 4
        ));
        assert!(matches!(
            ControlNotification::JobFinished { job: JobId::new(7), exit_code: None },
            ControlNotification::JobFinished { job, .. } if job.raw() == 7
        ));
        assert_eq!(MAX_CONTROL_OUTPUT_BYTES, 64 * 1024);
    }
}
