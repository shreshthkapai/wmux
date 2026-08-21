use crate::{ClientId, Command, KeyEvent, PaneId, TimerId};
use wmux_platform::MouseEvent;

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ClientInput {
    Bytes(Vec<u8>),
    Key(KeyEvent),
    Paste(Vec<u8>),
}

impl ClientInput {
    /// Translate semantic client input into the byte stream consumed by a PTY.
    /// Paste stays semantic until this boundary so every backend observes the
    /// authoritative pane's bracketed-paste mode.
    pub fn into_pty_bytes(self, bracketed_paste: bool) -> Vec<u8> {
        match self {
            Self::Bytes(bytes) => bytes,
            Self::Key(event) => event.raw,
            Self::Paste(bytes) if bracketed_paste => {
                let mut payload = Vec::with_capacity(bytes.len() + 12);
                payload.extend_from_slice(b"\x1b[200~");
                payload.extend_from_slice(&bytes);
                payload.extend_from_slice(b"\x1b[201~");
                payload
            }
            Self::Paste(bytes) => bytes,
        }
    }
}

/// An input to the single state-owning server loop.
///
/// Producers may create these events concurrently, but only the server loop
/// may apply them to multiplexer state.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ServerEvent {
    PtyOutput {
        pane: PaneId,
        bytes: Vec<u8>,
    },
    PtyExited {
        pane: PaneId,
        exit_code: Option<u32>,
    },
    ClientInput {
        client: ClientId,
        input: ClientInput,
    },
    ClientMouse {
        client: ClientId,
        event: MouseEvent,
    },
    ClientResize {
        client: ClientId,
        cols: u16,
        rows: u16,
    },
    ClientWritable {
        client: ClientId,
    },
    Command {
        client: ClientId,
        command: Command,
    },
    Timer {
        timer: TimerId,
    },
}

impl ServerEvent {
    pub const fn kind(&self) -> &'static str {
        match self {
            Self::PtyOutput { .. } => "pty-output",
            Self::PtyExited { .. } => "pty-exited",
            Self::ClientInput { .. } => "client-input",
            Self::ClientMouse { .. } => "client-mouse",
            Self::ClientResize { .. } => "client-resize",
            Self::ClientWritable { .. } => "client-writable",
            Self::Command { .. } => "command",
            Self::Timer { .. } => "timer",
        }
    }
}

#[cfg(test)]
mod tests {
    use super::ServerEvent;
    use crate::{ClientId, ClientInput, Command, KeyCode, KeyEvent, PaneId, TimerId};
    use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};

    fn assert_send_sync<T: Send + Sync>() {}

    #[test]
    fn server_events_are_thread_safe_and_semantic() {
        assert_send_sync::<ServerEvent>();

        let cases = [
            ServerEvent::PtyOutput {
                pane: PaneId::new(1),
                bytes: b"output".to_vec(),
            },
            ServerEvent::PtyExited {
                pane: PaneId::new(1),
                exit_code: Some(0),
            },
            ServerEvent::ClientInput {
                client: ClientId::new(2),
                input: ClientInput::Bytes(b"input".to_vec()),
            },
            ServerEvent::ClientMouse {
                client: ClientId::new(2),
                event: MouseEvent {
                    kind: MouseEventKind::ScrollUp,
                    button: MouseButton::None,
                    modifiers: MouseModifiers::default(),
                    column: 5,
                    row: 3,
                },
            },
            ServerEvent::ClientResize {
                client: ClientId::new(2),
                cols: 120,
                rows: 40,
            },
            ServerEvent::ClientWritable {
                client: ClientId::new(2),
            },
            ServerEvent::Command {
                client: ClientId::new(2),
                command: Command::ListSessions,
            },
            ServerEvent::Timer {
                timer: TimerId::new(3),
            },
        ];

        assert_eq!(
            cases.map(|event| event.kind()),
            [
                "pty-output",
                "pty-exited",
                "client-input",
                "client-mouse",
                "client-resize",
                "client-writable",
                "command",
                "timer",
            ]
        );
    }

    #[test]
    fn pane_input_translation_preserves_keys_and_applies_bracketed_paste() {
        assert_eq!(
            ClientInput::Bytes(b"\x1b[A".to_vec()).into_pty_bytes(true),
            b"\x1b[A"
        );
        assert_eq!(
            ClientInput::Key(KeyEvent::new(KeyCode::escape(), vec![0x1b])).into_pty_bytes(true),
            b"\x1b"
        );
        assert_eq!(
            ClientInput::Paste(b"abc".to_vec()).into_pty_bytes(false),
            b"abc"
        );
        assert_eq!(
            ClientInput::Paste(b"abc".to_vec()).into_pty_bytes(true),
            b"\x1b[200~abc\x1b[201~"
        );
    }
}
