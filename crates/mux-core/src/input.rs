#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TerminalKey {
    Backspace,
    DeleteWordBackward,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TerminalInput {
    Bytes(Vec<u8>),
    Key { key: TerminalKey, raw: Vec<u8> },
}

pub fn parse_terminal_input(bytes: &[u8]) -> Vec<TerminalInput> {
    let mut events = Vec::new();
    let mut literal = Vec::new();
    let mut index = 0;

    while index < bytes.len() {
        if bytes[index..].starts_with(b"\x1b\x7f")
            || bytes[index..].starts_with(b"\x1b\x08")
            || bytes[index..].starts_with(b"\x1b[3;5~")
        {
            flush_literal(&mut events, &mut literal);
            let len = if bytes[index..].starts_with(b"\x1b[3;5~") {
                b"\x1b[3;5~".len()
            } else {
                2
            };
            events.push(TerminalInput::Key {
                key: TerminalKey::DeleteWordBackward,
                raw: bytes[index..index + len].to_vec(),
            });
            index += len;
        } else if bytes[index] == 0x7f || bytes[index] == 0x08 {
            flush_literal(&mut events, &mut literal);
            events.push(TerminalInput::Key {
                key: TerminalKey::Backspace,
                raw: vec![bytes[index]],
            });
            index += 1;
        } else {
            literal.push(bytes[index]);
            index += 1;
        }
    }

    flush_literal(&mut events, &mut literal);
    events
}

pub fn encode_pane_input(events: &[TerminalInput]) -> Vec<u8> {
    let mut out = Vec::new();
    for event in events {
        match event {
            TerminalInput::Bytes(bytes) => out.extend_from_slice(bytes),
            TerminalInput::Key { raw, .. } => out.extend_from_slice(raw),
        }
    }
    out
}

fn flush_literal(events: &mut Vec<TerminalInput>, literal: &mut Vec<u8>) {
    if literal.is_empty() {
        return;
    }
    events.push(TerminalInput::Bytes(std::mem::take(literal)));
}

#[cfg(test)]
mod tests {
    use super::{encode_pane_input, parse_terminal_input, TerminalInput, TerminalKey};

    #[test]
    fn parses_backspace_as_key_event() {
        assert_eq!(
            parse_terminal_input(b"a\x7fb"),
            vec![
                TerminalInput::Bytes(b"a".to_vec()),
                TerminalInput::Key {
                    key: TerminalKey::Backspace,
                    raw: b"\x7f".to_vec()
                },
                TerminalInput::Bytes(b"b".to_vec())
            ]
        );
    }

    #[test]
    fn parses_common_ctrl_backspace_forms_as_delete_word_backward() {
        assert_eq!(
            parse_terminal_input(b"\x1b\x7f"),
            vec![TerminalInput::Key {
                key: TerminalKey::DeleteWordBackward,
                raw: b"\x1b\x7f".to_vec()
            }]
        );
        assert_eq!(
            parse_terminal_input(b"\x1b\x08"),
            vec![TerminalInput::Key {
                key: TerminalKey::DeleteWordBackward,
                raw: b"\x1b\x08".to_vec()
            }]
        );
        assert_eq!(
            parse_terminal_input(b"\x1b[3;5~"),
            vec![TerminalInput::Key {
                key: TerminalKey::DeleteWordBackward,
                raw: b"\x1b[3;5~".to_vec()
            }]
        );
    }

    #[test]
    fn keeps_raw_bytes_for_pane_input() {
        let events = vec![
            TerminalInput::Bytes(b"abc".to_vec()),
            TerminalInput::Key {
                key: TerminalKey::Backspace,
                raw: b"\x08".to_vec(),
            },
            TerminalInput::Key {
                key: TerminalKey::DeleteWordBackward,
                raw: b"\x1b[3;5~".to_vec(),
            },
        ];

        assert_eq!(encode_pane_input(&events), b"abc\x08\x1b[3;5~");
    }
}
