use std::{collections::VecDeque, sync::Arc};

use crate::text::{extends_grapheme, CellText};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum GraphemeAppend {
    NotExtension,
    IgnoredOverLimit,
    Appended { old_width: u8, new_width: u8 },
}

#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
pub enum Color {
    #[default]
    Default,
    Indexed(u8),
    Rgb(u8, u8, u8),
}

#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
pub struct Style {
    pub fg: Color,
    pub bg: Color,
    pub bold: bool,
    pub dim: bool,
    pub italic: bool,
    pub underline: bool,
    pub reverse: bool,
    pub hidden: bool,
    pub strikethrough: bool,
}

// Style's complete value domain fits in 57 bits. This is a canonical, table-free
// intern ID: equal styles always have the same ID, default is zero, and resolving
// an ID needs no allocation, lock, hash lookup, or process-global lifetime.
#[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
struct StyleId(u64);

impl StyleId {
    const COLOR_BITS: u32 = 25;
    const COLOR_MASK: u64 = (1 << Self::COLOR_BITS) - 1;
    const STYLE_MASK: u64 = (1 << 57) - 1;

    fn intern(style: Style) -> Self {
        let mut bits = encode_color(style.fg);
        bits |= encode_color(style.bg) << Self::COLOR_BITS;
        bits |= u64::from(style.bold) << 50;
        bits |= u64::from(style.dim) << 51;
        bits |= u64::from(style.italic) << 52;
        bits |= u64::from(style.underline) << 53;
        bits |= u64::from(style.reverse) << 54;
        bits |= u64::from(style.hidden) << 55;
        bits |= u64::from(style.strikethrough) << 56;
        Self(bits)
    }

    fn get(self) -> Style {
        Style {
            fg: decode_color(self.0 & Self::COLOR_MASK),
            bg: decode_color((self.0 >> Self::COLOR_BITS) & Self::COLOR_MASK),
            bold: self.0 & (1 << 50) != 0,
            dim: self.0 & (1 << 51) != 0,
            italic: self.0 & (1 << 52) != 0,
            underline: self.0 & (1 << 53) != 0,
            reverse: self.0 & (1 << 54) != 0,
            hidden: self.0 & (1 << 55) != 0,
            strikethrough: self.0 & (1 << 56) != 0,
        }
    }
}

/// Cell-local display metadata packed into the seven unused high bits of the
/// canonical 57-bit style word. Terminal cell widths are limited to 0, 1, or
/// 2, leaving room for the continuation marker without growing `Cell`.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct CellMetadata(u64);

impl CellMetadata {
    const WIDTH_SHIFT: u32 = 57;
    const WIDTH_MASK: u64 = 0b11 << Self::WIDTH_SHIFT;
    const CONTINUATION_MASK: u64 = 1 << 59;

    const fn new(style: StyleId, width: u8, continuation: bool) -> Self {
        Self(
            (style.0 & StyleId::STYLE_MASK)
                | (((width as u64) & 0b11) << Self::WIDTH_SHIFT)
                | ((continuation as u64) << 59),
        )
    }

    const fn style(self) -> StyleId {
        StyleId(self.0 & StyleId::STYLE_MASK)
    }

    const fn width(self) -> u8 {
        ((self.0 & Self::WIDTH_MASK) >> Self::WIDTH_SHIFT) as u8
    }

    const fn is_continuation(self) -> bool {
        self.0 & Self::CONTINUATION_MASK != 0
    }

    fn set_style(&mut self, style: StyleId) {
        self.0 = (self.0 & !StyleId::STYLE_MASK) | (style.0 & StyleId::STYLE_MASK);
    }
}

fn encode_color(color: Color) -> u64 {
    match color {
        Color::Default => 0,
        Color::Indexed(index) => u64::from(index) + 1,
        Color::Rgb(red, green, blue) => {
            257 + (u64::from(red) << 16) + (u64::from(green) << 8) + u64::from(blue)
        }
    }
}

fn decode_color(encoded: u64) -> Color {
    match encoded {
        0 => Color::Default,
        1..=256 => Color::Indexed((encoded - 1) as u8),
        value => {
            let rgb = value - 257;
            Color::Rgb((rgb >> 16) as u8, (rgb >> 8) as u8, rgb as u8)
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Cell {
    text: CellText,
    metadata: CellMetadata,
}

static DEFAULT_CELL: Cell = Cell {
    text: CellText::from_char_const(' '),
    metadata: CellMetadata::new(StyleId(0), 1, false),
};

impl Cell {
    pub(crate) const fn const_blank_for_render() -> Self {
        Self {
            text: CellText::from_char_const(' '),
            metadata: CellMetadata::new(StyleId(0), 1, false),
        }
    }

    pub fn blank() -> Self {
        DEFAULT_CELL.clone()
    }

    pub fn blank_with_style(style: Style) -> Self {
        Self::blank_with_style_ref(StyleId::intern(style))
    }

    fn blank_with_style_ref(style: StyleId) -> Self {
        Self {
            text: CellText::from_char_const(' '),
            metadata: CellMetadata::new(style, 1, false),
        }
    }

    pub fn ch(&self) -> char {
        self.text.first_char()
    }

    pub const fn text(&self) -> &CellText {
        &self.text
    }

    pub const fn width(&self) -> u8 {
        self.metadata.width()
    }

    pub const fn is_continuation(&self) -> bool {
        self.metadata.is_continuation()
    }

    pub fn style(&self) -> Style {
        self.metadata.style().get()
    }

    pub fn printable(ch: char, width: u8, style: Style) -> Self {
        Self::printable_with_style_ref(ch, width, StyleId::intern(style))
    }

    pub fn printable_text(text: CellText, width: u8, style: Style) -> Self {
        Self::printable_text_with_style_ref(text, width, StyleId::intern(style))
    }

    fn printable_with_style_ref(ch: char, width: u8, style: StyleId) -> Self {
        Self::printable_text_with_style_ref(CellText::from(ch), width, style)
    }

    fn printable_text_with_style_ref(text: CellText, width: u8, style: StyleId) -> Self {
        Self {
            text,
            metadata: CellMetadata::new(style, width, false),
        }
    }

    pub fn continuation(style: Style) -> Self {
        Self::continuation_with_style_ref(StyleId::intern(style))
    }

    fn continuation_with_style_ref(style: StyleId) -> Self {
        Self {
            text: CellText::from_char_const(' '),
            metadata: CellMetadata::new(style, 0, true),
        }
    }

    pub fn is_blank_default(&self) -> bool {
        self.text.is_single_char(' ')
            && self.width() == 1
            && !self.is_continuation()
            && self.metadata.style() == StyleId::default()
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Line {
    cells: Arc<Vec<Cell>>,
    cols: u16,
    wrapped: bool,
    generation: u64,
}

impl Line {
    pub fn blank(cols: u16) -> Self {
        Self {
            cells: Arc::new(Vec::new()),
            cols: cols.max(1),
            wrapped: false,
            generation: 0,
        }
    }

    pub fn text(&self) -> String {
        let mut text = String::new();
        for cell in self.cells.iter().filter(|cell| !cell.is_continuation()) {
            cell.text().push_to(&mut text);
        }
        while text.ends_with(' ') {
            text.pop();
        }
        text
    }

    pub fn cells(&self) -> &[Cell] {
        &self.cells
    }

    pub const fn cols(&self) -> u16 {
        self.cols
    }

    pub fn stored_len(&self) -> usize {
        self.cells.len()
    }

    pub fn shares_cells_with(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.cells, &other.cells)
    }

    pub fn set(&mut self, col: u16, ch: char, width: u8, style: Style) {
        self.set_text(col, CellText::from(ch), width, style);
    }

    pub fn set_text(&mut self, col: u16, text: CellText, width: u8, style: Style) {
        let col = col as usize;
        if col >= self.cols as usize {
            return;
        }
        let default_space = width != 2 && text.is_single_char(' ') && style == Style::default();
        if default_space && col >= self.cells.len() {
            return;
        }
        let may_shorten = default_space && col + 1 == self.cells.len();
        let style = StyleId::intern(style);
        self.materialize_to(col.saturating_add(usize::from(width.max(1))));
        self.clear_cell_boundary(col);
        if width == 2 {
            if col + 1 < self.cols as usize {
                self.clear_cell_boundary(col + 1);
            }
            let cells = Arc::make_mut(&mut self.cells);
            cells[col] = Cell::printable_text_with_style_ref(text, 2, style);
            if col + 1 < cells.len() {
                cells[col + 1] = Cell::continuation_with_style_ref(style);
            }
        } else {
            Arc::make_mut(&mut self.cells)[col] =
                Cell::printable_text_with_style_ref(text, 1, style);
        }
        if may_shorten {
            self.trim_default_tail();
        }
    }

    pub(crate) fn append_grapheme(&mut self, col: u16, ch: char) -> GraphemeAppend {
        let col = usize::from(col);
        if col >= usize::from(self.cols) {
            return GraphemeAppend::NotExtension;
        }

        let Some(current) = self.cell(col as u16).cloned() else {
            return GraphemeAppend::NotExtension;
        };
        if current.is_continuation() || !extends_grapheme(current.text(), ch) {
            return GraphemeAppend::NotExtension;
        }

        let mut text = current.text().clone();
        if !text.try_append(ch) {
            return GraphemeAppend::IgnoredOverLimit;
        }
        let old_width = current.width().max(1);
        let new_width = text.display_width().max(1);

        self.materialize_to(col + usize::from(old_width.max(new_width)));
        self.clear_cell_boundary(col);
        if new_width == 2 && col + 1 < usize::from(self.cols) {
            self.clear_cell_boundary(col + 1);
        }
        let cells = Arc::make_mut(&mut self.cells);
        let style = current.metadata.style();
        cells[col] = Cell::printable_text_with_style_ref(text, new_width, style);
        if new_width == 2 && col + 1 < cells.len() {
            cells[col + 1] = Cell::continuation_with_style_ref(style);
        } else if old_width == 2 && col + 1 < cells.len() {
            cells[col + 1] = Cell::blank();
        }
        self.trim_default_tail();
        GraphemeAppend::Appended {
            old_width,
            new_width,
        }
    }

    pub fn cell(&self, col: u16) -> Option<&Cell> {
        if col >= self.cols {
            None
        } else {
            Some(self.cells.get(col as usize).unwrap_or(&DEFAULT_CELL))
        }
    }

    pub fn reverse_range(&mut self, start: u16, end: u16) {
        let end = end.min(self.cols);
        if start >= end {
            return;
        }
        self.materialize_to(usize::from(end));
        let cells = Arc::make_mut(&mut self.cells);
        for cell in &mut cells[usize::from(start)..usize::from(end)] {
            let mut style = cell.style();
            style.reverse = !style.reverse;
            cell.metadata.set_style(StyleId::intern(style));
        }
    }

    pub(crate) fn replace_cell(&mut self, col: u16, cell: Cell) {
        if col >= self.cols {
            return;
        }
        if cell.is_blank_default() && col as usize >= self.cells.len() {
            return;
        }
        self.materialize_to(col as usize + 1);
        let may_shorten = cell.is_blank_default() && col as usize + 1 == self.cells.len();
        Arc::make_mut(&mut self.cells)[col as usize] = cell;
        if may_shorten {
            self.trim_default_tail();
        }
    }

    pub fn width_at(&self, col: u16) -> u8 {
        self.cell(col).map(Cell::width).unwrap_or(1)
    }

    pub fn previous_cell_start(&self, col: u16) -> u16 {
        if col == 0 {
            return 0;
        }
        let previous = col - 1;
        if self
            .cell(previous)
            .is_some_and(|cell| cell.is_continuation())
        {
            previous.saturating_sub(1)
        } else {
            previous
        }
    }

    pub const fn is_wrapped(&self) -> bool {
        self.wrapped
    }

    pub const fn generation(&self) -> u64 {
        self.generation
    }

    pub(crate) fn set_generation(&mut self, generation: u64) {
        self.generation = generation;
    }

    pub(crate) fn set_ascii_run(&mut self, col: u16, bytes: &[u8], style: Style) -> usize {
        let start = usize::from(col).min(self.cols as usize);
        let count = bytes.len().min(self.cols as usize - start);
        if count == 0 {
            return 0;
        }
        let style = StyleId::intern(style);
        self.materialize_to(start + count);
        self.clear_cell_boundary(start);
        self.clear_cell_boundary(start + count - 1);
        for (cell, byte) in Arc::make_mut(&mut self.cells)[start..start + count]
            .iter_mut()
            .zip(&bytes[..count])
        {
            *cell = Cell::printable_with_style_ref(char::from(*byte), 1, style);
        }
        if start + count >= self.cells.len()
            && bytes[..count].iter().all(|byte| *byte == b' ')
            && style.get() == Style::default()
        {
            self.trim_default_tail();
        }
        count
    }

    pub fn set_wrapped(&mut self, wrapped: bool) {
        self.wrapped = wrapped;
    }

    pub fn clear_all(&mut self) {
        self.clear_all_with_style(Style::default());
    }

    pub fn clear_all_with_style(&mut self, style: Style) {
        if style == Style::default() {
            self.cells = Arc::new(Vec::new());
        } else {
            let style = StyleId::intern(style);
            self.cells = Arc::new(vec![Cell::blank_with_style_ref(style); self.cols as usize]);
        }
        self.wrapped = false;
    }

    pub fn clear_range(&mut self, start: u16, end: u16) {
        self.clear_range_with_style(start, end, Style::default());
    }

    pub fn clear_range_with_style(&mut self, start: u16, end: u16, style: Style) {
        let start = start.min(self.cols) as usize;
        let end = end.min(self.cols) as usize;
        if style == Style::default() && start >= self.cells.len() {
            return;
        }
        let end = if style == Style::default() {
            end.min(self.cells.len())
        } else {
            end
        };
        if start < end {
            self.materialize_to(end);
            self.clear_cell_boundary(start);
            self.clear_cell_boundary(end.saturating_sub(1));
        }
        let style = StyleId::intern(style);
        for cell in &mut Arc::make_mut(&mut self.cells)[start..end] {
            *cell = Cell::blank_with_style_ref(style);
        }
        self.trim_default_tail();
    }

    pub fn insert_blank(&mut self, col: u16, count: u16) {
        self.insert_blank_with_style(col, count, Style::default());
    }

    pub fn insert_blank_with_style(&mut self, col: u16, count: u16, style: Style) {
        self.materialize_full();
        let start = (col as usize).min(self.cols as usize);
        let count = count.max(1) as usize;
        let style = StyleId::intern(style);
        let cells = Arc::make_mut(&mut self.cells);
        for index in (start..cells.len()).rev() {
            cells[index] = if index >= start + count {
                cells[index - count].clone()
            } else {
                Cell::blank_with_style_ref(style)
            };
        }
        self.sanitize_wide_cells();
        self.trim_default_tail();
    }

    pub fn delete(&mut self, col: u16, count: u16) {
        self.delete_with_style(col, count, Style::default());
    }

    pub fn delete_with_style(&mut self, col: u16, count: u16, style: Style) {
        self.materialize_full();
        let start = (col as usize).min(self.cols as usize);
        let count = count.max(1) as usize;
        let style = StyleId::intern(style);
        let cells = Arc::make_mut(&mut self.cells);
        for index in start..cells.len() {
            let source = index + count;
            cells[index] = if source < cells.len() {
                cells[source].clone()
            } else {
                Cell::blank_with_style_ref(style)
            };
        }
        self.sanitize_wide_cells();
        self.trim_default_tail();
    }

    pub fn resize(&mut self, cols: u16) {
        self.cols = cols.max(1);
        Arc::make_mut(&mut self.cells).truncate(self.cols as usize);
        if self.cells.last().is_some_and(|cell| cell.width() == 2)
            && self.cells.len() < self.cols as usize
        {
            Arc::make_mut(&mut self.cells).push(Cell::blank());
        }
        self.sanitize_wide_cells();
        self.trim_default_tail();
    }

    pub(crate) fn content_cells(&self) -> Vec<Cell> {
        let end = if self.wrapped {
            self.cols as usize
        } else {
            self.cells
                .iter()
                .rposition(|cell| !cell.is_blank_default())
                .map(|index| index + 1)
                .unwrap_or(0)
        };
        (0..end)
            .filter_map(|index| self.cell(index as u16))
            .filter(|cell| !cell.is_continuation())
            .cloned()
            .collect()
    }

    fn clear_cell_boundary(&mut self, col: usize) {
        if col >= self.cells.len() {
            return;
        }
        if self.cells[col].is_continuation() {
            let cells = Arc::make_mut(&mut self.cells);
            if col > 0 {
                cells[col - 1] = Cell::blank();
            }
            cells[col] = Cell::blank();
        } else if self.cells[col].width() == 2 && col + 1 < self.cells.len() {
            Arc::make_mut(&mut self.cells)[col + 1] = Cell::blank();
        }
    }

    fn sanitize_wide_cells(&mut self) {
        let cells = Arc::make_mut(&mut self.cells);
        for index in 0..cells.len() {
            if cells[index].is_continuation() {
                let valid = index > 0 && cells[index - 1].width() == 2;
                if !valid {
                    cells[index] = Cell::blank();
                }
            } else if cells[index].width() == 2 {
                if index + 1 >= cells.len() || index + 1 >= self.cols as usize {
                    continue;
                } else {
                    let style = cells[index].metadata.style();
                    cells[index + 1] = Cell::continuation_with_style_ref(style);
                }
            }
        }
    }

    fn materialize_to(&mut self, end: usize) {
        let end = end.min(self.cols as usize);
        if self.cells.len() < end {
            Arc::make_mut(&mut self.cells).resize(end, Cell::blank());
        }
    }

    fn materialize_full(&mut self) {
        self.materialize_to(self.cols as usize);
    }

    fn trim_default_tail(&mut self) {
        let len = self
            .cells
            .iter()
            .rposition(|cell| !cell.is_blank_default())
            .map_or(0, |index| index + 1);
        if len == 0 {
            if !self.cells.is_empty() {
                self.cells = Arc::new(Vec::new());
            }
        } else {
            Arc::make_mut(&mut self.cells).truncate(len);
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct LogicalLine {
    cells: Arc<Vec<Cell>>,
    complete: bool,
    captured_rows: usize,
    generation: u64,
}

impl LogicalLine {
    fn from_physical(line: Line) -> Self {
        let complete = !line.is_wrapped();
        let generation = line.generation();
        let cells = take_content_cells(line);
        Self {
            cells,
            complete,
            captured_rows: 1,
            generation,
        }
    }

    fn append_physical(&mut self, line: Line) {
        let complete = !line.is_wrapped();
        let generation = line.generation();
        let cells = take_content_cells(line);
        Arc::make_mut(&mut self.cells).extend(cells.iter().cloned());
        self.complete = complete;
        self.captured_rows += 1;
        self.generation = self.generation.max(generation);
    }

    fn reflow(&self, cols: u16) -> Vec<Line> {
        let mut lines = Vec::new();
        reflow_logical_line(&self.cells, cols, self.generation, &mut lines);
        if !self.complete {
            if let Some(last) = lines.last_mut() {
                last.set_wrapped(true);
            }
        }
        lines
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct HistoryCacheEntry {
    cols: u16,
    revision: u64,
    lines: Arc<Vec<Line>>,
}

#[derive(Clone, Debug)]
pub struct Grid {
    cols: u16,
    rows: u16,
    visible: Vec<Line>,
    history: VecDeque<LogicalLine>,
    history_rows: usize,
    below: Vec<Line>,
    history_limit: usize,
    history_revision: u64,
    history_added: u64,
    history_cache: Vec<HistoryCacheEntry>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub(crate) struct CursorLineEdgeAnchor {
    pub(crate) left: Vec<Cell>,
    pub(crate) right: Vec<Cell>,
}

impl CursorLineEdgeAnchor {
    fn is_spacer(cell: &Cell) -> bool {
        cell.text().is_single_char(' ') && cell.width() == 1 && !cell.is_continuation()
    }

    fn split(line: &Line, cursor_col: u16) -> Option<Self> {
        let old_cols = usize::from(line.cols());
        if line.is_wrapped() || old_cols == 0 {
            return None;
        }
        if Self::is_spacer(line.cell(line.cols().saturating_sub(1))?) {
            return None;
        }

        let mut longest_gap = (0, 0);
        let mut run_start = None;
        for col in usize::from(cursor_col)..old_cols {
            let spacer = line.cell(col as u16).is_some_and(Self::is_spacer);
            match (run_start, spacer) {
                (None, true) => run_start = Some(col),
                (Some(start), false) => {
                    if col.saturating_sub(start) > longest_gap.1 - longest_gap.0 {
                        longest_gap = (start, col);
                    }
                    run_start = None;
                }
                _ => {}
            }
        }
        let (gap_start, right_start) = longest_gap;
        let gap = right_start.saturating_sub(gap_start);
        if gap < 2 || usize::from(cursor_col) > gap_start {
            return None;
        }

        Some(Self {
            left: (0..gap_start)
                .filter_map(|col| line.cell(col as u16).cloned())
                .collect(),
            right: (right_start..old_cols)
                .filter_map(|col| line.cell(col as u16).cloned())
                .collect(),
        })
    }

    fn capture(line: &Line, cursor_col: u16, new_cols: u16) -> Option<Self> {
        let anchor = Self::split(line, cursor_col)?;
        let new_cols = usize::from(new_cols.max(1));
        if new_cols >= usize::from(line.cols())
            || anchor
                .left
                .len()
                .saturating_add(1)
                .saturating_add(anchor.right.len())
                > new_cols
        {
            return None;
        }
        Some(anchor)
    }

    pub(crate) fn capture_current(line: &Line, cursor_col: u16) -> Option<Self> {
        Self::split(line, cursor_col)
    }

    fn render(&self, cols: u16, generation: u64) -> Line {
        let mut line = Line::blank(cols);
        for (col, cell) in self.left.iter().cloned().enumerate() {
            line.replace_cell(col as u16, cell);
        }
        let right_start = usize::from(cols).saturating_sub(self.right.len());
        for (offset, cell) in self.right.iter().cloned().enumerate() {
            line.replace_cell((right_start + offset) as u16, cell);
        }
        line.set_generation(generation);
        line
    }
}

impl PartialEq for Grid {
    fn eq(&self, other: &Self) -> bool {
        self.cols == other.cols
            && self.rows == other.rows
            && self.visible == other.visible
            && self.history == other.history
            && self.history_rows == other.history_rows
            && self.below == other.below
            && self.history_limit == other.history_limit
    }
}

impl Eq for Grid {}

impl Grid {
    pub fn new(cols: u16, rows: u16) -> Self {
        let cols = cols.max(1);
        let rows = rows.max(1);
        Self {
            cols,
            rows,
            visible: vec![Line::blank(cols); rows as usize],
            history: VecDeque::new(),
            history_rows: 0,
            below: Vec::new(),
            history_limit: 10_000,
            history_revision: 0,
            history_added: 0,
            history_cache: Vec::new(),
        }
    }

    pub const fn cols(&self) -> u16 {
        self.cols
    }

    pub const fn rows(&self) -> u16 {
        self.rows
    }

    pub fn line(&self, row: u16) -> Option<&Line> {
        self.visible.get(row as usize)
    }

    pub fn line_mut(&mut self, row: u16) -> Option<&mut Line> {
        self.below.clear();
        self.visible.get_mut(row as usize)
    }

    pub(crate) fn set_line_generation(&mut self, row: u16, generation: u64) {
        if let Some(line) = self.visible.get_mut(row as usize) {
            line.set_generation(generation);
        }
    }

    pub fn set(&mut self, row: u16, col: u16, ch: char, width: u8, style: Style) {
        if let Some(line) = self.line_mut(row) {
            line.set(col, ch, width, style);
        }
    }

    pub fn set_text(&mut self, row: u16, col: u16, text: CellText, width: u8, style: Style) {
        if let Some(line) = self.line_mut(row) {
            line.set_text(col, text, width, style);
        }
    }

    pub(crate) fn append_grapheme(&mut self, row: u16, col: u16, ch: char) -> GraphemeAppend {
        self.line_mut(row)
            .map_or(GraphemeAppend::NotExtension, |line| {
                line.append_grapheme(col, ch)
            })
    }

    pub fn set_wrapped(&mut self, row: u16, wrapped: bool) {
        if let Some(line) = self.line_mut(row) {
            line.set_wrapped(wrapped);
        }
    }

    pub fn clear(&mut self) {
        self.below.clear();
        for line in &mut self.visible {
            line.clear_all();
        }
    }

    pub fn clear_history(&mut self) {
        if self.history.is_empty() {
            return;
        }
        self.history.clear();
        self.history_rows = 0;
        self.invalidate_history_cache();
    }

    pub fn scroll_up_whole_screen(&mut self, count: u16) {
        for _ in 0..count.max(1) {
            if self.visible.is_empty() {
                return;
            }
            let removed = self.visible.remove(0);
            self.push_history_line(removed);
            self.below.clear();
            self.trim_history();
            self.visible.push(Line::blank(self.cols));
        }
    }

    pub fn scroll_region_up(&mut self, top: u16, bottom: u16, count: u16) {
        self.scroll_region_up_inner(top, bottom, count, false);
    }

    pub fn scroll_top_region_up(&mut self, bottom: u16, count: u16) {
        self.scroll_region_up_inner(0, bottom, count, true);
    }

    fn scroll_region_up_inner(
        &mut self,
        top: u16,
        bottom: u16,
        count: u16,
        preserve_history: bool,
    ) {
        self.below.clear();
        let top = top.min(self.rows.saturating_sub(1)) as usize;
        let bottom = bottom.min(self.rows.saturating_sub(1)) as usize;
        if top > bottom {
            return;
        }
        for _ in 0..count.max(1) {
            let removed = self.visible.remove(top);
            if preserve_history {
                self.push_history_line(removed);
            }
            self.visible.insert(bottom, Line::blank(self.cols));
        }
        if preserve_history {
            self.trim_history();
        }
    }

    pub fn scroll_region_down(&mut self, top: u16, bottom: u16, count: u16) {
        self.below.clear();
        let top = top.min(self.rows.saturating_sub(1)) as usize;
        let bottom = bottom.min(self.rows.saturating_sub(1)) as usize;
        if top > bottom {
            return;
        }
        for _ in 0..count.max(1) {
            self.visible.remove(bottom);
            self.visible.insert(top, Line::blank(self.cols));
        }
    }

    pub fn resize(&mut self, cols: u16, rows: u16) {
        let _ = self.resize_with_cursor(cols, rows, 0, 0);
    }

    pub fn resize_without_reflow(
        &mut self,
        cols: u16,
        rows: u16,
        cursor_row: u16,
        cursor_col: u16,
    ) -> (u16, u16) {
        let cols = cols.max(1);
        let rows = rows.max(1);

        for line in &mut self.visible {
            line.resize(cols);
        }
        self.below.clear();

        let row_count = rows as usize;
        if self.visible.len() > row_count {
            self.visible.truncate(row_count);
        }
        while self.visible.len() < row_count {
            self.visible.push(Line::blank(cols));
        }

        self.cols = cols;
        self.rows = rows;

        (
            cursor_row.min(rows.saturating_sub(1)),
            cursor_col.min(cols.saturating_sub(1)),
        )
    }

    pub fn resize_with_cursor(
        &mut self,
        cols: u16,
        rows: u16,
        cursor_row: u16,
        cursor_col: u16,
    ) -> (u16, u16) {
        let cols = cols.max(1);
        let rows = rows.max(1);
        let old_cols = self.cols;
        let old_rows = self.rows;

        let mut cursor_row = cursor_row.min(old_rows.saturating_sub(1)) as usize;
        let mut cursor_col = cursor_col.min(old_cols.saturating_sub(1)) as usize;

        if cols != old_cols {
            let cursor_line_anchor = self
                .visible
                .get(cursor_row)
                .and_then(|line| CursorLineEdgeAnchor::capture(line, cursor_col as u16, cols));
            let old_visible_len = self.visible.len();
            let mut old_lines = Vec::with_capacity(old_visible_len + self.below.len() + 8);
            let history_boundary_rows = self.take_incomplete_history_tail(old_cols, &mut old_lines);
            old_lines.extend(self.visible.iter().cloned());
            old_lines.extend(self.below.iter().cloned());
            let (top_wrap_col, top_wrap_row) = wrap_position(&old_lines, 0, history_boundary_rows);
            let cursor_abs = history_boundary_rows.saturating_add(cursor_row);
            let (wrap_col, wrap_row) = wrap_position(&old_lines, cursor_col, cursor_abs);
            let anchored_row = cursor_line_anchor
                .as_ref()
                .map(|anchor| (history_boundary_rows.saturating_add(cursor_row), anchor));
            let mut reflowed = reflow_lines_with_anchor(&old_lines, cols, anchored_row);
            if reflowed.len() < rows as usize {
                let missing = rows as usize - reflowed.len();
                for _ in 0..missing {
                    reflowed.push(Line::blank(cols));
                }
            }
            let (new_cursor_col, new_cursor_abs) =
                unwrap_position(&reflowed, wrap_col, wrap_row, cols);
            let (_, visible_start) = unwrap_position(&reflowed, top_wrap_col, top_wrap_row, cols);
            let visible_start = visible_start.min(reflowed.len());
            let visible_end = visible_start
                .saturating_add(old_visible_len)
                .min(reflowed.len());

            for line in reflowed.drain(..visible_start) {
                self.push_history_line(line);
            }
            let visible_end = visible_end.saturating_sub(visible_start);
            self.visible = reflowed.drain(..visible_end).collect();
            self.below = reflowed;
            cursor_row = if new_cursor_abs < visible_start {
                0
            } else {
                new_cursor_abs.saturating_sub(visible_start)
            };
            cursor_col = new_cursor_col;
        }

        self.resize_height(rows, &mut cursor_row);

        while self.visible.len() < rows as usize {
            self.visible.push(Line::blank(cols));
        }

        self.cols = cols;
        self.rows = rows;
        self.trim_history();

        (
            cursor_row.min(rows.saturating_sub(1) as usize) as u16,
            cursor_col.min(cols.saturating_sub(1) as usize) as u16,
        )
    }

    fn resize_height(&mut self, rows: u16, cursor_row: &mut usize) {
        let old_rows = self.visible.len();
        let new_rows = rows as usize;

        if new_rows < old_rows {
            let moved_below = self.visible.split_off(new_rows);
            self.below.splice(0..0, moved_below);
            *cursor_row = (*cursor_row).min(new_rows.saturating_sub(1));
        } else if new_rows > old_rows {
            let mut needed = new_rows - old_rows;
            let pull_from_below = needed.min(self.below.len());
            if pull_from_below > 0 {
                let pulled = self.below.drain(..pull_from_below);
                self.visible.extend(pulled);
                needed -= pull_from_below;
            }

            for _ in 0..needed {
                self.visible.push(Line::blank(self.cols));
            }
        }
    }

    fn trim_history(&mut self) {
        let mut changed = false;
        while self.history_rows > self.history_limit && !self.history.is_empty() {
            let removed = self
                .history
                .pop_front()
                .expect("non-empty history disappeared");
            self.history_rows = self.history_rows.saturating_sub(removed.captured_rows);
            changed = true;
        }
        if changed {
            self.invalidate_history_cache();
        }
    }

    pub fn history_len(&self) -> usize {
        self.history_rows
    }

    pub const fn history_added(&self) -> u64 {
        self.history_added
    }

    pub fn set_history_limit(&mut self, history_limit: usize) {
        self.history_limit = history_limit;
        self.trim_history();
    }

    /// Materialize canonical scrollback at a requested width. Resize never
    /// calls this: copy/search consumers pay the wrapping cost only when they
    /// actually inspect old history, and repeated reads reuse the cached rows.
    pub fn history_lines_at_width(&mut self, cols: u16) -> Arc<Vec<Line>> {
        let cols = cols.max(1);
        if let Some(entry) = self
            .history_cache
            .iter()
            .find(|entry| entry.cols == cols && entry.revision == self.history_revision)
        {
            return Arc::clone(&entry.lines);
        }

        let mut lines = Vec::new();
        for logical in &self.history {
            lines.extend(logical.reflow(cols));
        }
        let lines = Arc::new(lines);
        if self.history_cache.len() == 2 {
            self.history_cache.remove(0);
        }
        self.history_cache.push(HistoryCacheEntry {
            cols,
            revision: self.history_revision,
            lines: Arc::clone(&lines),
        });
        lines
    }

    /// Materialize a viewport `offset` rows above the live bottom.
    pub fn viewport_lines_at_width(&mut self, cols: u16, rows: u16, offset: usize) -> Vec<Line> {
        let cols = cols.max(1);
        let rows = rows.max(1) as usize;
        let history = self.history_lines_at_width(cols);
        let live = self
            .visible
            .iter()
            .cloned()
            .map(|mut line| {
                line.resize(cols);
                line
            })
            .collect::<Vec<_>>();
        let total = history.len() + live.len();
        let offset = offset.min(history.len());
        let end = total.saturating_sub(offset);
        let start = end.saturating_sub(rows);
        let mut out = Vec::with_capacity(rows);
        out.extend((0..rows.saturating_sub(end - start)).map(|_| Line::blank(cols)));
        for index in start..end {
            if index < history.len() {
                out.push(history[index].clone());
            } else {
                out.push(live[index - history.len()].clone());
            }
        }
        out
    }

    pub fn max_viewport_offset_at_width(&mut self, cols: u16) -> usize {
        self.history_lines_at_width(cols).len()
    }

    pub fn copy_lines_at_width(&mut self, cols: u16) -> (Vec<Line>, usize) {
        let history = self.history_lines_at_width(cols);
        let history_len = history.len();
        let mut lines = Vec::with_capacity(history_len + self.visible.len());
        lines.extend(history.iter().cloned());
        lines.extend(self.visible.iter().cloned().map(|mut line| {
            line.resize(cols);
            line
        }));
        (lines, history_len)
    }

    fn push_history_line(&mut self, line: Line) {
        if self.history.back().is_some_and(|logical| !logical.complete) {
            self.history
                .back_mut()
                .expect("incomplete history tail disappeared")
                .append_physical(line);
        } else {
            self.history.push_back(LogicalLine::from_physical(line));
        }
        self.history_rows += 1;
        self.history_added = self.history_added.wrapping_add(1);
        self.invalidate_history_cache();
    }

    fn take_incomplete_history_tail(&mut self, cols: u16, out: &mut Vec<Line>) -> usize {
        let Some(tail) = self.history.back() else {
            return 0;
        };
        if tail.complete {
            return 0;
        }

        let tail = self.history.pop_back().expect("history tail disappeared");
        self.history_rows = self.history_rows.saturating_sub(tail.captured_rows);
        let rows = tail.reflow(cols);
        let count = rows.len();
        out.extend(rows);
        self.invalidate_history_cache();
        count
    }

    fn invalidate_history_cache(&mut self) {
        self.history_revision = self.history_revision.wrapping_add(1);
        self.history_cache.clear();
    }
}

fn take_content_cells(line: Line) -> Arc<Vec<Cell>> {
    let can_reuse = !line.cells.iter().any(Cell::is_continuation)
        && (!line.wrapped || line.cells.len() == line.cols as usize);
    if can_reuse {
        line.cells
    } else {
        Arc::new(line.content_cells())
    }
}

fn wrap_position(lines: &[Line], col: usize, row: usize) -> (usize, usize) {
    let mut wrap_col = 0;
    let mut wrap_row = 0;

    for line in lines.iter().take(row) {
        if line.is_wrapped() {
            wrap_col += display_width(&line.content_cells());
        } else {
            wrap_col = 0;
            wrap_row += 1;
        }
    }

    if let Some(line) = lines.get(row) {
        let cells = line.content_cells();
        wrap_col += col.min(display_width(&cells));
    }

    (wrap_col, wrap_row)
}

fn unwrap_position(lines: &[Line], wrap_col: usize, wrap_row: usize, cols: u16) -> (usize, usize) {
    let mut current_wrap_row = 0;
    let mut current_wrap_col = 0;

    for (row, line) in lines.iter().enumerate() {
        if current_wrap_row == wrap_row {
            let content_width = display_width(&line.content_cells());
            let col = wrap_col.saturating_sub(current_wrap_col);
            if col < content_width || !line.is_wrapped() {
                return (col.min(cols.saturating_sub(1) as usize), row);
            }
            current_wrap_col += content_width;
        }

        if !line.is_wrapped() {
            current_wrap_row += 1;
            current_wrap_col = 0;
        }
    }

    (
        cols.saturating_sub(1) as usize,
        lines.len().saturating_sub(1),
    )
}

fn display_width(cells: &[Cell]) -> usize {
    cells
        .iter()
        .map(|cell| usize::from(cell.width().max(1)))
        .sum()
}

#[cfg(test)]
fn reflow_lines(lines: &[Line], cols: u16) -> Vec<Line> {
    reflow_lines_with_anchor(lines, cols, None)
}

fn reflow_lines_with_anchor(
    lines: &[Line],
    cols: u16,
    anchor: Option<(usize, &CursorLineEdgeAnchor)>,
) -> Vec<Line> {
    let mut logical_lines: Vec<(Vec<Cell>, u64)> = Vec::new();
    let mut current = Vec::new();
    let mut generation = 0;
    let mut reflowed = Vec::new();

    for (row, line) in lines.iter().enumerate() {
        if anchor.is_some_and(|(anchor_row, _)| anchor_row == row) && current.is_empty() {
            let (_, anchor) = anchor.expect("checked above");
            for (logical, logical_generation) in logical_lines.drain(..) {
                reflow_logical_line(&logical, cols, logical_generation, &mut reflowed);
            }
            reflowed.push(anchor.render(cols, line.generation()));
            continue;
        }
        current.extend(line.content_cells());
        generation = generation.max(line.generation());
        if !line.is_wrapped() {
            logical_lines.push((std::mem::take(&mut current), generation));
            generation = 0;
        }
    }
    if !current.is_empty() {
        logical_lines.push((current, generation));
    }

    for (logical, generation) in logical_lines {
        reflow_logical_line(&logical, cols, generation, &mut reflowed);
    }
    reflowed
}

fn reflow_logical_line(cells: &[Cell], cols: u16, generation: u64, out: &mut Vec<Line>) {
    let cols = cols.max(1);
    let mut line = Line::blank(cols);
    let mut col = 0_u16;

    if cells.is_empty() {
        line.set_generation(generation);
        out.push(line);
        return;
    }

    for cell in cells {
        let width = u16::from(cell.width().max(1));
        if col > 0 && col.saturating_add(width) > cols {
            line.set_wrapped(true);
            line.set_generation(generation);
            out.push(line);
            line = Line::blank(cols);
            col = 0;
        }
        line.set_text(col, cell.text().clone(), cell.width().max(1), cell.style());
        col = col.saturating_add(width).min(cols);
    }
    line.set_generation(generation);
    out.push(line);
}

#[cfg(test)]
mod tests {
    use super::{reflow_lines, Cell, Color, Grid, Line, Style};
    use crate::CellText;
    use std::sync::Arc;

    #[test]
    fn configurable_history_limit_trims_existing_history() {
        let mut grid = Grid::new(10, 2);
        grid.set_history_limit(4);
        for _ in 0..8 {
            grid.scroll_up_whole_screen(1);
        }
        assert_eq!(grid.history_len(), 4);

        grid.set_history_limit(2);
        assert_eq!(grid.history_len(), 2);
    }

    #[test]
    fn historical_viewport_is_offset_from_live_bottom() {
        let mut grid = Grid::new(8, 2);
        for value in ["one", "two", "three"] {
            for (col, ch) in value.chars().enumerate() {
                grid.set(1, col as u16, ch, 1, Style::default());
            }
            grid.scroll_up_whole_screen(1);
        }
        for (col, ch) in "live".chars().enumerate() {
            grid.set(1, col as u16, ch, 1, Style::default());
        }

        let live = grid.viewport_lines_at_width(8, 2, 0);
        let history = grid.viewport_lines_at_width(8, 2, 2);
        assert_eq!(
            live.iter().map(Line::text).collect::<Vec<_>>(),
            ["three", "live"]
        );
        assert_eq!(
            history.iter().map(Line::text).collect::<Vec<_>>(),
            ["one", "two"]
        );
        assert_eq!(grid.max_viewport_offset_at_width(8), 3);
        assert_eq!(grid.history_added(), 3);
    }

    #[test]
    fn trailing_default_cells_are_implicit() {
        let mut line = Line::blank(160);
        assert_eq!(line.stored_len(), 0);
        assert_eq!(line.cells.capacity(), 0);
        assert_eq!(line.cell(159), Some(&Cell::blank()));

        line.set(120, 'x', 1, Style::default());
        assert_eq!(line.stored_len(), 121);
        line.clear_range(120, 160);
        assert_eq!(line.stored_len(), 0);
    }

    #[test]
    fn non_default_blank_cells_remain_explicit() {
        let mut line = Line::blank(80);
        let style = Style {
            bg: super::Color::Indexed(4),
            ..Style::default()
        };
        line.clear_range_with_style(70, 80, style);
        assert_eq!(line.stored_len(), 80);
        assert_eq!(line.cell(79).map(Cell::style), Some(style));
    }

    #[test]
    fn cloned_lines_are_copy_on_write() {
        let mut original = Line::blank(80);
        original.set(0, 'a', 1, Style::default());
        let mut snapshot = original.clone();
        assert!(original.shares_cells_with(&snapshot));

        snapshot.set(0, 'b', 1, Style::default());
        assert!(!original.shares_cells_with(&snapshot));
        assert_eq!(original.text(), "a");
        assert_eq!(snapshot.text(), "b");
    }

    #[test]
    fn replacing_either_half_of_a_wide_cell_clears_the_other_half() {
        let mut line = Line::blank(8);
        line.set(0, '\u{754c}', 2, Style::default());

        line.set(1, 'x', 1, Style::default());
        assert_eq!(line.text(), " x");
        assert_eq!(line.width_at(0), 1);
        assert_eq!(line.width_at(1), 1);

        line.set(2, '\u{754c}', 2, Style::default());
        line.set(2, 'y', 1, Style::default());
        assert_eq!(line.width_at(2), 1);
        assert_eq!(line.width_at(3), 1);
    }

    #[test]
    fn equal_non_default_styles_are_interned() {
        let style = Style {
            bold: true,
            fg: super::Color::Rgb(10, 20, 30),
            ..Style::default()
        };
        let left = Cell::printable('a', 1, style);
        let right = Cell::printable('b', 1, style);
        assert_eq!(left.metadata.style(), right.metadata.style());
        assert_eq!(std::mem::size_of::<super::StyleId>(), 8);
        assert!(std::mem::size_of::<Cell>() <= 16);
    }

    #[test]
    fn terminal_default_colours_are_explicit_and_interned() {
        let style = Style::default();
        assert_eq!(style.fg, Color::Default);
        assert_eq!(style.bg, Color::Default);
        assert_eq!(Cell::blank().style(), style);
    }

    #[test]
    fn completed_history_is_not_reflowed_during_width_resize() {
        let mut grid = Grid::new(12, 2);
        for value in ["first", "second", "third"] {
            for (col, ch) in value.chars().enumerate() {
                grid.set(0, col as u16, ch, 1, Style::default());
            }
            grid.scroll_up_whole_screen(1);
        }
        let first_cells = Arc::clone(&grid.history[0].cells);
        let revision = grid.history_revision;

        grid.resize_with_cursor(5, 2, 0, 0);

        assert!(Arc::ptr_eq(&first_cells, &grid.history[0].cells));
        assert_eq!(grid.history_revision, revision);
        assert!(grid.history_cache.is_empty());
    }

    #[test]
    fn resize_reflows_only_the_logical_line_crossing_the_viewport() {
        let mut grid = Grid::new(4, 2);
        for (col, ch) in "abcd".chars().enumerate() {
            grid.set(0, col as u16, ch, 1, Style::default());
        }
        grid.set_wrapped(0, true);
        for (col, ch) in "efgh".chars().enumerate() {
            grid.set(1, col as u16, ch, 1, Style::default());
        }
        grid.scroll_up_whole_screen(1);

        let cursor = grid.resize_with_cursor(2, 2, 0, 1);

        assert_eq!(grid.line(0).map(Line::text).as_deref(), Some("ef"));
        assert_eq!(grid.line(1).map(Line::text).as_deref(), Some("gh"));
        assert_eq!(cursor, (0, 1));
        let history = grid.history_lines_at_width(2);
        assert_eq!(
            history.iter().map(Line::text).collect::<Vec<_>>(),
            ["ab", "cd"]
        );
        assert!(history.last().is_some_and(Line::is_wrapped));
    }

    #[test]
    fn historical_wraps_are_lazy_and_cached_by_width() {
        let mut grid = Grid::new(10, 2);
        for (col, ch) in "historical".chars().enumerate() {
            grid.set(0, col as u16, ch, 1, Style::default());
        }
        grid.scroll_up_whole_screen(1);
        grid.resize_with_cursor(4, 2, 0, 0);
        assert!(grid.history_cache.is_empty());

        let first = grid.history_lines_at_width(4);
        let second = grid.history_lines_at_width(4);
        assert!(Arc::ptr_eq(&first, &second));
        assert_eq!(
            first.iter().map(Line::text).collect::<Vec<_>>(),
            ["hist", "oric", "al"]
        );

        let other_width = grid.history_lines_at_width(5);
        assert_eq!(
            other_width.iter().map(Line::text).collect::<Vec<_>>(),
            ["histo", "rical"]
        );
        assert_eq!(grid.history_cache.len(), 2);
    }

    #[test]
    fn combined_text_survives_wrapped_history_reflow() {
        let mut grid = Grid::new(4, 2);
        let mut text = CellText::from('e');
        assert!(text.try_append('\u{301}'));
        grid.set_text(0, 0, text, 1, Style::default());
        grid.set(0, 1, 'x', 1, Style::default());
        grid.set(0, 2, 'y', 1, Style::default());
        grid.set(0, 3, 'z', 1, Style::default());
        grid.set_wrapped(0, true);
        grid.scroll_up_whole_screen(1);

        let history = grid.history_lines_at_width(2);
        assert_eq!(
            history.iter().map(Line::text).collect::<Vec<_>>(),
            ["e\u{301}x", "yz"]
        );
    }

    #[test]
    fn wide_text_survives_shrink_to_one_column_and_growth() {
        let mut original = Line::blank(4);
        let mut emoji = CellText::from('\u{1f469}');
        assert!(emoji.try_append('\u{200d}'));
        assert!(emoji.try_append('\u{1f4bb}'));
        original.set_text(0, emoji, 2, Style::default());
        original.set(2, 'x', 1, Style::default());

        let narrow = reflow_lines(&[original], 1);
        assert_eq!(
            narrow.iter().map(Line::text).collect::<Vec<_>>(),
            ["\u{1f469}\u{200d}\u{1f4bb}", "x"]
        );
        assert_eq!(narrow[0].width_at(0), 2);

        let grown = reflow_lines(&narrow, 4);
        assert_eq!(
            grown.iter().map(Line::text).collect::<Vec<_>>(),
            ["\u{1f469}\u{200d}\u{1f4bb}x"]
        );
        assert_eq!(grown[0].width_at(0), 2);
        assert_eq!(grown[0].width_at(1), 0);
    }

    #[test]
    fn nonreflow_line_resize_restores_wide_cell_continuation_after_growth() {
        let mut line = Line::blank(4);
        line.set(0, '\u{754c}', 2, Style::default());

        line.resize(1);
        assert_eq!(line.text(), "\u{754c}");
        assert_eq!(line.width_at(0), 2);

        line.resize(4);
        assert_eq!(line.text(), "\u{754c}");
        assert_eq!(line.width_at(0), 2);
        assert_eq!(line.width_at(1), 0);
    }
}
