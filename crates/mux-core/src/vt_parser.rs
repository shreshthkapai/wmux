use std::fmt;

use crate::screen::Screen;
use vte::{Params, Perform};

pub struct VtParser {
    parser: vte::Parser,
}

impl VtParser {
    pub fn new() -> Self {
        Self {
            parser: vte::Parser::new(),
        }
    }

    pub fn feed(&mut self, screen: &mut Screen, bytes: &[u8]) {
        let mut performer = ScreenPerformer { screen };
        for byte in bytes {
            self.parser.advance(&mut performer, *byte);
        }
    }
}

impl Default for VtParser {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Debug for VtParser {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("VtParser").finish_non_exhaustive()
    }
}

struct ScreenPerformer<'a> {
    screen: &'a mut Screen,
}

impl ScreenPerformer<'_> {
    fn params(params: &Params) -> Vec<u16> {
        params
            .iter()
            .map(|param| param.first().copied().unwrap_or(0))
            .collect()
    }

    fn param_or(params: &[u16], index: usize, default: u16) -> u16 {
        match params.get(index).copied() {
            Some(0) | None => default,
            Some(value) => value,
        }
    }

    fn set_modes(&mut self, params: &[u16], enabled: bool) {
        for mode in params {
            match *mode {
                47 | 1047 | 1049 => self.screen.set_alternate_screen(enabled),
                2004 => self.screen.set_bracketed_paste(enabled),
                _ => {}
            }
        }
    }
}

impl Perform for ScreenPerformer<'_> {
    fn print(&mut self, c: char) {
        self.screen.write_char(c);
    }

    fn execute(&mut self, byte: u8) {
        match byte {
            0x07 => {}
            0x08 => self.screen.backspace(),
            0x09 => self.screen.tab(),
            0x0a..=0x0c => self.screen.linefeed(),
            0x0d => self.screen.carriage_return(),
            0x0e | 0x0f => {}
            _ => {}
        }
    }

    fn osc_dispatch(&mut self, _params: &[&[u8]], _bell_terminated: bool) {}

    fn esc_dispatch(&mut self, _intermediates: &[u8], ignore: bool, byte: u8) {
        if ignore {
            return;
        }

        match byte {
            b'7' => self.screen.save_cursor(),
            b'8' => self.screen.restore_cursor(),
            b'c' => self.screen.clear_screen(),
            b'D' => self.screen.linefeed(),
            b'E' => self.screen.newline(),
            b'M' => self.screen.move_cursor_up(1),
            _ => {}
        }
    }

    fn csi_dispatch(&mut self, params: &Params, _intermediates: &[u8], ignore: bool, action: char) {
        if ignore {
            return;
        }

        let params = Self::params(params);
        match action {
            '@' => self
                .screen
                .insert_blank_chars(Self::param_or(&params, 0, 1)),
            'A' => self.screen.move_cursor_up(Self::param_or(&params, 0, 1)),
            'B' | 'e' => self.screen.move_cursor_down(Self::param_or(&params, 0, 1)),
            'C' | 'a' => self.screen.move_cursor_right(Self::param_or(&params, 0, 1)),
            'D' => self.screen.move_cursor_left(Self::param_or(&params, 0, 1)),
            'E' => {
                self.screen.move_cursor_down(Self::param_or(&params, 0, 1));
                self.screen.carriage_return();
            }
            'F' => {
                self.screen.move_cursor_up(Self::param_or(&params, 0, 1));
                self.screen.carriage_return();
            }
            'G' => {
                let column = Self::param_or(&params, 0, 1).saturating_sub(1);
                self.screen.move_cursor_to(self.screen.cursor_row(), column);
            }
            'H' | 'f' => {
                let row = Self::param_or(&params, 0, 1).saturating_sub(1);
                let column = Self::param_or(&params, 1, 1).saturating_sub(1);
                self.screen.move_cursor_to(row, column);
            }
            'J' => match Self::param_or(&params, 0, 0) {
                0 => self.screen.clear_to_end_of_screen(),
                1 => self.screen.clear_to_start_of_screen(),
                2 | 3 => self.screen.clear_screen(),
                _ => {}
            },
            'K' => match Self::param_or(&params, 0, 0) {
                0 => self.screen.clear_line_right(),
                1 => self.screen.clear_line_left(),
                2 => self.screen.clear_line(),
                _ => {}
            },
            'L' => self
                .screen
                .insert_blank_lines(Self::param_or(&params, 0, 1)),
            'M' => self.screen.delete_lines(Self::param_or(&params, 0, 1)),
            'P' => self.screen.delete_chars(Self::param_or(&params, 0, 1)),
            'S' => self.screen.scroll_up(Self::param_or(&params, 0, 1)),
            'T' => self.screen.scroll_down(Self::param_or(&params, 0, 1)),
            'X' => self.screen.erase_chars(Self::param_or(&params, 0, 1)),
            'd' => {
                let row = Self::param_or(&params, 0, 1).saturating_sub(1);
                self.screen.move_cursor_to(row, self.screen.cursor_column());
            }
            'h' => self.set_modes(&params, true),
            'l' => self.set_modes(&params, false),
            'm' => {}
            'r' => {
                if params.is_empty() {
                    self.screen.reset_scroll_region();
                } else {
                    let top = Self::param_or(&params, 0, 1).saturating_sub(1);
                    let bottom = Self::param_or(&params, 1, self.screen.rows()).saturating_sub(1);
                    self.screen.set_scroll_region(top, bottom);
                }
            }
            's' => self.screen.save_cursor(),
            'u' => self.screen.restore_cursor(),
            _ => {}
        }
    }
}

#[cfg(test)]
mod tests {
    use super::VtParser;
    use crate::screen::Screen;

    fn feed(bytes: &[u8]) -> Screen {
        let mut parser = VtParser::new();
        let mut screen = Screen::new(20, 5);
        parser.feed(&mut screen, bytes);
        screen
    }

    #[test]
    fn parses_utf8_printable_text() {
        let screen = feed("hello \u{03bb}".as_bytes());

        assert_eq!(
            screen.grid().line(0).unwrap().visible_text(),
            "hello \u{03bb}"
        );
        assert_eq!(screen.cursor_row(), 0);
        assert_eq!(screen.cursor_column(), 7);
    }

    #[test]
    fn parses_split_utf8_across_chunks() {
        let mut parser = VtParser::new();
        let mut screen = Screen::new(10, 2);
        parser.feed(&mut screen, &[0xce]);
        parser.feed(&mut screen, &[0xbb]);

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "\u{03bb}");
        assert_eq!(screen.cursor_column(), 1);
    }

    #[test]
    fn parses_cursor_movement_and_erase_line() {
        let screen = feed(b"abcdef\x1b[3D\x1b[K");

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "abc");
        assert_eq!(screen.cursor_column(), 3);
    }

    #[test]
    fn parses_line_editing_sequences() {
        let screen = feed(b"abcdef\x1b[3D\x1b[2P");

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "abcf");
        assert_eq!(screen.cursor_column(), 3);
    }

    #[test]
    fn parses_scroll_region_and_line_operations() {
        let screen = feed(b"one\r\ntwo\r\nthree\r\nfour\x1b[2;4r\x1b[2;1H\x1b[M");

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "one");
        assert_eq!(screen.grid().line(1).unwrap().visible_text(), "three");
        assert_eq!(screen.grid().line(2).unwrap().visible_text(), "four");
        assert_eq!(screen.grid().line(3).unwrap().visible_text(), "");
    }

    #[test]
    fn tracks_alternate_screen_private_mode() {
        let mut parser = VtParser::new();
        let mut screen = Screen::new(20, 5);

        parser.feed(&mut screen, b"primary");
        parser.feed(&mut screen, b"\x1b[?1049h");
        parser.feed(&mut screen, b"alternate");
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "alternate");

        parser.feed(&mut screen, b"\x1b[?1049l");
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "primary");
    }

    #[test]
    fn skips_osc_title_sequences() {
        let screen = feed(b"before\x1b]0;ignored title\x07after");

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "beforeafter");
    }

    #[test]
    fn tracks_bracketed_paste_private_mode() {
        let mut parser = VtParser::new();
        let mut screen = Screen::new(20, 5);

        parser.feed(&mut screen, b"\x1b[?2004h");
        assert!(screen.bracketed_paste());

        parser.feed(&mut screen, b"\x1b[?2004l");
        assert!(!screen.bracketed_paste());
    }
}
