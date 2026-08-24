use wmux_core::{Color, Screen};

#[derive(Debug)]
enum State {
    Ground,
    Escape,
    Csi { private: bool, bytes: Vec<u8> },
    Osc,
    OscEscape,
}

pub struct LegacyTerminalEngine {
    state: State,
    utf8: Vec<u8>,
}

impl LegacyTerminalEngine {
    pub fn new() -> Self {
        Self {
            state: State::Ground,
            utf8: Vec::new(),
        }
    }

    pub fn feed(&mut self, screen: &mut Screen, bytes: &[u8]) {
        for byte in bytes.iter().copied() {
            match &mut self.state {
                State::Ground => match byte {
                    0x1b => self.state = State::Escape,
                    0x00..=0x1f | 0x7f => execute(screen, byte),
                    _ => {
                        if let Some(ch) = decode_print(&mut self.utf8, byte) {
                            screen.put_char(ch);
                        }
                    }
                },
                State::Escape => {
                    self.state = match byte {
                        b'[' => State::Csi {
                            private: false,
                            bytes: Vec::new(),
                        },
                        b']' => State::Osc,
                        b'7' => {
                            screen.save_cursor();
                            State::Ground
                        }
                        b'8' => {
                            screen.restore_cursor();
                            State::Ground
                        }
                        b'D' => {
                            screen.linefeed();
                            State::Ground
                        }
                        b'E' => {
                            screen.newline();
                            State::Ground
                        }
                        b'M' => {
                            screen.move_up(1);
                            State::Ground
                        }
                        b'c' => {
                            screen.clear_screen_mode(2);
                            State::Ground
                        }
                        _ => State::Ground,
                    };
                }
                State::Csi { private, bytes } => {
                    if byte == b'?' && bytes.is_empty() {
                        *private = true;
                    } else if (0x40..=0x7e).contains(&byte) {
                        let params = parse_params(bytes);
                        let private = *private;
                        self.state = State::Ground;
                        apply_csi(screen, private, &params, byte);
                    } else {
                        bytes.push(byte);
                    }
                }
                State::Osc => {
                    if byte == 0x07 {
                        self.state = State::Ground;
                    } else if byte == 0x1b {
                        self.state = State::OscEscape;
                    }
                }
                State::OscEscape => {
                    self.state = if byte == b'\\' {
                        State::Ground
                    } else {
                        State::Osc
                    };
                }
            }
        }
    }
}

fn decode_print(bytes: &mut Vec<u8>, byte: u8) -> Option<char> {
    bytes.push(byte);
    match std::str::from_utf8(bytes) {
        Ok(text) => {
            let ch = text.chars().next();
            bytes.clear();
            ch
        }
        Err(error) if error.error_len().is_none() => None,
        Err(_) => {
            bytes.clear();
            Some('\u{fffd}')
        }
    }
}

fn parse_params(bytes: &[u8]) -> Vec<u16> {
    if bytes.is_empty() {
        return Vec::new();
    }
    String::from_utf8_lossy(bytes)
        .split(';')
        .map(|part| part.parse().unwrap_or(0))
        .collect()
}

fn param(params: &[u16], index: usize, default: u16) -> u16 {
    match params.get(index).copied() {
        Some(0) | None => default,
        Some(value) => value,
    }
}

fn execute(screen: &mut Screen, byte: u8) {
    match byte {
        b'\x08' => screen.backspace(),
        b'\t' => screen.tab(),
        b'\n' | 0x0b | 0x0c => screen.linefeed(),
        b'\r' => screen.carriage_return(),
        0x85 => screen.newline(),
        _ => {}
    }
}

fn apply_csi(screen: &mut Screen, private: bool, params: &[u16], final_byte: u8) {
    match final_byte {
        b'@' => screen.insert_blank_chars(param(params, 0, 1)),
        b'A' => screen.move_up(param(params, 0, 1)),
        b'B' | b'e' => screen.move_down(param(params, 0, 1)),
        b'C' | b'a' => screen.move_right(param(params, 0, 1)),
        b'D' => screen.move_left(param(params, 0, 1)),
        b'E' => {
            screen.move_down(param(params, 0, 1));
            screen.carriage_return();
        }
        b'F' => {
            screen.move_up(param(params, 0, 1));
            screen.carriage_return();
        }
        b'G' => screen.move_to(screen.cursor().0, param(params, 0, 1).saturating_sub(1)),
        b'H' | b'f' => screen.move_to(
            param(params, 0, 1).saturating_sub(1),
            param(params, 1, 1).saturating_sub(1),
        ),
        b'J' => screen.clear_screen_mode(param(params, 0, 0)),
        b'K' => screen.clear_line(param(params, 0, 0)),
        b'L' => screen.insert_lines(param(params, 0, 1)),
        b'M' => screen.delete_lines(param(params, 0, 1)),
        b'P' => screen.delete_chars(param(params, 0, 1)),
        b'S' => {
            screen.set_scroll_region(0, screen.rows().saturating_sub(1));
            screen.delete_lines(param(params, 0, 1));
        }
        b'T' => {
            screen.set_scroll_region(0, screen.rows().saturating_sub(1));
            screen.insert_lines(param(params, 0, 1));
        }
        b'X' => screen.erase_chars(param(params, 0, 1)),
        b'd' => screen.move_to(param(params, 0, 1).saturating_sub(1), screen.cursor().1),
        b'h' if private => set_private_modes(screen, params, true),
        b'l' if private => set_private_modes(screen, params, false),
        b'm' => apply_sgr(screen, params),
        b'r' => screen.set_scroll_region(
            param(params, 0, 1).saturating_sub(1),
            param(params, 1, screen.rows()).saturating_sub(1),
        ),
        b's' => screen.save_cursor(),
        b'u' => screen.restore_cursor(),
        _ => {}
    }
}

fn set_private_modes(screen: &mut Screen, params: &[u16], enabled: bool) {
    for mode in params {
        match mode {
            25 => screen.set_cursor_visible(enabled),
            47 | 1047 | 1049 => screen.set_alternate(enabled),
            2004 => screen.set_bracketed_paste(enabled),
            2026 => screen.set_synchronized_output(enabled),
            _ => {}
        }
    }
}

fn apply_sgr(screen: &mut Screen, params: &[u16]) {
    let params = if params.is_empty() { &[0][..] } else { params };
    let mut index = 0;
    while index < params.len() {
        match params[index] {
            0 => screen.reset_style(),
            1 => screen.set_bold(true),
            2 => screen.set_dim(true),
            3 => screen.set_italic(true),
            4 => screen.set_underline(true),
            7 => screen.set_reverse(true),
            8 => screen.set_hidden(true),
            9 => screen.set_strikethrough(true),
            22 => {
                screen.set_bold(false);
                screen.set_dim(false);
            }
            23 => screen.set_italic(false),
            24 => screen.set_underline(false),
            27 => screen.set_reverse(false),
            28 => screen.set_hidden(false),
            29 => screen.set_strikethrough(false),
            30..=37 => screen.set_fg(Color::Indexed((params[index] - 30) as u8)),
            39 => screen.set_fg(Color::Default),
            40..=47 => screen.set_bg(Color::Indexed((params[index] - 40) as u8)),
            49 => screen.set_bg(Color::Default),
            90..=97 => screen.set_fg(Color::Indexed((params[index] - 82) as u8)),
            100..=107 => screen.set_bg(Color::Indexed((params[index] - 92) as u8)),
            38 | 48 => {
                let foreground = params[index] == 38;
                if let Some((color, consumed)) = parse_color(&params[index + 1..]) {
                    if foreground {
                        screen.set_fg(color);
                    } else {
                        screen.set_bg(color);
                    }
                    index += consumed;
                }
            }
            _ => {}
        }
        index += 1;
    }
}

fn parse_color(params: &[u16]) -> Option<(Color, usize)> {
    match params {
        [5, index, ..] => Some((Color::Indexed((*index).min(255) as u8), 2)),
        [2, red, green, blue, ..] => Some((
            Color::Rgb(
                (*red).min(255) as u8,
                (*green).min(255) as u8,
                (*blue).min(255) as u8,
            ),
            4,
        )),
        _ => None,
    }
}
