//! Versioned local IPC protocol.

pub mod frame;
pub mod messages;
pub mod version;

pub use frame::{
    read_frame, read_message, write_frame, write_message, Frame, FrameDecodeError, FrameKind,
    MAGIC, MAX_FRAME_PAYLOAD,
};
pub use messages::{
    AttachRequest, CommandRequest, CommandResponse, DetachRequest, IdentifyMessage,
    MessageDecodeError, ProtocolMessage,
};
pub use version::{PROTOCOL_MAJOR, PROTOCOL_MINOR, PROTOCOL_VERSION};
