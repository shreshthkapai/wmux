use std::{fmt, io};

pub const MAX_PLATFORM_DIAGNOSTIC_BYTES: usize = 4_096;

pub type PlatformResult<T> = Result<T, PlatformError>;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PlatformErrorKind {
    NotFound,
    AlreadyRunning,
    Busy,
    PermissionDenied,
    Disconnected,
    TimedOut,
    InvalidInput,
    InvalidData,
    Unsupported,
    Other,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PlatformError {
    kind: PlatformErrorKind,
    operation: &'static str,
    message: String,
}

impl PlatformError {
    pub fn new(
        kind: PlatformErrorKind,
        operation: &'static str,
        message: impl Into<String>,
    ) -> Self {
        Self {
            kind,
            operation,
            message: bounded_message(message.into()),
        }
    }

    pub fn from_io(operation: &'static str, error: io::Error) -> Self {
        let kind = match error.kind() {
            io::ErrorKind::NotFound => PlatformErrorKind::NotFound,
            io::ErrorKind::AlreadyExists => PlatformErrorKind::AlreadyRunning,
            io::ErrorKind::WouldBlock => PlatformErrorKind::Busy,
            io::ErrorKind::PermissionDenied => PlatformErrorKind::PermissionDenied,
            io::ErrorKind::BrokenPipe
            | io::ErrorKind::UnexpectedEof
            | io::ErrorKind::ConnectionAborted
            | io::ErrorKind::ConnectionReset => PlatformErrorKind::Disconnected,
            io::ErrorKind::TimedOut => PlatformErrorKind::TimedOut,
            io::ErrorKind::InvalidInput => PlatformErrorKind::InvalidInput,
            io::ErrorKind::InvalidData => PlatformErrorKind::InvalidData,
            io::ErrorKind::Unsupported => PlatformErrorKind::Unsupported,
            _ => PlatformErrorKind::Other,
        };
        Self::new(kind, operation, error.to_string())
    }

    pub const fn kind(&self) -> PlatformErrorKind {
        self.kind
    }

    pub const fn operation(&self) -> &'static str {
        self.operation
    }

    pub fn message(&self) -> &str {
        &self.message
    }

    pub fn into_io(self) -> io::Error {
        let kind = match self.kind {
            PlatformErrorKind::NotFound => io::ErrorKind::NotFound,
            PlatformErrorKind::AlreadyRunning => io::ErrorKind::AlreadyExists,
            PlatformErrorKind::Busy => io::ErrorKind::WouldBlock,
            PlatformErrorKind::PermissionDenied => io::ErrorKind::PermissionDenied,
            PlatformErrorKind::Disconnected => io::ErrorKind::BrokenPipe,
            PlatformErrorKind::TimedOut => io::ErrorKind::TimedOut,
            PlatformErrorKind::InvalidInput => io::ErrorKind::InvalidInput,
            PlatformErrorKind::InvalidData => io::ErrorKind::InvalidData,
            PlatformErrorKind::Unsupported => io::ErrorKind::Unsupported,
            PlatformErrorKind::Other => io::ErrorKind::Other,
        };
        io::Error::new(kind, self.to_string())
    }
}

impl fmt::Display for PlatformError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.message.is_empty() {
            formatter.write_str(self.operation)
        } else {
            write!(formatter, "{}: {}", self.operation, self.message)
        }
    }
}

impl std::error::Error for PlatformError {}

fn bounded_message(mut message: String) -> String {
    if message.len() <= MAX_PLATFORM_DIAGNOSTIC_BYTES {
        return message;
    }
    let mut end = MAX_PLATFORM_DIAGNOSTIC_BYTES;
    while !message.is_char_boundary(end) {
        end -= 1;
    }
    message.truncate(end);
    message
}
