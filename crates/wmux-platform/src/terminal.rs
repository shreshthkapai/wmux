use crate::PlatformResult;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TerminalSize {
    pub cols: u16,
    pub rows: u16,
}

impl TerminalSize {
    pub const fn new(cols: u16, rows: u16) -> Self {
        Self { cols, rows }
    }
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
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct MouseEvent {
    pub kind: MouseEventKind,
    pub button: MouseButton,
    pub modifiers: MouseModifiers,
    pub column: u16,
    pub row: u16,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TerminalKeyCode {
    Char(char),
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Backspace,
    Delete,
    Insert,
    Enter,
    Tab,
    BackTab,
    Escape,
    Function(u8),
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct TerminalKeyModifiers(u8);

impl TerminalKeyModifiers {
    pub const SHIFT: u8 = 1 << 0;
    pub const ALT: u8 = 1 << 1;
    pub const CONTROL: u8 = 1 << 2;
    pub const SUPER: u8 = 1 << 3;
    const VALID: u8 = Self::SHIFT | Self::ALT | Self::CONTROL | Self::SUPER;

    pub const fn from_bits(bits: u8) -> Option<Self> {
        if bits & !Self::VALID == 0 {
            Some(Self(bits))
        } else {
            None
        }
    }

    pub const fn new(bits: u8) -> Self {
        Self(bits & Self::VALID)
    }

    pub const fn bits(self) -> u8 {
        self.0
    }

    pub const fn contains(self, modifier: u8) -> bool {
        self.0 & modifier != 0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TerminalKeyEvent {
    pub code: TerminalKeyCode,
    pub modifiers: TerminalKeyModifiers,
    pub raw: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TerminalInput {
    Key(TerminalKeyEvent),
    Paste(String),
    Mouse(MouseEvent),
    Resize(TerminalSize),
}

/// A saved terminal mode which restores the host terminal when dropped.
pub trait TerminalModeGuard: Send {}

impl<T: Send> TerminalModeGuard for T {}

pub trait TerminalBackend: Send + Sync {
    fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>>;
    fn read_input(&self) -> PlatformResult<Option<TerminalInput>>;
    fn write_output(&self, bytes: &[u8]) -> PlatformResult<()>;
    fn write_render_transaction(
        &self,
        bytes: &[u8],
        synchronized_output: bool,
    ) -> PlatformResult<()>;
    fn write_clipboard_text(&self, text: &str) -> PlatformResult<()>;
    fn size(&self) -> PlatformResult<TerminalSize>;
}
