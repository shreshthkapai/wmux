use mux_core::ClientId;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct TerminalSize {
    pub columns: u16,
    pub rows: u16,
    pub xpixel: u16,
    pub ypixel: u16,
}

impl TerminalSize {
    pub const fn cells(columns: u16, rows: u16) -> Self {
        Self {
            columns,
            rows,
            xpixel: 0,
            ypixel: 0,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct TerminalClientHandle(pub u64);

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TerminalEvent {
    Input {
        client: ClientId,
        bytes: Vec<u8>,
    },
    Resized {
        client: ClientId,
        size: TerminalSize,
    },
    Closed {
        client: ClientId,
    },
}

pub trait TerminalClientBackend {
    type Error;

    fn open_current_terminal(&mut self) -> Result<TerminalClientHandle, Self::Error>;
    fn enter_raw_mode(&mut self, handle: TerminalClientHandle) -> Result<(), Self::Error>;
    fn restore_terminal(&mut self, handle: TerminalClientHandle) -> Result<(), Self::Error>;
    fn write_output(
        &mut self,
        handle: TerminalClientHandle,
        bytes: &[u8],
    ) -> Result<(), Self::Error>;
    fn query_size(&mut self, handle: TerminalClientHandle) -> Result<TerminalSize, Self::Error>;
}
