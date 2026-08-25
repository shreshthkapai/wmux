use std::fmt;

use smallvec::SmallVec;
use vte::{Params, Perform};

use crate::screen::{DamageOperation, InsertDeleteKind, MAX_TITLE_BYTES};
use crate::{Color, CursorStyle, Screen};

const MAX_CSI_PARAMS: usize = 32;
pub const MAX_OSC_BYTES: usize = 1024;
const _: () = assert!(MAX_TITLE_BYTES < MAX_OSC_BYTES);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CsiParams {
    values: [u16; MAX_CSI_PARAMS],
    len: u8,
}

impl CsiParams {
    const fn empty() -> Self {
        Self {
            values: [0; MAX_CSI_PARAMS],
            len: 0,
        }
    }

    fn from_vte(params: &Params) -> Self {
        let mut result = Self::empty();
        for value in params.iter().flatten().copied() {
            if usize::from(result.len) == MAX_CSI_PARAMS {
                break;
            }
            result.values[usize::from(result.len)] = value;
            result.len += 1;
        }
        result
    }

    pub fn as_slice(&self) -> &[u16] {
        &self.values[..usize::from(self.len)]
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TerminalOperation {
    PrintRun {
        start: u32,
        len: u32,
    },
    Execute(u8),
    InsertChars(u16),
    MoveUp(u16),
    MoveDown(u16),
    MoveRight(u16),
    MoveLeft(u16),
    NextLine(u16),
    PreviousLine(u16),
    MoveColumn(u16),
    MoveTo {
        row: u16,
        col: u16,
    },
    ClearScreen(u16),
    ClearLine(u16),
    InsertLines(u16),
    DeleteLines(u16),
    DeleteChars(u16),
    ScrollUp(u16),
    ScrollDown(u16),
    ReverseIndex,
    EraseChars(u16),
    MoveRow(u16),
    ModeChange {
        private: bool,
        enabled: bool,
        modes: CsiParams,
    },
    Style(CsiParams),
    CursorStyle(u16),
    SetScrollRegion {
        top: u16,
        bottom: u16,
    },
    SaveCursor,
    RestoreCursor,
    SetTitle(String),
    Reset,
}

pub type TerminalEvent = TerminalOperation;

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct TerminalBatch {
    text: String,
    operations: SmallVec<[TerminalOperation; 32]>,
}

impl TerminalBatch {
    pub fn operations(&self) -> &[TerminalOperation] {
        &self.operations
    }

    fn push_print(&mut self, ch: char) {
        let start = self.text.len();
        self.text.push(ch);
        let added = self.text.len() - start;
        if let Some(TerminalOperation::PrintRun { start: _, len }) = self.operations.last_mut() {
            *len += added as u32;
        } else {
            self.operations.push(TerminalOperation::PrintRun {
                start: start as u32,
                len: added as u32,
            });
        }
    }

    fn push(&mut self, operation: TerminalOperation) {
        self.operations.push(operation);
    }

    fn print_text(&self, start: u32, len: u32) -> &str {
        let start = start as usize;
        &self.text[start..start + len as usize]
    }

    fn clear(&mut self) {
        self.text.clear();
        self.operations.clear();
    }
}

impl Perform for TerminalBatch {
    fn print(&mut self, ch: char) {
        self.push_print(ch);
    }

    fn execute(&mut self, byte: u8) {
        self.push(TerminalOperation::Execute(byte));
    }

    fn csi_dispatch(&mut self, params: &Params, intermediates: &[u8], ignore: bool, action: char) {
        if ignore {
            return;
        }
        let params = CsiParams::from_vte(params);
        let values = params.as_slice();
        let private = intermediates.contains(&b'?');
        let operation = match action as u8 {
            b'@' => TerminalOperation::InsertChars(param(values, 0, 1)),
            b'A' => TerminalOperation::MoveUp(param(values, 0, 1)),
            b'B' | b'e' => TerminalOperation::MoveDown(param(values, 0, 1)),
            b'C' | b'a' => TerminalOperation::MoveRight(param(values, 0, 1)),
            b'D' => TerminalOperation::MoveLeft(param(values, 0, 1)),
            b'E' => TerminalOperation::NextLine(param(values, 0, 1)),
            b'F' => TerminalOperation::PreviousLine(param(values, 0, 1)),
            b'G' => TerminalOperation::MoveColumn(param(values, 0, 1).saturating_sub(1)),
            b'H' | b'f' => TerminalOperation::MoveTo {
                row: param(values, 0, 1).saturating_sub(1),
                col: param(values, 1, 1).saturating_sub(1),
            },
            b'J' => TerminalOperation::ClearScreen(param(values, 0, 0)),
            b'K' => TerminalOperation::ClearLine(param(values, 0, 0)),
            b'L' => TerminalOperation::InsertLines(param(values, 0, 1)),
            b'M' => TerminalOperation::DeleteLines(param(values, 0, 1)),
            b'P' => TerminalOperation::DeleteChars(param(values, 0, 1)),
            b'S' => TerminalOperation::ScrollUp(param(values, 0, 1)),
            b'T' => TerminalOperation::ScrollDown(param(values, 0, 1)),
            b'X' => TerminalOperation::EraseChars(param(values, 0, 1)),
            b'd' => TerminalOperation::MoveRow(param(values, 0, 1).saturating_sub(1)),
            b'h' | b'l' => TerminalOperation::ModeChange {
                private,
                enabled: action == 'h',
                modes: params,
            },
            b'm' => TerminalOperation::Style(params),
            b'q' if intermediates == b" " => {
                TerminalOperation::CursorStyle(values.first().copied().unwrap_or(0))
            }
            b'r' => TerminalOperation::SetScrollRegion {
                top: param(values, 0, 1).saturating_sub(1),
                bottom: param(values, 1, u16::MAX).saturating_sub(1),
            },
            b's' => TerminalOperation::SaveCursor,
            b'u' => TerminalOperation::RestoreCursor,
            _ => return,
        };
        self.push(operation);
    }

    fn esc_dispatch(&mut self, intermediates: &[u8], ignore: bool, byte: u8) {
        if ignore || !intermediates.is_empty() {
            return;
        }
        let operation = match byte {
            b'7' => TerminalOperation::SaveCursor,
            b'8' => TerminalOperation::RestoreCursor,
            b'D' => TerminalOperation::Execute(b'\n'),
            b'E' => TerminalOperation::Execute(0x85),
            b'M' => TerminalOperation::ReverseIndex,
            b'c' => TerminalOperation::Reset,
            _ => return,
        };
        self.push(operation);
    }

    fn put(&mut self, _byte: u8) {
        // DCS payloads are intentionally discard-only. The core does not
        // accumulate platform/device control strings it does not implement.
    }

    fn osc_dispatch(&mut self, _params: &[&[u8]], _bell_terminated: bool) {
        // `vte` exposes at most 16 semicolon-delimited OSC parameters. The
        // bounded sidecar scanner in `TerminalEngine` owns title extraction so
        // title text after that parser limit is not silently truncated.
    }
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
enum OscScanState {
    #[default]
    Ground,
    Escape,
    Osc,
    OscEscape,
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
struct OscTitleScanner {
    state: OscScanState,
    raw: Vec<u8>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct OscTitleEvent {
    end: usize,
    title: String,
}

impl OscTitleScanner {
    fn scan(&mut self, bytes: &[u8]) -> SmallVec<[OscTitleEvent; 2]> {
        let mut events = SmallVec::new();
        for (index, byte) in bytes.iter().copied().enumerate() {
            match self.state {
                OscScanState::Ground => {
                    if byte == 0x1b {
                        self.state = OscScanState::Escape;
                    }
                }
                OscScanState::Escape => match byte {
                    b']' => self.begin_osc(),
                    0x1b => {}
                    _ => self.state = OscScanState::Ground,
                },
                OscScanState::Osc => match byte {
                    0x07 => {
                        if let Some(title) = self.finish_osc() {
                            events.push(OscTitleEvent {
                                end: index + 1,
                                title,
                            });
                        }
                    }
                    0x1b => self.state = OscScanState::OscEscape,
                    0x18 | 0x1a => {
                        self.raw.clear();
                        self.state = OscScanState::Ground;
                    }
                    0x20..=0xff if self.raw.len() < MAX_OSC_BYTES => self.raw.push(byte),
                    _ => {}
                },
                OscScanState::OscEscape => match byte {
                    b'\\' => {
                        if let Some(title) = self.finish_osc() {
                            events.push(OscTitleEvent {
                                end: index + 1,
                                title,
                            });
                        }
                    }
                    b']' => self.begin_osc(),
                    0x1b => {}
                    _ => {
                        self.raw.clear();
                        self.state = OscScanState::Ground;
                    }
                },
            }
        }
        events
    }

    fn begin_osc(&mut self) {
        self.raw.clear();
        self.state = OscScanState::Osc;
    }

    fn finish_osc(&mut self) -> Option<String> {
        self.state = OscScanState::Ground;
        let title_start = if matches!(self.raw.as_slice(), b"0" | b"2") {
            self.raw.len()
        } else {
            let Some(separator) = self.raw.iter().position(|byte| *byte == b';') else {
                self.raw.clear();
                return None;
            };
            if !matches!(&self.raw[..separator], b"0" | b"2") {
                self.raw.clear();
                return None;
            }
            separator + 1
        };
        let mut title = String::from_utf8_lossy(&self.raw[title_start..]).into_owned();
        self.raw.clear();
        truncate_utf8(&mut title, MAX_TITLE_BYTES);
        Some(title)
    }
}

pub struct TerminalEngine {
    parser: vte::Parser<MAX_OSC_BYTES>,
    batch: TerminalBatch,
    title_scanner: OscTitleScanner,
}

impl fmt::Debug for TerminalEngine {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("TerminalEngine")
            .finish_non_exhaustive()
    }
}

impl TerminalEngine {
    pub fn new() -> Self {
        Self {
            parser: vte::Parser::new_with_size(),
            batch: TerminalBatch::default(),
            title_scanner: OscTitleScanner::default(),
        }
    }

    pub fn feed(&mut self, screen: &mut Screen, bytes: &[u8]) -> Option<u64> {
        if bytes.is_empty() {
            return None;
        }
        self.batch.clear();
        let titles = self.title_scanner.scan(bytes);
        let mut start = 0;
        for event in titles {
            self.parser
                .advance(&mut self.batch, &bytes[start..event.end]);
            self.batch.push(TerminalOperation::SetTitle(event.title));
            start = event.end;
        }
        self.parser.advance(&mut self.batch, &bytes[start..]);
        if self.batch.operations.is_empty() {
            return None;
        }
        apply_batch(screen, &self.batch)
    }
}

impl Default for TerminalEngine {
    fn default() -> Self {
        Self::new()
    }
}

fn truncate_utf8(text: &mut String, max_bytes: usize) {
    if text.len() <= max_bytes {
        return;
    }
    let mut end = max_bytes;
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    text.truncate(end);
}

fn param(params: &[u16], index: usize, default: u16) -> u16 {
    match params.get(index).copied() {
        Some(0) | None => default,
        Some(value) => value,
    }
}

fn apply_batch(screen: &mut Screen, batch: &TerminalBatch) -> Option<u64> {
    screen.begin_update();
    for operation in &batch.operations {
        let before = screen.cursor();
        match operation {
            TerminalOperation::PrintRun { start, len } => {
                if screen.put_run(batch.print_text(*start, *len)) {
                    screen.record_damage(DamageOperation::PrintRun {
                        start: before,
                        end: screen.cursor(),
                    });
                }
            }
            TerminalOperation::Execute(byte) => match *byte {
                b'\x08' => screen.backspace(),
                b'\t' => screen.tab(),
                b'\n' | 0x0b | 0x0c => screen.linefeed(),
                b'\r' => screen.carriage_return(),
                0x85 => screen.newline(),
                _ => {}
            },
            TerminalOperation::InsertChars(count) => screen.insert_blank_chars(*count),
            TerminalOperation::MoveUp(count) => screen.move_up(*count),
            TerminalOperation::MoveDown(count) => screen.move_down(*count),
            TerminalOperation::MoveRight(count) => screen.move_right(*count),
            TerminalOperation::MoveLeft(count) => screen.move_left(*count),
            TerminalOperation::NextLine(count) => {
                screen.move_down(*count);
                screen.carriage_return();
            }
            TerminalOperation::PreviousLine(count) => {
                screen.move_up(*count);
                screen.carriage_return();
            }
            TerminalOperation::MoveColumn(col) => screen.move_to(screen.cursor().0, *col),
            TerminalOperation::MoveTo { row, col } => screen.move_to(*row, *col),
            TerminalOperation::ClearScreen(mode) => screen.clear_screen_mode(*mode),
            TerminalOperation::ClearLine(mode) => screen.clear_line(*mode),
            TerminalOperation::InsertLines(count) => screen.insert_lines(*count),
            TerminalOperation::DeleteLines(count) => screen.delete_lines(*count),
            TerminalOperation::DeleteChars(count) => screen.delete_chars(*count),
            TerminalOperation::ScrollUp(count) => screen.scroll_up(*count),
            TerminalOperation::ScrollDown(count) => screen.scroll_down(*count),
            TerminalOperation::ReverseIndex => screen.reverse_index(),
            TerminalOperation::EraseChars(count) => screen.erase_chars(*count),
            TerminalOperation::MoveRow(row) => screen.move_to(*row, screen.cursor().1),
            TerminalOperation::ModeChange {
                private,
                enabled,
                modes,
            } => {
                if *private {
                    set_private_modes(screen, modes.as_slice(), *enabled);
                }
            }
            TerminalOperation::Style(params) => apply_sgr(screen, params.as_slice()),
            TerminalOperation::CursorStyle(value) => {
                if let Some(style) = CursorStyle::from_decscusr(*value) {
                    screen.set_cursor_style(style);
                }
            }
            TerminalOperation::SetScrollRegion { top, bottom } => {
                screen.set_scroll_region(*top, (*bottom).min(screen.rows().saturating_sub(1)));
            }
            TerminalOperation::SaveCursor => screen.save_cursor(),
            TerminalOperation::RestoreCursor => screen.restore_cursor(),
            TerminalOperation::SetTitle(title) => screen.set_title(title),
            TerminalOperation::Reset => {
                screen.clear_screen_mode(2);
                screen.set_cursor_visible(true);
                screen.set_cursor_style(CursorStyle::Default);
            }
        }
        record_operation_damage(screen, operation, before);
    }
    screen.finish_update()
}

fn record_operation_damage(screen: &mut Screen, operation: &TerminalOperation, before: (u16, u16)) {
    let after = screen.cursor();
    let damage = match operation {
        TerminalOperation::PrintRun { .. } => return,
        TerminalOperation::ClearScreen(_) | TerminalOperation::Reset => DamageOperation::Full,
        TerminalOperation::ClearLine(mode) => {
            let (start, end) = match mode {
                1 => (0, before.1.saturating_add(1)),
                2 => (0, screen.cols()),
                _ => (before.1, screen.cols()),
            };
            DamageOperation::ClearRange {
                row: before.0,
                start,
                end,
            }
        }
        TerminalOperation::EraseChars(count) => DamageOperation::ClearRange {
            row: before.0,
            start: before.1,
            end: before.1.saturating_add(*count).min(screen.cols()),
        },
        TerminalOperation::InsertChars(count) => DamageOperation::InsertDelete {
            row: before.0,
            col: before.1,
            count: *count,
            kind: InsertDeleteKind::InsertChars,
        },
        TerminalOperation::DeleteChars(count) => DamageOperation::InsertDelete {
            row: before.0,
            col: before.1,
            count: *count,
            kind: InsertDeleteKind::DeleteChars,
        },
        TerminalOperation::InsertLines(count) => {
            let (_, bottom) = screen.scroll_region();
            DamageOperation::ScrollRegion {
                top: before.0,
                bottom,
                lines: -(*count as i32),
            }
        }
        TerminalOperation::DeleteLines(count) => {
            let (_, bottom) = screen.scroll_region();
            DamageOperation::ScrollRegion {
                top: before.0,
                bottom,
                lines: i32::from(*count),
            }
        }
        TerminalOperation::ScrollDown(count) => {
            let (top, bottom) = screen.scroll_region();
            DamageOperation::ScrollRegion {
                top,
                bottom,
                lines: -(*count as i32),
            }
        }
        TerminalOperation::ScrollUp(count) => {
            let (top, bottom) = screen.scroll_region();
            DamageOperation::ScrollRegion {
                top,
                bottom,
                lines: i32::from(*count),
            }
        }
        TerminalOperation::ReverseIndex if before.0 == screen.scroll_region().0 => {
            let (top, bottom) = screen.scroll_region();
            DamageOperation::ScrollRegion {
                top,
                bottom,
                lines: -1,
            }
        }
        TerminalOperation::Execute(byte)
            if matches!(*byte, b'\n' | 0x0b | 0x0c | 0x85)
                && before.0 == screen.scroll_region().1 =>
        {
            let (top, bottom) = screen.scroll_region();
            DamageOperation::ScrollRegion {
                top,
                bottom,
                lines: 1,
            }
        }
        TerminalOperation::ModeChange { modes, enabled, .. } => {
            for mode in modes.as_slice() {
                screen.record_damage(DamageOperation::ModeChange {
                    mode: *mode,
                    enabled: *enabled,
                });
            }
            return;
        }
        TerminalOperation::Style(_) | TerminalOperation::CursorStyle(_) => {
            DamageOperation::ModeChange {
                mode: 0,
                enabled: true,
            }
        }
        TerminalOperation::SetTitle(_) => DamageOperation::TitleChange,
        _ => DamageOperation::CursorMove {
            from: before,
            to: after,
        },
    };
    screen.record_damage(damage);
}

fn set_private_modes(screen: &mut Screen, params: &[u16], enabled: bool) {
    for mode in params {
        match *mode {
            25 => screen.set_cursor_visible(enabled),
            47 => screen.switch_alternate(enabled, false),
            1047 => screen.switch_alternate(enabled, !enabled),
            1048 if enabled => screen.save_cursor(),
            1048 => screen.restore_cursor(),
            1049 if enabled && !screen.alternate_active() => {
                screen.save_cursor();
                screen.switch_alternate(true, true);
            }
            1049 if !enabled => {
                screen.switch_alternate(false, true);
                screen.restore_cursor();
            }
            1049 => {}
            9 | 1000 | 1002 | 1003 | 1005 | 1006 | 1015 => screen.set_mouse_mode(*mode, enabled),
            1007 => screen.set_alternate_scroll(enabled),
            2004 => screen.set_bracketed_paste(enabled),
            2026 => screen.set_synchronized_output(enabled),
            9001 => screen.set_win32_input_mode(enabled),
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
            90..=97 => screen.set_fg(Color::Indexed((params[index] - 90 + 8) as u8)),
            100..=107 => screen.set_bg(Color::Indexed((params[index] - 100 + 8) as u8)),
            38 | 48 => {
                let is_fg = params[index] == 38;
                if let Some((color, consumed)) = parse_sgr_color(&params[index + 1..]) {
                    if is_fg {
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

fn parse_sgr_color(params: &[u16]) -> Option<(Color, usize)> {
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

#[cfg(test)]
mod tests {
    use super::{TerminalEngine, TerminalOperation};
    use crate::{Color, CursorStyle, Screen, MAX_TITLE_BYTES};
    use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};

    fn run(bytes: &[u8]) -> Screen {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);
        engine.feed(&mut screen, bytes);
        screen
    }

    #[test]
    fn parses_prompt_and_command_echo() {
        let screen = run(b"C:\\wmux>abc\x08 \x08d");

        assert_eq!(screen.grid().line(0).unwrap().text(), "C:\\wmux>abd");
        assert_eq!(screen.cursor(), (0, 11));
    }

    #[test]
    fn coalesces_printable_ascii_into_a_semantic_run() {
        let mut parser = vte::Parser::new();
        let mut batch = super::TerminalBatch::default();
        parser.advance(&mut batch, b"a printable run\x1b[2Dtail");

        assert_eq!(
            batch
                .operations()
                .iter()
                .filter(|operation| matches!(operation, TerminalOperation::PrintRun { .. }))
                .count(),
            2
        );
        assert!(matches!(
            batch.operations()[1],
            TerminalOperation::MoveLeft(2)
        ));
    }

    #[test]
    fn malformed_and_oversized_control_sequences_are_safely_ignored() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);
        engine.feed(
            &mut screen,
            b"before\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33m-after",
        );

        assert_eq!(screen.grid().line(0).unwrap().text(), "before-after");
    }

    #[test]
    fn malformed_large_csi_parameters_do_not_overflow() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);

        engine.feed(&mut screen, b"x\x1b[65535C");
        assert_eq!(screen.cursor(), (0, 19));

        engine.feed(&mut screen, b"\n\x1b[65535B");
        assert_eq!(screen.cursor(), (4, 19));
    }

    #[test]
    fn top_anchored_scroll_region_preserves_inline_agent_history() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);
        engine.feed(
            &mut screen,
            b"old-one\r\nold-two\r\nold-three\r\ncomposer\r\nstatus",
        );

        // Inline terminal applications finalize transcript rows above their
        // live viewport by scrolling a top-anchored DEC region. Rows leaving
        // the top of that region are terminal history, even when the region's
        // bottom is above the physical screen bottom.
        engine.feed(
            &mut screen,
            b"\x1b[1;3r\x1b[3;1H\r\nagent-one\r\nagent-two\x1b[r",
        );

        assert_eq!(screen.max_viewport_offset(20), 2);
        assert_eq!(
            screen
                .viewport_lines(20, 5, 2)
                .iter()
                .map(crate::Line::text)
                .collect::<Vec<_>>(),
            ["old-one", "old-two", "old-three", "agent-one", "agent-two"]
        );
    }

    #[test]
    fn erase_saved_lines_drops_scrollback_without_touching_the_live_screen() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(8, 2);
        engine.feed(&mut screen, b"old-one\r\nold-two\r\nlive");
        assert!(screen.grid().history_len() > 0);

        engine.feed(&mut screen, b"\x1b[3J");

        assert_eq!(screen.grid().history_len(), 0);
        assert_eq!(screen.grid().line(0).unwrap().text(), "old-two");
        assert_eq!(screen.grid().line(1).unwrap().text(), "live");
        assert_eq!(screen.cursor(), (1, 4));
    }

    #[test]
    fn shell_clear_sequence_cannot_reflow_erased_history_back_into_the_pane() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 3);
        engine.feed(
            &mut screen,
            b"stale-output-one\r\nstale-output-two\r\nold-prompt",
        );

        engine.feed(&mut screen, b"\x1b[H\x1b[2J\x1b[3Jnew-prompt");
        screen.resize(6, 3);

        assert_eq!(screen.grid().history_len(), 0);
        assert_eq!(screen.grid().line(0).unwrap().text(), "new-pr");
        assert_eq!(screen.grid().line(1).unwrap().text(), "ompt");
        assert_eq!(screen.grid().line(2).unwrap().text(), "");
    }

    #[test]
    fn explicit_scroll_up_preserves_top_anchored_region_history() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);
        engine.feed(
            &mut screen,
            b"old-one\r\nold-two\r\nold-three\r\ncomposer\r\nstatus",
        );

        engine.feed(&mut screen, b"\x1b[1;3r\x1b[2S\x1b[r");

        assert_eq!(screen.max_viewport_offset(20), 2);
        assert_eq!(screen.grid().line(0).unwrap().text(), "old-three");
        assert_eq!(screen.grid().line(3).unwrap().text(), "composer");
        assert_eq!(screen.grid().line(4).unwrap().text(), "status");
    }

    #[test]
    fn reverse_index_scrolls_down_at_the_top_margin() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);
        engine.feed(
            &mut screen,
            b"one\r\ntwo\r\nthree\r\nfour\r\nfive\x1b[2;4r\x1b[2;1H\x1bM",
        );

        assert_eq!(screen.cursor(), (1, 0));
        assert_eq!(screen.grid().line(0).unwrap().text(), "one");
        assert_eq!(screen.grid().line(1).unwrap().text(), "");
        assert_eq!(screen.grid().line(2).unwrap().text(), "two");
        assert_eq!(screen.grid().line(3).unwrap().text(), "three");
        assert_eq!(screen.grid().line(4).unwrap().text(), "five");
    }

    #[test]
    fn handles_cursor_clear_and_osc_title() {
        let screen = run(b"\x1b]0;ignored\x07hello\x1b[3D\x1b[K");

        assert_eq!(screen.grid().line(0).unwrap().text(), "he");
        assert_eq!(screen.cursor(), (0, 2));
        assert_eq!(screen.title(), "ignored");
    }

    #[test]
    fn osc_zero_and_two_update_a_bounded_authoritative_title() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);
        engine.feed(&mut screen, b"\x1b]0;first;section\x07");
        assert_eq!(screen.title(), "first;section");

        let long = format!("\x1b]2;{}\x1b\\", "x".repeat(MAX_TITLE_BYTES + 40));
        engine.feed(&mut screen, long.as_bytes());
        assert_eq!(screen.title().len(), MAX_TITLE_BYTES);
        assert_eq!(screen.title(), "x".repeat(MAX_TITLE_BYTES));

        let unicode = format!("\x1b]2;{}\x07", "\u{754c}".repeat(200));
        engine.feed(&mut screen, unicode.as_bytes());
        assert_eq!(screen.title(), "\u{754c}".repeat(MAX_TITLE_BYTES / 3));
        assert_eq!(screen.title().len(), 510);
    }

    #[test]
    fn osc_title_preserves_content_beyond_vte_parameter_limit() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);
        let title = (0..40)
            .map(|index| format!("part-{index}"))
            .collect::<Vec<_>>()
            .join(";");
        let sequence = format!("\x1b]2;{title}\x1b\\");

        for chunk in sequence.as_bytes().chunks(7) {
            engine.feed(&mut screen, chunk);
        }

        assert_eq!(screen.title(), title);
    }

    #[test]
    fn osc_title_command_without_separator_clears_the_title() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);

        engine.feed(&mut screen, b"\x1b]2;current\x07");
        engine.feed(&mut screen, b"\x1b]0\x07");

        assert_eq!(screen.title(), "");
    }

    #[test]
    fn osc_title_preserves_utf8_continuation_bytes_in_c1_range() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);

        engine.feed(&mut screen, "\x1b]2;\u{dc}ber\x07".as_bytes());

        assert_eq!(screen.title(), "\u{dc}ber");
    }

    #[test]
    fn oversized_control_strings_recover_to_printable_ground_state() {
        let cases: &[(&str, &[u8], &[u8])] = &[
            ("DCS", b"\x1bPq", b"\x1b\\"),
            ("SOS", b"\x1bX", b"\x1b\\"),
            ("PM", b"\x1b^", b"\x1b\\"),
            ("APC", b"\x1b_", b"\x1b\\"),
            ("OSC", b"\x1b]0;", b"\x07"),
        ];

        for (name, introducer, terminator) in cases {
            let mut bytes = Vec::with_capacity(16 * 1024 + introducer.len() + terminator.len() + 5);
            bytes.extend_from_slice(introducer);
            bytes.extend(std::iter::repeat_n(b'x', 16 * 1024));
            bytes.extend_from_slice(terminator);
            bytes.extend_from_slice(b"after");

            let screen = run(&bytes);
            assert_eq!(
                screen.grid().line(0).unwrap().text(),
                "after",
                "{name} did not return to ground state"
            );
        }
    }

    #[test]
    fn tracks_decscusr_cursor_shape() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);

        engine.feed(&mut screen, b"\x1b[5 q");
        assert_eq!(screen.cursor_style(), CursorStyle::BlinkingBar);

        engine.feed(&mut screen, b"\x1b[4 q");
        assert_eq!(screen.cursor_style(), CursorStyle::SteadyUnderline);

        engine.feed(&mut screen, b"\x1b[99 q");
        assert_eq!(screen.cursor_style(), CursorStyle::SteadyUnderline);

        engine.feed(&mut screen, b"\x1b[0 q");
        assert_eq!(screen.cursor_style(), CursorStyle::Default);
    }

    #[test]
    fn handles_osc_terminated_by_string_terminator() {
        let screen = run(b"\x1b]0;ignored\x1b\\hello");

        assert_eq!(screen.grid().line(0).unwrap().text(), "hello");
        assert_eq!(screen.cursor(), (0, 5));
    }

    #[test]
    fn handles_split_utf8() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 2);
        engine.feed(&mut screen, &[0xce]);
        engine.feed(&mut screen, &[0xbb]);

        assert_eq!(screen.grid().line(0).unwrap().text(), "\u{03bb}");
    }

    #[test]
    fn combining_marks_survive_split_terminal_feeds_in_one_cell() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(8, 2);
        engine.feed(&mut screen, b"e");
        engine.feed(&mut screen, "\u{301}".as_bytes());

        let line = screen.grid().line(0).unwrap();
        assert_eq!(line.text(), "e\u{301}");
        assert_eq!(line.width_at(0), 1);
        assert_eq!(line.width_at(1), 1);
        assert_eq!(screen.cursor(), (0, 1));
    }

    #[test]
    fn variation_selector_expands_the_previous_cell_to_two_columns() {
        let screen = run("\u{2764}\u{fe0f}x".as_bytes());
        let line = screen.grid().line(0).unwrap();

        assert_eq!(line.text(), "\u{2764}\u{fe0f}x");
        assert_eq!(line.width_at(0), 2);
        assert_eq!(line.width_at(1), 0);
        assert_eq!(line.width_at(2), 1);
        assert_eq!(screen.cursor(), (0, 3));
    }

    #[test]
    fn emoji_zwj_sequence_occupies_one_wide_cell() {
        let screen = run("\u{1f469}\u{200d}\u{1f4bb}x".as_bytes());
        let line = screen.grid().line(0).unwrap();

        assert_eq!(line.text(), "\u{1f469}\u{200d}\u{1f4bb}x");
        assert_eq!(line.width_at(0), 2);
        assert_eq!(line.width_at(1), 0);
        assert_eq!(line.width_at(2), 1);
        assert_eq!(screen.cursor(), (0, 3));
    }

    #[test]
    fn emoji_modifier_stays_with_its_base_cell() {
        let screen = run("\u{1f44d}\u{1f3fd}x".as_bytes());
        let line = screen.grid().line(0).unwrap();

        assert_eq!(line.text(), "\u{1f44d}\u{1f3fd}x");
        assert_eq!(line.width_at(0), 2);
        assert_eq!(line.width_at(1), 0);
        assert_eq!(line.width_at(2), 1);
    }

    #[test]
    fn regional_indicator_pair_is_stored_as_one_flag_cell() {
        let screen = run("\u{1f1ec}\u{1f1e7}x".as_bytes());
        let line = screen.grid().line(0).unwrap();

        assert_eq!(line.text(), "\u{1f1ec}\u{1f1e7}x");
        assert_eq!(line.width_at(0), 2);
        assert_eq!(line.width_at(1), 0);
        assert_eq!(line.width_at(2), 1);
    }

    #[test]
    fn zero_width_scalar_at_column_zero_is_ignored() {
        let screen = run("\u{301}a".as_bytes());

        assert_eq!(screen.grid().line(0).unwrap().text(), "a");
        assert_eq!(screen.cursor(), (0, 1));
    }

    #[test]
    fn sgr_styles_are_stored_per_cell() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 2);

        engine.feed(&mut screen, b"\x1b[1;3;4;31;48;5;24mX\x1b[0mY");

        let line = screen.grid().line(0).unwrap();
        let x_style = line.cell(0).unwrap().style();
        assert!(x_style.bold);
        assert!(x_style.italic);
        assert!(x_style.underline);
        assert_eq!(x_style.fg, Color::Indexed(1));
        assert_eq!(x_style.bg, Color::Indexed(24));
        assert_eq!(line.cell(1).unwrap().style().fg, Color::Default);
    }

    #[test]
    fn sgr_39_and_49_restore_explicit_terminal_defaults() {
        let screen = run(b"\x1b[31;44mx\x1b[39;49my");
        let line = screen.grid().line(0).unwrap();

        assert_eq!(line.cell(0).unwrap().style().fg, Color::Indexed(1));
        assert_eq!(line.cell(0).unwrap().style().bg, Color::Indexed(4));
        assert_eq!(line.cell(1).unwrap().style().fg, Color::Default);
        assert_eq!(line.cell(1).unwrap().style().bg, Color::Default);
    }

    #[test]
    fn sgr_rgb_color_is_stored_per_cell() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 2);

        engine.feed(&mut screen, b"\x1b[38;2;10;20;30mR");

        assert_eq!(
            screen.grid().line(0).unwrap().cell(0).unwrap().style().fg,
            Color::Rgb(10, 20, 30)
        );
    }

    #[test]
    fn printable_at_last_column_sets_pending_wrap() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(5, 2);

        engine.feed(&mut screen, b"abcde");
        assert_eq!(screen.grid().line(0).unwrap().text(), "abcde");
        assert_eq!(screen.grid().line(1).unwrap().text(), "");
        assert_eq!(screen.cursor(), (0, 4));

        engine.feed(&mut screen, b"f");
        assert_eq!(screen.grid().line(0).unwrap().text(), "abcde");
        assert!(screen.grid().line(0).unwrap().is_wrapped());
        assert_eq!(screen.grid().line(1).unwrap().text(), "f");
        assert_eq!(screen.cursor(), (1, 1));
    }

    #[test]
    fn wide_cells_occupy_two_columns() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(6, 2);

        engine.feed(&mut screen, "a界b".as_bytes());

        let line = screen.grid().line(0).unwrap();
        assert_eq!(line.text(), "a界b");
        assert_eq!(line.width_at(0), 1);
        assert_eq!(line.width_at(1), 2);
        assert_eq!(line.width_at(2), 0);
        assert_eq!(line.width_at(3), 1);
        assert_eq!(screen.cursor(), (0, 4));
    }

    #[test]
    fn wide_cell_wraps_before_final_column() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(4, 2);

        engine.feed(&mut screen, "abc界".as_bytes());

        assert_eq!(screen.grid().line(0).unwrap().text(), "abc");
        assert!(screen.grid().line(0).unwrap().is_wrapped());
        assert_eq!(screen.grid().line(1).unwrap().text(), "界");
        assert_eq!(screen.cursor(), (1, 2));
    }

    #[test]
    fn resize_reflows_wrapped_lines_without_scrolling_down() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(5, 4);

        engine.feed(&mut screen, b"abcdefghij");
        screen.resize(3, 4);

        assert_eq!(screen.grid().line(0).unwrap().text(), "abc");
        assert_eq!(screen.grid().line(1).unwrap().text(), "def");
        assert_eq!(screen.grid().line(2).unwrap().text(), "ghi");
        assert_eq!(screen.grid().line(3).unwrap().text(), "j");
    }

    #[test]
    fn resize_shorter_preserves_visible_top() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 4);

        engine.feed(&mut screen, b"one\r\ntwo\r\nthree\r\nfour");
        screen.resize(10, 2);

        assert_eq!(screen.grid().line(0).unwrap().text(), "one");
        assert_eq!(screen.grid().line(1).unwrap().text(), "two");
    }

    #[test]
    fn resize_grow_restores_rows_hidden_by_shrink() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 4);

        engine.feed(&mut screen, b"one\r\ntwo\r\nthree\r\nfour");
        screen.resize(10, 2);
        screen.resize(10, 4);

        assert_eq!(screen.grid().line(0).unwrap().text(), "one");
        assert_eq!(screen.grid().line(1).unwrap().text(), "two");
        assert_eq!(screen.grid().line(2).unwrap().text(), "three");
        assert_eq!(screen.grid().line(3).unwrap().text(), "four");
    }

    #[test]
    fn resize_renders_authoritative_screen_immediately() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 3);

        engine.feed(&mut screen, b"old top\r\nold middle\r\nold bottom");
        screen.resize(10, 3);
        engine.feed(&mut screen, b"\x1b[2J\x1b[Hrecent");

        assert_eq!(render_text(&screen, 0), "recent");
    }

    #[test]
    fn alternate_resize_preserves_absolute_rows_instead_of_reflowing() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(12, 3);

        engine.feed(&mut screen, b"\x1b[?1049h");
        engine.feed(&mut screen, b"\x1b[1;1Habcdefghijkl\x1b[2;1Hstatus-line");
        screen.resize(6, 3);

        assert_eq!(render_text(&screen, 0), "abcdef");
        assert_eq!(render_text(&screen, 1), "status");
        assert_eq!(screen.grid().line(0).unwrap().text(), "abcdef");
        assert_eq!(screen.grid().line(1).unwrap().text(), "status");
    }

    #[test]
    fn synchronized_output_mode_tracks_decset_2026() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 2);

        engine.feed(&mut screen, b"\x1b[?2026hpartial");
        assert!(screen.synchronized_output());
        assert_eq!(render_text(&screen, 0), "partial");

        let before = screen.generation();
        engine.feed(&mut screen, b"\x1b[?2026l");
        assert!(!screen.synchronized_output());
        assert_eq!(screen.generation(), before + 1);
        assert_eq!(screen.line_generation(0), Some(screen.generation()));
        assert_eq!(screen.line_generation(1), Some(screen.generation()));
    }

    #[test]
    fn alternate_screen_preserves_primary() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);
        engine.feed(&mut screen, b"primary");
        engine.feed(&mut screen, b"\x1b[?1049h\x1b[Halt");
        assert_eq!(screen.grid().line(0).unwrap().text(), "alt");
        engine.feed(&mut screen, b"\x1b[?1049l");
        assert_eq!(screen.grid().line(0).unwrap().text(), "primary");
    }

    #[test]
    fn mode_1049_restores_primary_cursor_state_and_repaints_primary_rows() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);
        engine.feed(&mut screen, b"primary\x1b[3;6H\x1b[31m");

        engine.feed(&mut screen, b"\x1b[?1049h");
        assert!(screen.alternate_active());
        assert_eq!(screen.cursor(), (2, 5));

        // A cursor save performed by the full-screen application belongs to
        // the alternate screen and must not replace the primary saved state.
        engine.feed(&mut screen, b"\x1b[4;9H\x1b7\x1b[32malt");
        engine.feed(&mut screen, b"\x1b[?1049l");

        assert!(!screen.alternate_active());
        assert_eq!(screen.cursor(), (2, 5));
        assert_eq!(screen.current_style().fg, Color::Indexed(1));
        assert_eq!(render_text(&screen, 0), "primary");
        for row in 0..screen.rows() {
            assert_eq!(screen.line_generation(row), Some(screen.generation()));
        }

        engine.feed(&mut screen, b"next");
        assert_eq!(render_text(&screen, 0), "primary");
        assert_eq!(render_text(&screen, 2), "     next");
    }

    #[test]
    fn mode_1048_saves_and_restores_cursor_state_without_switching_buffers() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);

        engine.feed(
            &mut screen,
            b"\x1b[2;4H\x1b[1m\x1b[?1048h\x1b[4;8H\x1b[22m\x1b[?1048l",
        );

        assert!(!screen.alternate_active());
        assert_eq!(screen.cursor(), (1, 3));
        assert!(screen.current_style().bold);
    }

    #[test]
    fn mode_1049_clamps_the_restored_cursor_after_an_alternate_screen_resize() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(10, 5);
        engine.feed(&mut screen, b"\x1b[5;10H\x1b[?1049h");

        screen.resize(4, 2);
        engine.feed(&mut screen, b"\x1b[?1049l");

        assert!(!screen.alternate_active());
        assert_eq!(screen.cursor(), (1, 3));
    }

    #[test]
    fn mode_1049_reflows_the_saved_primary_cursor_during_resize() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(5, 4);
        engine.feed(&mut screen, b"abcdefghij\x1b[?1049h");

        screen.resize(3, 4);
        engine.feed(&mut screen, b"\x1b[?1049l");

        assert_eq!(screen.cursor(), (3, 1));
        assert_eq!(render_text(&screen, 0), "abc");
        assert_eq!(render_text(&screen, 1), "def");
        assert_eq!(render_text(&screen, 2), "ghi");
        assert_eq!(render_text(&screen, 3), "j");

        engine.feed(&mut screen, b"k");
        assert_eq!(render_text(&screen, 3), "jk");
    }

    #[test]
    fn mode_47_preserves_alternate_contents_across_buffer_switches() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);

        engine.feed(&mut screen, b"\x1b[?47halt\x1b[?47l\x1b[?47h");

        assert!(screen.alternate_active());
        assert_eq!(render_text(&screen, 0), "alt");
    }

    #[test]
    fn mode_1047_clears_alternate_contents_when_leaving() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);

        engine.feed(&mut screen, b"\x1b[?47halt\x1b[?47l\x1b[?1047h");
        assert_eq!(render_text(&screen, 0), "alt");

        engine.feed(&mut screen, b"\x1b[?1047l\x1b[?47h");
        assert!(screen.alternate_active());
        assert_eq!(render_text(&screen, 0), "");
    }

    #[test]
    fn mode_1049_clears_previous_alternate_contents_when_entering() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 5);

        engine.feed(&mut screen, b"\x1b[?47hold\x1b[?47l\x1b[?1049h");

        assert!(screen.alternate_active());
        assert_eq!(render_text(&screen, 0), "");
    }

    #[test]
    fn dec_mouse_modes_are_authoritative_and_sgr_encoded() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(80, 24);
        engine.feed(&mut screen, b"\x1b[?1002h\x1b[?1006h");

        let drag = screen
            .encode_mouse(
                MouseEvent {
                    kind: MouseEventKind::Drag,
                    button: MouseButton::Left,
                    modifiers: MouseModifiers::new(MouseModifiers::SHIFT | MouseModifiers::CONTROL),
                    column: 10,
                    row: 4,
                },
                10,
                4,
            )
            .unwrap();
        assert_eq!(drag, b"\x1b[<52;11;5M");

        engine.feed(&mut screen, b"\x1b[?1002l");
        assert!(!screen.application_mouse_enabled());
        assert!(screen
            .encode_mouse(
                MouseEvent {
                    kind: MouseEventKind::Down,
                    button: MouseButton::Left,
                    modifiers: MouseModifiers::default(),
                    column: 0,
                    row: 0,
                },
                0,
                0,
            )
            .is_none());
    }

    #[test]
    fn x10_drops_release_and_normal_mode_uses_legacy_release() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(80, 24);
        let release = MouseEvent {
            kind: MouseEventKind::Up,
            button: MouseButton::Left,
            modifiers: MouseModifiers::default(),
            column: 2,
            row: 3,
        };

        engine.feed(&mut screen, b"\x1b[?9h");
        assert!(screen.encode_mouse(release, 2, 3).is_none());
        engine.feed(&mut screen, b"\x1b[?9l\x1b[?1000h");
        assert_eq!(screen.encode_mouse(release, 2, 3).unwrap(), b"\x1b[M##$");
    }

    #[test]
    fn private_win32_input_mode_tracks_set_and_reset() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(80, 24);

        engine.feed(&mut screen, b"\x1b[?9001h");
        assert!(screen.win32_input_mode());
        engine.feed(&mut screen, b"\x1b[?9001l");
        assert!(!screen.win32_input_mode());
    }

    #[test]
    fn replays_cmd_startup_without_prompt_gap() {
        const PROMPT: &str = "C:\\Users\\shres\\mux\\wmux>";
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(80, 24);

        engine.feed(
            &mut screen,
            b"\x1b[?9001h\x1b[?1004h\x1b[?25l\x1b[2J\x1b[m\x1b[H",
        );
        engine.feed(
            &mut screen,
            b"\x1b]0;C:\\WINDOWS\\system32\\cmd.exe\x07\x1b[?25h",
        );
        engine.feed(&mut screen, PROMPT.as_bytes());

        assert_eq!(screen.grid().line(0).unwrap().text(), PROMPT);
        assert_eq!(screen.grid().line(1).unwrap().text(), "");
        assert_eq!(screen.cursor(), (0, PROMPT.len() as u16));
    }

    #[test]
    fn replays_input_backspace_on_prompt_row_only() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(80, 24);

        engine.feed(&mut screen, b"C:\\work>dwdwd");
        engine.feed(&mut screen, b"\x08 \x08");

        assert_eq!(screen.grid().line(0).unwrap().text(), "C:\\work>dwdw");
        assert_eq!(screen.grid().line(1).unwrap().text(), "");
        assert_eq!(screen.cursor(), (0, 12));
    }

    fn render_text(screen: &Screen, row: u16) -> String {
        let mut text = String::new();
        for cell in screen
            .render_line_cells(row)
            .unwrap_or_default()
            .into_iter()
            .filter(|cell| !cell.is_continuation())
        {
            cell.text().push_to(&mut text);
        }
        while text.ends_with(' ') {
            text.pop();
        }
        text
    }
}
