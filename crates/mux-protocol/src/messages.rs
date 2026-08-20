#[derive(Clone, Debug, Eq, PartialEq)]
pub struct IdentifyMessage {
    pub terminal_name: Option<String>,
    pub cwd: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandRequest {
    pub command: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandResponse {
    pub success: bool,
    pub message: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AttachRequest {
    pub target: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DetachRequest {
    pub reason: Option<String>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ProtocolMessage {
    Hello {
        version: u32,
        client_pid: u32,
    },
    HelloOk {
        version: u32,
        server_pid: u32,
    },
    HelloError {
        server_version: u32,
        message: String,
    },
    Identify(IdentifyMessage),
    Command(CommandRequest),
    CommandResponse(CommandResponse),
    Attach(AttachRequest),
    Detach(DetachRequest),
    Resize {
        cols: u16,
        rows: u16,
        xpixel: u16,
        ypixel: u16,
    },
    Input {
        bytes: Vec<u8>,
    },
    Output {
        bytes: Vec<u8>,
    },
    Exit {
        code: i32,
        message: Option<String>,
    },
    Shutdown,
}

impl ProtocolMessage {
    pub fn encode(&self) -> Vec<u8> {
        let mut out = Vec::new();
        match self {
            Self::Hello {
                version,
                client_pid,
            } => {
                put_u16(&mut out, 1);
                put_u32(&mut out, *version);
                put_u32(&mut out, *client_pid);
            }
            Self::HelloOk {
                version,
                server_pid,
            } => {
                put_u16(&mut out, 2);
                put_u32(&mut out, *version);
                put_u32(&mut out, *server_pid);
            }
            Self::HelloError {
                server_version,
                message,
            } => {
                put_u16(&mut out, 3);
                put_u32(&mut out, *server_version);
                put_string(&mut out, message);
            }
            Self::Identify(message) => {
                put_u16(&mut out, 4);
                put_opt_string(&mut out, message.terminal_name.as_deref());
                put_opt_string(&mut out, message.cwd.as_deref());
            }
            Self::Command(request) => {
                put_u16(&mut out, 5);
                put_string(&mut out, &request.command);
            }
            Self::CommandResponse(response) => {
                put_u16(&mut out, 6);
                put_bool(&mut out, response.success);
                put_string(&mut out, &response.message);
            }
            Self::Attach(request) => {
                put_u16(&mut out, 7);
                put_opt_string(&mut out, request.target.as_deref());
            }
            Self::Detach(request) => {
                put_u16(&mut out, 8);
                put_opt_string(&mut out, request.reason.as_deref());
            }
            Self::Resize {
                cols,
                rows,
                xpixel,
                ypixel,
            } => {
                put_u16(&mut out, 9);
                put_u16(&mut out, *cols);
                put_u16(&mut out, *rows);
                put_u16(&mut out, *xpixel);
                put_u16(&mut out, *ypixel);
            }
            Self::Input { bytes } => {
                put_u16(&mut out, 10);
                put_bytes(&mut out, bytes);
            }
            Self::Output { bytes } => {
                put_u16(&mut out, 11);
                put_bytes(&mut out, bytes);
            }
            Self::Exit { code, message } => {
                put_u16(&mut out, 12);
                put_i32(&mut out, *code);
                put_opt_string(&mut out, message.as_deref());
            }
            Self::Shutdown => {
                put_u16(&mut out, 13);
            }
        }
        out
    }

    pub fn decode(bytes: &[u8]) -> Result<Self, MessageDecodeError> {
        let mut cursor = Cursor::new(bytes);
        let tag = cursor.u16()?;
        let message = match tag {
            1 => Self::Hello {
                version: cursor.u32()?,
                client_pid: cursor.u32()?,
            },
            2 => Self::HelloOk {
                version: cursor.u32()?,
                server_pid: cursor.u32()?,
            },
            3 => Self::HelloError {
                server_version: cursor.u32()?,
                message: cursor.string()?,
            },
            4 => Self::Identify(IdentifyMessage {
                terminal_name: cursor.opt_string()?,
                cwd: cursor.opt_string()?,
            }),
            5 => Self::Command(CommandRequest {
                command: cursor.string()?,
            }),
            6 => Self::CommandResponse(CommandResponse {
                success: cursor.bool()?,
                message: cursor.string()?,
            }),
            7 => Self::Attach(AttachRequest {
                target: cursor.opt_string()?,
            }),
            8 => Self::Detach(DetachRequest {
                reason: cursor.opt_string()?,
            }),
            9 => Self::Resize {
                cols: cursor.u16()?,
                rows: cursor.u16()?,
                xpixel: cursor.u16()?,
                ypixel: cursor.u16()?,
            },
            10 => Self::Input {
                bytes: cursor.bytes()?,
            },
            11 => Self::Output {
                bytes: cursor.bytes()?,
            },
            12 => Self::Exit {
                code: cursor.i32()?,
                message: cursor.opt_string()?,
            },
            13 => Self::Shutdown,
            raw => return Err(MessageDecodeError::UnknownMessage(raw)),
        };
        cursor.finish()?;
        Ok(message)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum MessageDecodeError {
    Truncated,
    TrailingBytes(usize),
    UnknownMessage(u16),
    InvalidBool(u8),
    InvalidOptionTag(u8),
    InvalidUtf8,
    LengthOverflow,
}

struct Cursor<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Cursor<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    fn finish(&self) -> Result<(), MessageDecodeError> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err(MessageDecodeError::TrailingBytes(
                self.bytes.len() - self.offset,
            ))
        }
    }

    fn take(&mut self, len: usize) -> Result<&'a [u8], MessageDecodeError> {
        let end = self
            .offset
            .checked_add(len)
            .ok_or(MessageDecodeError::LengthOverflow)?;
        let Some(slice) = self.bytes.get(self.offset..end) else {
            return Err(MessageDecodeError::Truncated);
        };
        self.offset = end;
        Ok(slice)
    }

    fn u8(&mut self) -> Result<u8, MessageDecodeError> {
        Ok(self.take(1)?[0])
    }

    fn bool(&mut self) -> Result<bool, MessageDecodeError> {
        match self.u8()? {
            0 => Ok(false),
            1 => Ok(true),
            raw => Err(MessageDecodeError::InvalidBool(raw)),
        }
    }

    fn u16(&mut self) -> Result<u16, MessageDecodeError> {
        let bytes = self.take(2)?;
        Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
    }

    fn u32(&mut self) -> Result<u32, MessageDecodeError> {
        let bytes = self.take(4)?;
        Ok(u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    fn i32(&mut self) -> Result<i32, MessageDecodeError> {
        let bytes = self.take(4)?;
        Ok(i32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    fn string(&mut self) -> Result<String, MessageDecodeError> {
        let bytes = self.bytes()?;
        String::from_utf8(bytes).map_err(|_| MessageDecodeError::InvalidUtf8)
    }

    fn opt_string(&mut self) -> Result<Option<String>, MessageDecodeError> {
        match self.u8()? {
            0 => Ok(None),
            1 => Ok(Some(self.string()?)),
            raw => Err(MessageDecodeError::InvalidOptionTag(raw)),
        }
    }

    fn bytes(&mut self) -> Result<Vec<u8>, MessageDecodeError> {
        let len = self.u32()? as usize;
        Ok(self.take(len)?.to_vec())
    }
}

fn put_bool(out: &mut Vec<u8>, value: bool) {
    out.push(u8::from(value));
}

fn put_u16(out: &mut Vec<u8>, value: u16) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_i32(out: &mut Vec<u8>, value: i32) {
    out.extend_from_slice(&value.to_le_bytes());
}

fn put_string(out: &mut Vec<u8>, value: &str) {
    put_bytes(out, value.as_bytes());
}

fn put_opt_string(out: &mut Vec<u8>, value: Option<&str>) {
    match value {
        Some(value) => {
            out.push(1);
            put_string(out, value);
        }
        None => out.push(0),
    }
}

fn put_bytes(out: &mut Vec<u8>, value: &[u8]) {
    assert!(u32::try_from(value.len()).is_ok());
    put_u32(out, value.len() as u32);
    out.extend_from_slice(value);
}

#[cfg(test)]
mod tests {
    use super::{
        AttachRequest, CommandRequest, CommandResponse, DetachRequest, IdentifyMessage,
        ProtocolMessage,
    };

    #[test]
    fn every_message_round_trips() {
        let messages = [
            ProtocolMessage::Hello {
                version: 1,
                client_pid: 2,
            },
            ProtocolMessage::HelloOk {
                version: 3,
                server_pid: 4,
            },
            ProtocolMessage::HelloError {
                server_version: 5,
                message: "bad version".to_string(),
            },
            ProtocolMessage::Identify(IdentifyMessage {
                terminal_name: Some("xterm-256color".to_string()),
                cwd: Some("C:\\work".to_string()),
            }),
            ProtocolMessage::Command(CommandRequest {
                command: "list-clients".to_string(),
            }),
            ProtocolMessage::CommandResponse(CommandResponse {
                success: true,
                message: "ok".to_string(),
            }),
            ProtocolMessage::Attach(AttachRequest {
                target: Some("main".to_string()),
            }),
            ProtocolMessage::Detach(DetachRequest {
                reason: Some("test".to_string()),
            }),
            ProtocolMessage::Resize {
                cols: 120,
                rows: 40,
                xpixel: 0,
                ypixel: 0,
            },
            ProtocolMessage::Input {
                bytes: b"abc".to_vec(),
            },
            ProtocolMessage::Output {
                bytes: b"def".to_vec(),
            },
            ProtocolMessage::Exit {
                code: 1,
                message: None,
            },
            ProtocolMessage::Shutdown,
        ];

        for message in messages {
            assert_eq!(ProtocolMessage::decode(&message.encode()), Ok(message));
        }
    }
}
