use crate::{Line, PaneId};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SearchDirection {
    Forward,
    Backward,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CopyPosition {
    pub line: usize,
    pub column: u16,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum CopyModeResult {
    Continue,
    Cancel,
    Copy(Vec<u8>),
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CopyMode {
    pub pane: PaneId,
    cursor: CopyPosition,
    anchor: Option<CopyPosition>,
    rectangular: bool,
    search_input: Option<(SearchDirection, String)>,
    last_search: Option<(SearchDirection, String)>,
    viewport_offset: usize,
}

impl CopyMode {
    pub fn new(
        pane: PaneId,
        cursor_line: usize,
        cursor_column: u16,
        total_lines: usize,
        rows: u16,
    ) -> Self {
        let mut mode = Self {
            pane,
            cursor: CopyPosition {
                line: cursor_line.min(total_lines.saturating_sub(1)),
                column: cursor_column,
            },
            anchor: None,
            rectangular: false,
            search_input: None,
            last_search: None,
            viewport_offset: 0,
        };
        mode.ensure_cursor_visible(total_lines, rows);
        mode
    }

    pub const fn cursor(&self) -> CopyPosition {
        self.cursor
    }

    pub const fn anchor(&self) -> Option<CopyPosition> {
        self.anchor
    }

    pub const fn rectangular(&self) -> bool {
        self.rectangular
    }

    pub const fn viewport_offset(&self) -> usize {
        self.viewport_offset
    }

    pub fn anchor_output(&mut self, added: usize) {
        self.viewport_offset = self.viewport_offset.saturating_add(added);
    }

    pub fn prompt(&self) -> Option<String> {
        self.search_input.as_ref().map(|(direction, query)| {
            let marker = match direction {
                SearchDirection::Forward => '/',
                SearchDirection::Backward => '?',
            };
            format!("{marker}{query}")
        })
    }

    pub fn visible_cursor_row(&self, total_lines: usize, rows: u16) -> u16 {
        self.cursor
            .line
            .saturating_sub(viewport_top(total_lines, rows, self.viewport_offset))
            .min(usize::from(rows.saturating_sub(1))) as u16
    }

    pub fn handle_key(&mut self, bytes: &[u8], lines: &[Line], rows: u16) -> CopyModeResult {
        if self.search_input.is_some() {
            return self.handle_search_input(bytes, lines, rows);
        }
        let total = lines.len();
        let page = usize::from(rows.max(1));
        match bytes {
            b"q" | b"\x1b" | b"\x03" => return CopyModeResult::Cancel,
            b"\x1b[A" | b"k" | b"\x10" => self.move_vertical(-1, total, rows),
            b"\x1b[B" | b"j" | b"\x0e" => self.move_vertical(1, total, rows),
            b"\x1b[D" | b"h" | b"\x08" | b"\x7f" => self.move_left(lines),
            b"\x1b[C" | b"l" | b"\x06" => self.move_right(lines),
            b"\x1b[5~" | b"\x02" => self.move_vertical(-(page as isize), total, rows),
            b"\x1b[6~" | b"\x16" => self.move_vertical(page as isize, total, rows),
            b"\x15" => self.move_vertical(-((page / 2).max(1) as isize), total, rows),
            b"\x04" => self.move_vertical((page / 2).max(1) as isize, total, rows),
            b"g" | b"\x1b[1~" | b"\x1b[H" => {
                self.cursor.line = 0;
                self.cursor.column = 0;
                self.ensure_cursor_visible(total, rows);
            }
            b"G" | b"\x1b[4~" | b"\x1b[F" => {
                self.cursor.line = total.saturating_sub(1);
                self.cursor.column = line_end(lines.get(self.cursor.line));
                self.ensure_cursor_visible(total, rows);
            }
            b"0" | b"^" | b"\x01" => self.cursor.column = 0,
            b"$" | b"\x05" => self.cursor.column = line_end(lines.get(self.cursor.line)),
            b"b" => self.previous_word(lines),
            b"w" => self.next_word(lines),
            b" " | b"\x00" => self.anchor = Some(self.cursor),
            b"v" | b"\x16r" => self.rectangular = !self.rectangular,
            b"/" => self.search_input = Some((SearchDirection::Forward, String::new())),
            b"?" => self.search_input = Some((SearchDirection::Backward, String::new())),
            b"n" => self.repeat_search(lines, rows, false),
            b"N" => self.repeat_search(lines, rows, true),
            b"\r" | b"\x0a" | b"\x17" | b"\x1bw" => {
                let copied = self.selected_text(lines);
                return if copied.is_empty() {
                    CopyModeResult::Cancel
                } else {
                    CopyModeResult::Copy(copied.into_bytes())
                };
            }
            _ => {}
        }
        CopyModeResult::Continue
    }

    pub fn scroll(&mut self, delta: isize, total_lines: usize, rows: u16) {
        self.move_vertical(delta, total_lines, rows);
    }

    pub fn place_cursor(
        &mut self,
        viewport_row: u16,
        column: u16,
        total_lines: usize,
        rows: u16,
        begin_selection: bool,
    ) {
        let top = viewport_top(total_lines, rows, self.viewport_offset);
        self.cursor = CopyPosition {
            line: top
                .saturating_add(usize::from(viewport_row))
                .min(total_lines.saturating_sub(1)),
            column,
        };
        if begin_selection && self.anchor.is_none() {
            self.anchor = Some(self.cursor);
        }
    }

    pub fn selected_text(&self, lines: &[Line]) -> String {
        let Some(anchor) = self.anchor else {
            return String::new();
        };
        let (start, end) = ordered(anchor, self.cursor);
        let mut out = String::new();
        for (line_index, line) in lines
            .iter()
            .enumerate()
            .take(
                end.line
                    .min(lines.len().saturating_sub(1))
                    .saturating_add(1),
            )
            .skip(start.line)
        {
            let from = if self.rectangular {
                start.column.min(end.column)
            } else if line_index == start.line {
                start.column
            } else {
                0
            };
            let to = if self.rectangular {
                start.column.max(end.column).saturating_add(1)
            } else if line_index == end.line {
                end.column.saturating_add(1)
            } else {
                line.cols()
            };
            out.push_str(&line_text_range(line, from, to));
            if line_index != end.line && !line.is_wrapped() {
                out.push('\n');
            }
        }
        out
    }

    pub fn selection_ranges(&self) -> Vec<(usize, u16, u16)> {
        let Some(anchor) = self.anchor else {
            return Vec::new();
        };
        let (start, end) = ordered(anchor, self.cursor);
        (start.line..=end.line)
            .map(|line| {
                let from = if self.rectangular {
                    start.column.min(end.column)
                } else if line == start.line {
                    start.column
                } else {
                    0
                };
                let to = if self.rectangular {
                    start.column.max(end.column).saturating_add(1)
                } else if line == end.line {
                    end.column.saturating_add(1)
                } else {
                    u16::MAX
                };
                (line, from, to)
            })
            .collect()
    }

    fn handle_search_input(&mut self, bytes: &[u8], lines: &[Line], rows: u16) -> CopyModeResult {
        match bytes {
            b"\x1b" | b"\x03" => self.search_input = None,
            b"\x7f" | b"\x08" => {
                if let Some((_, query)) = self.search_input.as_mut() {
                    query.pop();
                }
            }
            b"\r" | b"\x0a" => {
                if let Some(search) = self.search_input.take() {
                    if !search.1.is_empty() {
                        self.last_search = Some(search.clone());
                        self.search(lines, rows, search.0, &search.1);
                    }
                }
            }
            _ => {
                if let Ok(text) = std::str::from_utf8(bytes) {
                    if !text.chars().any(char::is_control) {
                        if let Some((_, query)) = self.search_input.as_mut() {
                            query.push_str(text);
                        }
                    }
                }
            }
        }
        CopyModeResult::Continue
    }

    fn repeat_search(&mut self, lines: &[Line], rows: u16, reverse: bool) {
        if let Some((mut direction, query)) = self.last_search.clone() {
            if reverse {
                direction = match direction {
                    SearchDirection::Forward => SearchDirection::Backward,
                    SearchDirection::Backward => SearchDirection::Forward,
                };
            }
            self.search(lines, rows, direction, &query);
        }
    }

    fn search(&mut self, lines: &[Line], rows: u16, direction: SearchDirection, query: &str) {
        let found = match direction {
            SearchDirection::Forward => ((self.cursor.line + 1)..lines.len()).find_map(|line| {
                find_query_column(&lines[line], query, false).map(|column| (line, column))
            }),
            SearchDirection::Backward => (0..self.cursor.line).rev().find_map(|line| {
                find_query_column(&lines[line], query, true).map(|column| (line, column))
            }),
        };
        if let Some((line, column)) = found {
            self.cursor.line = line;
            self.cursor.column = column;
            self.ensure_cursor_visible(lines.len(), rows);
        }
    }

    fn move_vertical(&mut self, delta: isize, total: usize, rows: u16) {
        self.cursor.line = self
            .cursor
            .line
            .saturating_add_signed(delta)
            .min(total.saturating_sub(1));
        self.ensure_cursor_visible(total, rows);
    }

    fn move_left(&mut self, lines: &[Line]) {
        if self.cursor.column > 0 {
            self.cursor.column -= 1;
        } else if self.cursor.line > 0 {
            self.cursor.line -= 1;
            self.cursor.column = line_end(lines.get(self.cursor.line));
        }
    }

    fn move_right(&mut self, lines: &[Line]) {
        let end = line_end(lines.get(self.cursor.line));
        if self.cursor.column < end {
            self.cursor.column += 1;
        } else if self.cursor.line + 1 < lines.len() {
            self.cursor.line += 1;
            self.cursor.column = 0;
        }
    }

    fn previous_word(&mut self, lines: &[Line]) {
        let Some(line) = lines.get(self.cursor.line) else {
            return;
        };
        let end = usize::from(line_end(Some(line)));
        let mut column = usize::from(self.cursor.column).min(end);
        column = column.saturating_sub(1);
        while column > 0 && column_is_ascii_whitespace(line, column) {
            column -= 1;
        }
        while column > 0 && !column_is_ascii_whitespace(line, column - 1) {
            column -= 1;
        }
        self.cursor.column = normalize_cell_start(line, column);
    }

    fn next_word(&mut self, lines: &[Line]) {
        let Some(line) = lines.get(self.cursor.line) else {
            return;
        };
        let end = usize::from(line_end(Some(line)));
        let mut column = usize::from(self.cursor.column).min(end);
        while column < end && !column_is_ascii_whitespace(line, column) {
            column += 1;
        }
        while column < end && column_is_ascii_whitespace(line, column) {
            column += 1;
        }
        self.cursor.column = normalize_cell_start(line, column);
    }

    fn ensure_cursor_visible(&mut self, total: usize, rows: u16) {
        let rows = usize::from(rows.max(1));
        let mut top = total.saturating_sub(rows.saturating_add(self.viewport_offset));
        if self.cursor.line < top {
            self.viewport_offset = total.saturating_sub(rows).saturating_sub(self.cursor.line);
            top = self.cursor.line;
        }
        if self.cursor.line >= top.saturating_add(rows) {
            self.viewport_offset = total.saturating_sub(self.cursor.line.saturating_add(1));
        }
        self.viewport_offset = self.viewport_offset.min(total.saturating_sub(rows));
    }
}

fn viewport_top(total: usize, rows: u16, offset: usize) -> usize {
    total.saturating_sub(usize::from(rows.max(1)).saturating_add(offset))
}

fn ordered(a: CopyPosition, b: CopyPosition) -> (CopyPosition, CopyPosition) {
    if (a.line, a.column) <= (b.line, b.column) {
        (a, b)
    } else {
        (b, a)
    }
}

fn line_end(line: Option<&Line>) -> u16 {
    line.map_or(0, |line| {
        line.stored_len()
            .min(usize::from(line.cols()))
            .min(usize::from(u16::MAX)) as u16
    })
}

fn find_query_column(line: &Line, query: &str, reverse: bool) -> Option<u16> {
    let text = line.text();
    let byte_offset = if reverse {
        text.rfind(query)?
    } else {
        text.find(query)?
    };
    Some(byte_offset_to_grid_column(line, byte_offset))
}

fn byte_offset_to_grid_column(line: &Line, byte_offset: usize) -> u16 {
    let mut consumed = 0_usize;
    for column in 0..line.cols() {
        let Some(cell) = line.cell(column) else {
            break;
        };
        if cell.is_continuation() {
            continue;
        }
        let end = consumed.saturating_add(cell.text().byte_len());
        if byte_offset < end {
            return column;
        }
        consumed = end;
    }
    line_end(Some(line))
}

fn column_is_ascii_whitespace(line: &Line, column: usize) -> bool {
    u16::try_from(column)
        .ok()
        .and_then(|column| line.cell(column))
        .is_some_and(|cell| {
            !cell.is_continuation() && cell.text().first_char().is_ascii_whitespace()
        })
}

fn normalize_cell_start(line: &Line, column: usize) -> u16 {
    let column = column.min(usize::from(u16::MAX)) as u16;
    if line.cell(column).is_some_and(|cell| cell.is_continuation()) {
        line.previous_cell_start(column.saturating_add(1))
    } else {
        column
    }
}

fn line_text_range(line: &Line, from: u16, to: u16) -> String {
    let mut out = String::new();
    for column in from..to.min(line.cols()) {
        let Some(cell) = line.cell(column) else {
            out.push(' ');
            continue;
        };
        if !cell.is_continuation() {
            cell.text().push_to(&mut out);
        }
    }
    while out.ends_with(' ') {
        out.pop();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{CellText, PaneId, Style};

    fn line(text: &str) -> Line {
        let mut line = Line::blank(32);
        for (column, ch) in text.chars().enumerate() {
            line.set(column as u16, ch, 1, Style::default());
        }
        line
    }

    #[test]
    fn navigation_keeps_cursor_visible() {
        let lines = (0..20)
            .map(|n| line(&format!("line {n}")))
            .collect::<Vec<_>>();
        let mut mode = CopyMode::new(PaneId::new(1), 19, 0, lines.len(), 5);
        mode.handle_key(b"g", &lines, 5);
        assert_eq!(mode.cursor().line, 0);
        assert_eq!(mode.viewport_offset(), 15);
        mode.handle_key(b"G", &lines, 5);
        assert_eq!(mode.cursor().line, 19);
        assert_eq!(mode.viewport_offset(), 0);
    }

    #[test]
    fn selection_copies_across_lines() {
        let lines = vec![line("alpha"), line("beta")];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 1, lines.len(), 2);
        mode.handle_key(b" ", &lines, 2);
        mode.handle_key(b"\x1b[B", &lines, 2);
        mode.handle_key(b"\x1b[C", &lines, 2);
        assert_eq!(mode.selected_text(&lines), "lpha\nbet");
    }

    #[test]
    fn selection_copies_combined_text_without_splitting_grid_columns() {
        let mut line = Line::blank(8);
        let mut text = CellText::from('e');
        assert!(text.try_append('\u{301}'));
        line.set_text(0, text, 1, Style::default());
        line.set(1, 'x', 1, Style::default());
        let lines = vec![line];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 0, 1, 1);

        mode.handle_key(b" ", &lines, 1);
        mode.handle_key(b"l", &lines, 1);

        assert_eq!(mode.selected_text(&lines), "e\u{301}x");
    }

    #[test]
    fn search_moves_to_matching_line() {
        let lines = vec![line("zero"), line("target here"), line("last")];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 0, lines.len(), 2);
        mode.handle_key(b"/", &lines, 2);
        mode.handle_key(b"target", &lines, 2);
        mode.handle_key(b"\r", &lines, 2);
        assert_eq!(mode.cursor(), CopyPosition { line: 1, column: 0 });
    }

    #[test]
    fn search_maps_utf8_offsets_to_grid_columns() {
        let mut combined = Line::blank(32);
        let mut text = CellText::from('e');
        assert!(text.try_append('\u{301}'));
        combined.set_text(0, text, 1, Style::default());
        combined.set(1, ' ', 1, Style::default());
        for (offset, ch) in "target".chars().enumerate() {
            combined.set(offset as u16 + 2, ch, 1, Style::default());
        }
        let lines = vec![line("zero"), combined];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 0, lines.len(), 2);

        mode.handle_key(b"/", &lines, 2);
        mode.handle_key(b"target", &lines, 2);
        mode.handle_key(b"\r", &lines, 2);

        assert_eq!(mode.cursor(), CopyPosition { line: 1, column: 2 });
    }

    #[test]
    fn word_navigation_uses_grid_columns_for_multibyte_cells() {
        let lines = vec![line("\u{3bb} target")];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 0, lines.len(), 1);

        mode.handle_key(b"w", &lines, 1);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 2 });

        mode.handle_key(b"b", &lines, 1);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 0 });
    }

    #[test]
    fn word_navigation_crosses_wide_cell_continuations() {
        let mut wide = Line::blank(16);
        wide.set(0, '\u{754c}', 2, Style::default());
        wide.set(2, ' ', 1, Style::default());
        for (offset, ch) in "target".chars().enumerate() {
            wide.set(offset as u16 + 3, ch, 1, Style::default());
        }
        let lines = vec![wide];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 0, lines.len(), 1);

        mode.handle_key(b"w", &lines, 1);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 3 });

        mode.handle_key(b"b", &lines, 1);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 0 });
    }

    #[test]
    fn emacs_right_can_reach_one_past_end_before_wrapping() {
        let lines = vec![line("abc"), line("next")];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 1, lines.len(), 2);

        mode.handle_key(b"l", &lines, 2);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 2 });
        mode.handle_key(b"l", &lines, 2);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 3 });
        mode.handle_key(b"l", &lines, 2);
        assert_eq!(mode.cursor(), CopyPosition { line: 1, column: 0 });
    }

    #[test]
    fn line_end_uses_grid_columns_for_wide_characters() {
        let mut wide = Line::blank(8);
        wide.set(0, '界', 2, Style::default());
        wide.set(2, 'x', 1, Style::default());
        let lines = vec![wide];
        let mut mode = CopyMode::new(PaneId::new(1), 0, 0, lines.len(), 1);

        mode.handle_key(b"$", &lines, 1);
        assert_eq!(mode.cursor(), CopyPosition { line: 0, column: 3 });
    }
}
