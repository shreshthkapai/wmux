use std::io::{self, IoSlice, Read, Write};
use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};

pub const VERSION: u32 = 5;
const MAGIC: &[u8; 4] = b"WMX5";
pub const FRAME_HEADER_LEN: usize = 9;
pub const MAX_FRAME: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct TerminalCapabilities(u32);

impl TerminalCapabilities {
    pub const SYNCHRONIZED_OUTPUT: u32 = 1 << 0;
    pub const SCROLL_REGION: u32 = 1 << 1;

    pub const fn new(bits: u32) -> Self {
        Self(bits)
    }

    pub const fn bits(self) -> u32 {
        self.0
    }

    pub const fn contains(self, capability: u32) -> bool {
        self.0 & capability != 0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Message {
    Hello {
        version: u32,
        pid: u32,
        capabilities: TerminalCapabilities,
    },
    HelloOk {
        version: u32,
        pid: u32,
        capabilities: TerminalCapabilities,
    },
    Command(String),
    CommandOk(String),
    CommandErr(String),
    Input(Vec<u8>),
    Key(Vec<u8>),
    Paste(Vec<u8>),
    Mouse(MouseEvent),
    Output(Vec<u8>),
    Clipboard(Vec<u8>),
    Resize {
        cols: u16,
        rows: u16,
    },
    Detach,
    Shutdown,
}

impl Message {
    fn tag(&self) -> u8 {
        match self {
            Self::Hello { .. } => 1,
            Self::HelloOk { .. } => 2,
            Self::Command(_) => 3,
            Self::CommandOk(_) => 4,
            Self::CommandErr(_) => 5,
            Self::Input(_) => 6,
            Self::Output(_) => 7,
            Self::Resize { .. } => 8,
            Self::Detach => 9,
            Self::Shutdown => 10,
            Self::Key(_) => 11,
            Self::Paste(_) => 12,
            Self::Mouse(_) => 13,
            Self::Clipboard(_) => 14,
        }
    }

    pub fn wire_len(&self) -> usize {
        FRAME_HEADER_LEN + payload_len(self)
    }
}

/// An encoded frame whose variable payload retains the message's allocation.
/// The transport writes `header()` and `payload()` with a vectored write.
#[derive(Debug)]
pub struct EncodedFrame {
    header: [u8; FRAME_HEADER_LEN],
    payload: EncodedPayload,
}

#[derive(Debug)]
enum EncodedPayload {
    Inline { bytes: [u8; 12], len: usize },
    Owned(Vec<u8>),
}

impl EncodedFrame {
    pub fn from_message(message: Message) -> Self {
        let tag = message.tag();
        let payload = EncodedPayload::from_message(message);
        let mut header = [0; FRAME_HEADER_LEN];
        header[..MAGIC.len()].copy_from_slice(MAGIC);
        header[4] = tag;
        header[5..].copy_from_slice(&(payload.as_slice().len() as u32).to_le_bytes());
        Self { header, payload }
    }

    pub fn header(&self) -> &[u8; FRAME_HEADER_LEN] {
        &self.header
    }

    pub fn payload(&self) -> &[u8] {
        self.payload.as_slice()
    }

    pub fn wire_len(&self) -> usize {
        FRAME_HEADER_LEN + self.payload.as_slice().len()
    }
}

impl EncodedPayload {
    fn from_message(message: Message) -> Self {
        match message {
            Message::Hello {
                version,
                pid,
                capabilities,
            }
            | Message::HelloOk {
                version,
                pid,
                capabilities,
            } => {
                let mut bytes = [0; 12];
                bytes[..4].copy_from_slice(&version.to_le_bytes());
                bytes[4..8].copy_from_slice(&pid.to_le_bytes());
                bytes[8..12].copy_from_slice(&capabilities.bits().to_le_bytes());
                Self::Inline { bytes, len: 12 }
            }
            Message::Command(text) | Message::CommandOk(text) | Message::CommandErr(text) => {
                Self::Owned(text.into_bytes())
            }
            Message::Input(bytes)
            | Message::Key(bytes)
            | Message::Paste(bytes)
            | Message::Output(bytes)
            | Message::Clipboard(bytes) => Self::Owned(bytes),
            Message::Resize { cols, rows } => {
                let mut bytes = [0; 12];
                bytes[..2].copy_from_slice(&cols.to_le_bytes());
                bytes[2..4].copy_from_slice(&rows.to_le_bytes());
                Self::Inline { bytes, len: 4 }
            }
            Message::Mouse(event) => {
                let mut bytes = [0; 12];
                encode_mouse(event, &mut bytes);
                Self::Inline { bytes, len: 7 }
            }
            Message::Detach | Message::Shutdown => Self::Inline {
                bytes: [0; 12],
                len: 0,
            },
        }
    }

    fn as_slice(&self) -> &[u8] {
        match self {
            Self::Inline { bytes, len } => &bytes[..*len],
            Self::Owned(bytes) => bytes,
        }
    }
}

pub fn write_message(mut writer: impl Write, message: &Message) -> io::Result<()> {
    let mut fixed = [0_u8; 12];
    let payload = borrowed_payload(message, &mut fixed);
    let header = frame_header(message.tag(), payload.len());
    write_vectored_all(&mut writer, &header, payload)
}

/// Encode one complete wire frame for transports that own their write buffer.
pub fn encode_frame(message: &Message) -> Vec<u8> {
    let mut frame = Vec::new();
    encode_frame_into(message, &mut frame);
    frame
}

/// Encode into a caller-owned buffer so benchmark, control, and file
/// transports can retain capacity across frames.
pub fn encode_frame_into(message: &Message, frame: &mut Vec<u8>) {
    frame.clear();
    frame.reserve(message.wire_len());
    frame.extend_from_slice(MAGIC);
    frame.push(message.tag());
    frame.extend_from_slice(&(payload_len(message) as u32).to_le_bytes());
    encode_payload_into(message, frame);
}

fn frame_header(tag: u8, payload_len: usize) -> [u8; FRAME_HEADER_LEN] {
    let mut header = [0; FRAME_HEADER_LEN];
    header[..MAGIC.len()].copy_from_slice(MAGIC);
    header[4] = tag;
    header[5..].copy_from_slice(&(payload_len as u32).to_le_bytes());
    header
}

fn borrowed_payload<'a>(message: &'a Message, fixed: &'a mut [u8; 12]) -> &'a [u8] {
    match message {
        Message::Hello {
            version,
            pid,
            capabilities,
        }
        | Message::HelloOk {
            version,
            pid,
            capabilities,
        } => {
            fixed[..4].copy_from_slice(&version.to_le_bytes());
            fixed[4..8].copy_from_slice(&pid.to_le_bytes());
            fixed[8..12].copy_from_slice(&capabilities.bits().to_le_bytes());
            &fixed[..12]
        }
        Message::Command(text) | Message::CommandOk(text) | Message::CommandErr(text) => {
            text.as_bytes()
        }
        Message::Input(bytes)
        | Message::Key(bytes)
        | Message::Paste(bytes)
        | Message::Output(bytes)
        | Message::Clipboard(bytes) => bytes,
        Message::Resize { cols, rows } => {
            fixed[..2].copy_from_slice(&cols.to_le_bytes());
            fixed[2..4].copy_from_slice(&rows.to_le_bytes());
            &fixed[..4]
        }
        Message::Mouse(event) => {
            encode_mouse(*event, fixed);
            &fixed[..7]
        }
        Message::Detach | Message::Shutdown => &[],
    }
}

fn write_vectored_all(writer: &mut impl Write, header: &[u8], payload: &[u8]) -> io::Result<()> {
    let mut header_offset = 0;
    let mut payload_offset = 0;
    while header_offset < header.len() || payload_offset < payload.len() {
        let written = if header_offset < header.len() {
            writer.write_vectored(&[
                IoSlice::new(&header[header_offset..]),
                IoSlice::new(&payload[payload_offset..]),
            ])?
        } else {
            writer.write(&payload[payload_offset..])?
        };
        if written == 0 {
            return Err(io::Error::new(
                io::ErrorKind::WriteZero,
                "failed to write IPC frame",
            ));
        }
        let header_remaining = header.len() - header_offset;
        let header_written = written.min(header_remaining);
        header_offset += header_written;
        payload_offset += written - header_written;
    }
    Ok(())
}

/// Validate a frame header and return its tag and payload length.
pub fn decode_frame_header(header: &[u8; FRAME_HEADER_LEN]) -> io::Result<(u8, usize)> {
    if &header[..MAGIC.len()] != MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad magic"));
    }
    let len = u32::from_le_bytes([header[5], header[6], header[7], header[8]]) as usize;
    if len > MAX_FRAME {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "frame too large",
        ));
    }
    Ok((header[4], len))
}

/// Decode a payload after `decode_frame_header` has validated its header.
pub fn decode_frame_payload(tag: u8, payload: &[u8]) -> io::Result<Message> {
    decode_payload_owned(tag, payload.to_vec())
}

/// Decode a frame payload by transferring its allocation into variable-sized
/// messages instead of copying their bytes a second time.
pub fn decode_frame_payload_owned(tag: u8, payload: Vec<u8>) -> io::Result<Message> {
    decode_payload_owned(tag, payload)
}

pub fn read_message(mut reader: impl Read) -> io::Result<Option<Message>> {
    let mut magic = [0; 4];
    if read_or_eof(&mut reader, &mut magic)? {
        return Ok(None);
    }
    if &magic != MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad magic"));
    }
    let mut tag = [0; 1];
    reader.read_exact(&mut tag)?;
    let mut len = [0; 4];
    reader.read_exact(&mut len)?;
    let len = u32::from_le_bytes(len) as usize;
    if len > MAX_FRAME {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "frame too large",
        ));
    }
    let mut payload = vec![0; len];
    reader.read_exact(&mut payload)?;
    decode_payload_owned(tag[0], payload).map(Some)
}

fn read_or_eof(reader: &mut impl Read, buf: &mut [u8]) -> io::Result<bool> {
    match reader.read_exact(buf) {
        Ok(()) => Ok(false),
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => Ok(true),
        Err(error) => Err(error),
    }
}

fn payload_len(message: &Message) -> usize {
    match message {
        Message::Hello { .. } | Message::HelloOk { .. } => 12,
        Message::Command(text) | Message::CommandOk(text) | Message::CommandErr(text) => text.len(),
        Message::Input(bytes)
        | Message::Key(bytes)
        | Message::Paste(bytes)
        | Message::Output(bytes)
        | Message::Clipboard(bytes) => bytes.len(),
        Message::Resize { .. } => 4,
        Message::Mouse(_) => 7,
        Message::Detach | Message::Shutdown => 0,
    }
}

fn encode_payload_into(message: &Message, out: &mut Vec<u8>) {
    match message {
        Message::Hello {
            version,
            pid,
            capabilities,
        }
        | Message::HelloOk {
            version,
            pid,
            capabilities,
        } => {
            out.extend_from_slice(&version.to_le_bytes());
            out.extend_from_slice(&pid.to_le_bytes());
            out.extend_from_slice(&capabilities.bits().to_le_bytes());
        }
        Message::Command(text) | Message::CommandOk(text) | Message::CommandErr(text) => {
            out.extend_from_slice(text.as_bytes());
        }
        Message::Input(bytes)
        | Message::Key(bytes)
        | Message::Paste(bytes)
        | Message::Output(bytes)
        | Message::Clipboard(bytes) => {
            out.extend_from_slice(bytes);
        }
        Message::Resize { cols, rows } => {
            out.extend_from_slice(&cols.to_le_bytes());
            out.extend_from_slice(&rows.to_le_bytes());
        }
        Message::Mouse(event) => {
            let mut bytes = [0; 12];
            encode_mouse(*event, &mut bytes);
            out.extend_from_slice(&bytes[..7]);
        }
        Message::Detach | Message::Shutdown => {}
    }
}

fn decode_payload_owned(tag: u8, payload: Vec<u8>) -> io::Result<Message> {
    match tag {
        1 => {
            let (version, pid, capabilities) = hello_fields(&payload)?;
            Ok(Message::Hello {
                version,
                pid,
                capabilities,
            })
        }
        2 => {
            let (version, pid, capabilities) = hello_fields(&payload)?;
            Ok(Message::HelloOk {
                version,
                pid,
                capabilities,
            })
        }
        3 => Ok(Message::Command(string_payload(payload)?)),
        4 => Ok(Message::CommandOk(string_payload(payload)?)),
        5 => Ok(Message::CommandErr(string_payload(payload)?)),
        6 => Ok(Message::Input(payload)),
        7 => Ok(Message::Output(payload)),
        8 => {
            if payload.len() != 4 {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "bad resize"));
            }
            Ok(Message::Resize {
                cols: u16::from_le_bytes([payload[0], payload[1]]),
                rows: u16::from_le_bytes([payload[2], payload[3]]),
            })
        }
        9 => {
            require_empty(&payload)?;
            Ok(Message::Detach)
        }
        10 => {
            require_empty(&payload)?;
            Ok(Message::Shutdown)
        }
        11 => Ok(Message::Key(payload)),
        12 => Ok(Message::Paste(payload)),
        13 => decode_mouse(&payload).map(Message::Mouse),
        14 => Ok(Message::Clipboard(payload)),
        _ => Err(io::Error::new(io::ErrorKind::InvalidData, "unknown tag")),
    }
}

fn encode_mouse(event: MouseEvent, out: &mut [u8; 12]) {
    out[0] = match event.kind {
        MouseEventKind::Down => 0,
        MouseEventKind::Up => 1,
        MouseEventKind::Drag => 2,
        MouseEventKind::Move => 3,
        MouseEventKind::ScrollUp => 4,
        MouseEventKind::ScrollDown => 5,
        MouseEventKind::ScrollLeft => 6,
        MouseEventKind::ScrollRight => 7,
    };
    out[1] = match event.button {
        MouseButton::None => 0,
        MouseButton::Left => 1,
        MouseButton::Middle => 2,
        MouseButton::Right => 3,
    };
    out[2] = event.modifiers.bits();
    out[3..5].copy_from_slice(&event.column.to_le_bytes());
    out[5..7].copy_from_slice(&event.row.to_le_bytes());
}

fn decode_mouse(payload: &[u8]) -> io::Result<MouseEvent> {
    if payload.len() != 7 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "bad mouse event",
        ));
    }
    let kind = match payload[0] {
        0 => MouseEventKind::Down,
        1 => MouseEventKind::Up,
        2 => MouseEventKind::Drag,
        3 => MouseEventKind::Move,
        4 => MouseEventKind::ScrollUp,
        5 => MouseEventKind::ScrollDown,
        6 => MouseEventKind::ScrollLeft,
        7 => MouseEventKind::ScrollRight,
        _ => return Err(io::Error::new(io::ErrorKind::InvalidData, "bad mouse kind")),
    };
    let button = match payload[1] {
        0 => MouseButton::None,
        1 => MouseButton::Left,
        2 => MouseButton::Middle,
        3 => MouseButton::Right,
        _ => {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "bad mouse button",
            ))
        }
    };
    Ok(MouseEvent {
        kind,
        button,
        modifiers: MouseModifiers::new(payload[2]),
        column: u16::from_le_bytes([payload[3], payload[4]]),
        row: u16::from_le_bytes([payload[5], payload[6]]),
    })
}

fn require_empty(payload: &[u8]) -> io::Result<()> {
    if payload.is_empty() {
        Ok(())
    } else {
        Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unexpected payload",
        ))
    }
}

fn hello_fields(payload: &[u8]) -> io::Result<(u32, u32, TerminalCapabilities)> {
    if payload.len() != 12 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "bad hello"));
    }
    Ok((
        u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]),
        u32::from_le_bytes([payload[4], payload[5], payload[6], payload[7]]),
        TerminalCapabilities::new(u32::from_le_bytes([
            payload[8],
            payload[9],
            payload[10],
            payload[11],
        ])),
    ))
}

fn string_payload(payload: Vec<u8>) -> io::Result<String> {
    String::from_utf8(payload)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid utf8"))
}

#[cfg(test)]
mod tests {
    use super::{
        decode_frame_header, decode_frame_payload, decode_frame_payload_owned, encode_frame,
        read_message, write_message, EncodedFrame, Message, TerminalCapabilities, FRAME_HEADER_LEN,
        VERSION,
    };
    use std::io;
    use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};

    #[test]
    fn roundtrips_all_basics() {
        let messages = [
            Message::Hello {
                version: VERSION,
                pid: 10,
                capabilities: TerminalCapabilities::new(TerminalCapabilities::SYNCHRONIZED_OUTPUT),
            },
            Message::Command("new -s x".to_string()),
            Message::Input(b"abc".to_vec()),
            Message::Key(b"abc".to_vec()),
            Message::Paste(b"abc def".to_vec()),
            Message::Mouse(MouseEvent {
                kind: MouseEventKind::ScrollUp,
                button: MouseButton::None,
                modifiers: MouseModifiers::new(MouseModifiers::SHIFT),
                column: 120,
                row: 40,
            }),
            Message::Clipboard(b"selected text".to_vec()),
            Message::Resize { cols: 80, rows: 24 },
            Message::Detach,
        ];

        for message in messages {
            let mut bytes = Vec::new();
            write_message(&mut bytes, &message).unwrap();
            assert_eq!(read_message(bytes.as_slice()).unwrap(), Some(message));
        }
    }

    #[test]
    fn async_transport_primitives_preserve_the_wire_format() {
        let message = Message::Output(vec![0, 1, 2, 0xff]);
        let frame = encode_frame(&message);
        let header: [u8; FRAME_HEADER_LEN] = frame[..FRAME_HEADER_LEN].try_into().unwrap();
        let (tag, payload_len) = decode_frame_header(&header).unwrap();
        assert_eq!(payload_len, frame.len() - FRAME_HEADER_LEN);
        assert_eq!(
            decode_frame_payload(tag, &frame[FRAME_HEADER_LEN..]).unwrap(),
            message
        );
    }

    #[test]
    fn owned_codec_transfers_variable_payload_without_reallocation() {
        let payload = vec![b'x'; 4096];
        let pointer = payload.as_ptr();
        let decoded = decode_frame_payload_owned(7, payload).unwrap();
        assert!(
            matches!(decoded, Message::Output(bytes) if bytes.len() == 4096 && bytes.as_ptr() == pointer)
        );

        let payload = vec![b'y'; 4096];
        let pointer = payload.as_ptr();
        let frame = EncodedFrame::from_message(Message::Output(payload));
        assert_eq!(frame.payload().as_ptr(), pointer);
    }

    #[test]
    fn async_transport_header_rejects_oversized_frames() {
        let mut header = [0_u8; FRAME_HEADER_LEN];
        header[..4].copy_from_slice(b"WMX5");
        header[4] = 7;
        header[5..].copy_from_slice(&u32::MAX.to_le_bytes());
        assert_eq!(
            decode_frame_header(&header).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }

    #[test]
    fn fixed_empty_messages_reject_trailing_payload() {
        assert_eq!(
            decode_frame_payload_owned(9, vec![1]).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );
    }
}
