use crate::{version::PROTOCOL_MAJOR, ProtocolMessage};
use std::io::{self, Read, Write};

pub const MAGIC: &[u8; 4] = b"WMUX";
pub const MAX_FRAME_PAYLOAD: usize = 16 * 1024 * 1024;
pub const HEADER_LEN: usize = 20;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum FrameKind {
    Control = 1,
    AttachInput = 2,
    AttachOutput = 3,
    Event = 4,
    Error = 5,
}

impl FrameKind {
    fn from_u16(raw: u16) -> Option<Self> {
        match raw {
            1 => Some(Self::Control),
            2 => Some(Self::AttachInput),
            3 => Some(Self::AttachOutput),
            4 => Some(Self::Event),
            5 => Some(Self::Error),
            _ => None,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Frame {
    pub kind: FrameKind,
    pub request_id: u64,
    pub payload: Vec<u8>,
}

impl Frame {
    pub fn encode(&self) -> Vec<u8> {
        let payload_len = self.payload.len();
        assert!(payload_len <= MAX_FRAME_PAYLOAD);

        let mut out = Vec::with_capacity(HEADER_LEN + payload_len);
        out.extend_from_slice(MAGIC);
        out.extend_from_slice(&PROTOCOL_MAJOR.to_le_bytes());
        out.extend_from_slice(&(self.kind as u16).to_le_bytes());
        out.extend_from_slice(&self.request_id.to_le_bytes());
        out.extend_from_slice(&(payload_len as u32).to_le_bytes());
        out.extend_from_slice(&self.payload);
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, FrameDecodeError> {
        if bytes.len() < HEADER_LEN {
            return Err(FrameDecodeError::TruncatedHeader);
        }
        if &bytes[0..4] != MAGIC {
            return Err(FrameDecodeError::BadMagic);
        }

        let version = u16::from_le_bytes([bytes[4], bytes[5]]);
        if version != PROTOCOL_MAJOR {
            return Err(FrameDecodeError::UnsupportedVersion(version));
        }

        let kind_raw = u16::from_le_bytes([bytes[6], bytes[7]]);
        let kind = FrameKind::from_u16(kind_raw).ok_or(FrameDecodeError::UnknownKind(kind_raw))?;
        let request_id = u64::from_le_bytes([
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15],
        ]);
        let payload_len = u32::from_le_bytes([bytes[16], bytes[17], bytes[18], bytes[19]]) as usize;
        if payload_len > MAX_FRAME_PAYLOAD {
            return Err(FrameDecodeError::OversizedPayload(payload_len));
        }
        if bytes.len() < HEADER_LEN + payload_len {
            return Err(FrameDecodeError::TruncatedPayload);
        }

        Ok(Self {
            kind,
            request_id,
            payload: bytes[HEADER_LEN..HEADER_LEN + payload_len].to_vec(),
        })
    }
}

pub fn read_frame(reader: &mut impl Read) -> io::Result<Option<Frame>> {
    let mut header = [0_u8; HEADER_LEN];
    match reader.read_exact(&mut header) {
        Ok(()) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
        Err(error) => return Err(error),
    }

    let payload_len = u32::from_le_bytes([header[16], header[17], header[18], header[19]]) as usize;
    if payload_len > MAX_FRAME_PAYLOAD {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("frame payload too large: {payload_len}"),
        ));
    }

    let mut bytes = Vec::with_capacity(HEADER_LEN + payload_len);
    bytes.extend_from_slice(&header);
    bytes.resize(HEADER_LEN + payload_len, 0);
    reader.read_exact(&mut bytes[HEADER_LEN..])?;
    Frame::decode(&bytes).map(Some).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("invalid protocol frame: {error:?}"),
        )
    })
}

pub fn write_frame(writer: &mut impl Write, frame: &Frame) -> io::Result<()> {
    writer.write_all(&frame.encode())?;
    writer.flush()
}

pub fn read_message(reader: &mut impl Read) -> io::Result<Option<(u64, ProtocolMessage)>> {
    let Some(frame) = read_frame(reader)? else {
        return Ok(None);
    };
    let message = ProtocolMessage::decode(&frame.payload).map_err(|error| {
        io::Error::new(
            io::ErrorKind::InvalidData,
            format!("invalid protocol message: {error:?}"),
        )
    })?;
    Ok(Some((frame.request_id, message)))
}

pub fn write_message(
    writer: &mut impl Write,
    request_id: u64,
    message: &ProtocolMessage,
) -> io::Result<()> {
    write_frame(
        writer,
        &Frame {
            kind: FrameKind::Control,
            request_id,
            payload: message.encode(),
        },
    )
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum FrameDecodeError {
    TruncatedHeader,
    BadMagic,
    UnsupportedVersion(u16),
    UnknownKind(u16),
    OversizedPayload(usize),
    TruncatedPayload,
}

#[cfg(test)]
mod tests {
    use super::{Frame, FrameKind};

    #[test]
    fn frame_round_trips() {
        let frame = Frame {
            kind: FrameKind::Control,
            request_id: 42,
            payload: b"hello".to_vec(),
        };

        assert_eq!(Frame::decode(&frame.encode()), Ok(frame));
    }
}
