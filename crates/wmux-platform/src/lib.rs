//! OS-neutral runtime seams shared by wmux clients, servers, and adapters.
//!
//! Native handles, descriptors, process identifiers used as handles, console
//! records, and signal numbers belong in platform adapter crates.

mod error;
mod job;
mod pane;
mod terminal;
mod transport;

pub use error::{PlatformError, PlatformErrorKind, PlatformResult, MAX_PLATFORM_DIAGNOSTIC_BYTES};
pub use job::{JobBackend, JobEvent, JobNotifier, JobRequest, PlatformJobId, SpawnJob};
pub use pane::{
    CommandSpec, PlatformEvent, PlatformNotifier, PlatformPaneId, PlatformRequest, PtyBackend,
    PtyEvent, SpawnPane, TerminationMode,
};
pub use terminal::{
    MouseButton, MouseEvent, MouseEventKind, MouseModifiers, TerminalBackend, TerminalInput,
    TerminalKeyCode, TerminalKeyEvent, TerminalKeyModifiers, TerminalModeGuard, TerminalSize,
};
pub use transport::{
    AcceptedConnection, BoxedIpcStream, ClientTransport, DaemonSpec, Endpoint, IpcStream,
    PeerIdentity, PlatformFuture, ServerListener, ServerPlatform,
};

#[cfg(test)]
mod tests {
    use super::{
        CommandSpec, JobEvent, JobRequest, MouseButton, MouseEvent, MouseEventKind, MouseModifiers,
        PeerIdentity, PlatformError, PlatformErrorKind, PlatformEvent, PlatformJobId,
        PlatformPaneId, PlatformRequest, SpawnJob, SpawnPane, TerminalKeyModifiers, TerminalSize,
        TerminationMode,
    };
    use std::{ffi::OsString, path::PathBuf};

    fn assert_send_sync<T: Send + Sync>() {}

    #[test]
    fn platform_contract_is_thread_safe_and_handle_free() {
        assert_send_sync::<PlatformEvent>();
        assert_send_sync::<PlatformRequest>();
        assert_send_sync::<JobEvent>();
        assert_send_sync::<JobRequest>();
        assert_send_sync::<MouseEvent>();

        let pane = PlatformPaneId::new(7);
        let spawn = PlatformRequest::SpawnPane(SpawnPane {
            pane,
            size: TerminalSize::new(120, 40),
            command: Some(CommandSpec {
                program: OsString::from("shell"),
                args: vec![OsString::from("--login")],
            }),
            cwd: Some(PathBuf::from("workspace")),
            environment: vec![(OsString::from("TERM"), OsString::from("wmux-256color"))],
        });
        let requests = [
            spawn,
            PlatformRequest::WritePane {
                pane,
                bytes: b"input".to_vec(),
            },
            PlatformRequest::ResizePane {
                pane,
                size: TerminalSize::new(80, 24),
            },
            PlatformRequest::TerminatePane {
                pane,
                mode: TerminationMode::Force,
            },
        ];

        assert_eq!(requests.len(), 4);
        assert_eq!(pane.raw(), 7);
        assert_eq!(
            MouseEvent {
                kind: MouseEventKind::ScrollUp,
                button: MouseButton::None,
                modifiers: MouseModifiers::new(MouseModifiers::CONTROL),
                column: 10,
                row: 5,
            }
            .modifiers
            .bits(),
            MouseModifiers::CONTROL
        );
    }

    #[test]
    fn platform_errors_are_classified_and_bounded() {
        let error = PlatformError::new(
            PlatformErrorKind::Disconnected,
            "read pane",
            "λ".repeat(4_096),
        );

        assert_eq!(error.kind(), PlatformErrorKind::Disconnected);
        assert_eq!(error.operation(), "read pane");
        assert!(error.message().len() <= 4_096);
        assert!(error.message().is_char_boundary(error.message().len()));
    }

    #[test]
    fn peer_identity_is_opaque_but_comparable() {
        let first = PeerIdentity::from_token(b"owner-a");
        let same = PeerIdentity::from_token(b"owner-a");
        let other = PeerIdentity::from_token(b"owner-b");

        assert_eq!(first, same);
        assert_ne!(first, other);
        assert!(!format!("{first:?}").contains("owner-a"));
    }

    #[test]
    fn terminal_modifiers_reject_unsupported_bits() {
        assert!(TerminalKeyModifiers::from_bits(0b1111).is_some());
        assert!(TerminalKeyModifiers::from_bits(0b1_0000).is_none());
    }

    #[test]
    fn pane_exit_and_stream_close_are_distinct_events() {
        let pane = PlatformPaneId::new(9);
        assert_ne!(
            PlatformEvent::PtyExited {
                pane,
                exit_code: Some(0),
            },
            PlatformEvent::PtyClosed { pane }
        );
    }

    #[test]
    fn job_contract_is_opaque_and_has_terminal_close() {
        let job = PlatformJobId::new(11);
        let request = JobRequest::Spawn(SpawnJob {
            job,
            command: "printf job".to_owned(),
            cwd: Some(PathBuf::from("workspace")),
            environment: vec![(OsString::from("WMUX_JOB"), OsString::from("1"))],
        });

        assert_eq!(job.raw(), 11);
        assert!(matches!(request, JobRequest::Spawn(_)));
        assert_ne!(
            JobEvent::Exited {
                job,
                exit_code: Some(0)
            },
            JobEvent::Closed { job }
        );
        assert!(!format!("{job:?}").contains("pid"));
    }
}
