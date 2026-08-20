use std::{ffi::OsString, io, path::PathBuf};

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct PlatformPaneId(u64);

impl PlatformPaneId {
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    pub const fn raw(self) -> u64 {
        self.0
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TerminalSize {
    pub cols: u16,
    pub rows: u16,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MouseEventKind {
    Down,
    Up,
    Drag,
    Move,
    ScrollUp,
    ScrollDown,
    ScrollLeft,
    ScrollRight,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MouseButton {
    None,
    Left,
    Middle,
    Right,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct MouseModifiers(u8);

impl MouseModifiers {
    pub const SHIFT: u8 = 1 << 0;
    pub const ALT: u8 = 1 << 1;
    pub const CONTROL: u8 = 1 << 2;

    pub const fn new(bits: u8) -> Self {
        Self(bits & (Self::SHIFT | Self::ALT | Self::CONTROL))
    }

    pub const fn bits(self) -> u8 {
        self.0
    }

    pub const fn contains(self, modifier: u8) -> bool {
        self.0 & modifier != 0
    }
}

/// A terminal mouse event in zero-based character-cell coordinates.
///
/// Platform backends normalize native input into this type. Raw console
/// records, window handles, and terminal-specific escape bytes never cross
/// the platform boundary.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MouseEvent {
    pub kind: MouseEventKind,
    pub button: MouseButton,
    pub modifiers: MouseModifiers,
    pub column: u16,
    pub row: u16,
}

impl TerminalSize {
    pub const fn new(cols: u16, rows: u16) -> Self {
        Self { cols, rows }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandSpec {
    pub program: OsString,
    pub args: Vec<OsString>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SpawnPane {
    pub pane: PlatformPaneId,
    pub size: TerminalSize,
    pub command: Option<CommandSpec>,
    pub cwd: Option<PathBuf>,
    pub environment: Vec<(OsString, OsString)>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TerminationMode {
    Graceful,
    Force,
}

/// A semantic operation requested by core from an OS backend.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PlatformRequest {
    SpawnPane(SpawnPane),
    WritePane {
        pane: PlatformPaneId,
        bytes: Vec<u8>,
    },
    ResizePane {
        pane: PlatformPaneId,
        size: TerminalSize,
    },
    TerminatePane {
        pane: PlatformPaneId,
        mode: TerminationMode,
    },
}

/// A semantic event emitted by an OS backend.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum PlatformEvent {
    PtyOutput {
        pane: PlatformPaneId,
        bytes: Vec<u8>,
    },
    PtyExited {
        pane: PlatformPaneId,
        exit_code: Option<u32>,
    },
}

pub type PtyEvent = PlatformEvent;

/// An OS backend owns all raw process and terminal handles internally.
///
/// The server communicates exclusively through semantic requests and events,
/// so an associated platform pane type cannot leak into core state.
pub trait PtyBackend {
    fn submit(&mut self, request: PlatformRequest) -> io::Result<()>;
    fn next_event(&mut self) -> io::Result<PlatformEvent>;
}

pub trait TerminalBackend {
    fn enter(&mut self) -> io::Result<()>;
    fn restore(&mut self) -> io::Result<()>;
    fn read_input(&mut self, buffer: &mut [u8]) -> io::Result<usize>;
    fn write_output(&mut self, bytes: &[u8]) -> io::Result<()>;
    fn size(&self) -> io::Result<TerminalSize>;
}

#[cfg(test)]
mod tests {
    use super::{
        CommandSpec, MouseButton, MouseEvent, MouseEventKind, MouseModifiers, PlatformEvent,
        PlatformPaneId, PlatformRequest, SpawnPane, TerminalSize, TerminationMode,
    };
    use std::{ffi::OsString, path::PathBuf};

    fn assert_send_sync<T: Send + Sync>() {}

    #[test]
    fn platform_contract_is_thread_safe_and_handle_free() {
        assert_send_sync::<PlatformEvent>();
        assert_send_sync::<PlatformRequest>();
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
}
