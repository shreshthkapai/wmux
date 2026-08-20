use std::collections::VecDeque;

use crate::{scalar_width, Cell, Color, Grid, Line, Style};
use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};

const DAMAGE_JOURNAL_CAPACITY: usize = 512;
const MAX_DAMAGE_OPERATIONS_PER_BATCH: usize = 256;
pub const MAX_TITLE_BYTES: usize = 512;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum InsertDeleteKind {
    InsertChars,
    DeleteChars,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum CursorStyle {
    #[default]
    Default,
    BlinkingBlock,
    SteadyBlock,
    BlinkingUnderline,
    SteadyUnderline,
    BlinkingBar,
    SteadyBar,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum MouseTrackingMode {
    #[default]
    Off,
    X10,
    Normal,
    Button,
    Any,
}

#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
struct MouseModes {
    x10: bool,
    normal: bool,
    button: bool,
    any: bool,
    utf8: bool,
    sgr: bool,
    urxvt: bool,
}

impl MouseModes {
    fn tracking(self) -> MouseTrackingMode {
        if self.any {
            MouseTrackingMode::Any
        } else if self.button {
            MouseTrackingMode::Button
        } else if self.normal {
            MouseTrackingMode::Normal
        } else if self.x10 {
            MouseTrackingMode::X10
        } else {
            MouseTrackingMode::Off
        }
    }
}

impl CursorStyle {
    pub const fn from_decscusr(value: u16) -> Option<Self> {
        match value {
            0 => Some(Self::Default),
            1 => Some(Self::BlinkingBlock),
            2 => Some(Self::SteadyBlock),
            3 => Some(Self::BlinkingUnderline),
            4 => Some(Self::SteadyUnderline),
            5 => Some(Self::BlinkingBar),
            6 => Some(Self::SteadyBar),
            _ => None,
        }
    }

    pub const fn decscusr(self) -> u8 {
        match self {
            Self::Default => 0,
            Self::BlinkingBlock => 1,
            Self::SteadyBlock => 2,
            Self::BlinkingUnderline => 3,
            Self::SteadyUnderline => 4,
            Self::BlinkingBar => 5,
            Self::SteadyBar => 6,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum DamageOperation {
    PrintRun {
        start: (u16, u16),
        end: (u16, u16),
    },
    ClearRange {
        row: u16,
        start: u16,
        end: u16,
    },
    ScrollRegion {
        top: u16,
        bottom: u16,
        lines: i32,
    },
    InsertDelete {
        row: u16,
        col: u16,
        count: u16,
        kind: InsertDeleteKind,
    },
    CursorMove {
        from: (u16, u16),
        to: (u16, u16),
    },
    ModeChange {
        mode: u16,
        enabled: bool,
    },
    TitleChange,
    Full,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct DamageBatch {
    pub generation: u64,
    pub operations: Vec<DamageOperation>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct DamageStatus {
    pub current_generation: u64,
    pub retained_batches: usize,
    pub requires_full_redraw: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Screen {
    primary: Grid,
    alternate: Grid,
    alternate_active: bool,
    cursor_row: u16,
    cursor_col: u16,
    saved_cursor: Option<(u16, u16)>,
    scroll_top: u16,
    scroll_bottom: u16,
    pending_primary_rows: Vec<bool>,
    pending_alternate_rows: Vec<bool>,
    update_active: bool,
    pending_damage: Vec<DamageOperation>,
    generation: u64,
    damage_journal: VecDeque<DamageBatch>,
    bracketed_paste: bool,
    mouse_modes: MouseModes,
    synchronized_output: bool,
    synchronized_output_epoch: u64,
    cursor_visible: bool,
    cursor_style: CursorStyle,
    pending_wrap: bool,
    current_style: Style,
    title: String,
}

impl Screen {
    pub fn new(cols: u16, rows: u16) -> Self {
        let rows = rows.max(1);
        Self {
            primary: Grid::new(cols, rows),
            alternate: Grid::new(cols, rows),
            alternate_active: false,
            cursor_row: 0,
            cursor_col: 0,
            saved_cursor: None,
            scroll_top: 0,
            scroll_bottom: rows - 1,
            pending_primary_rows: vec![false; rows as usize],
            pending_alternate_rows: vec![false; rows as usize],
            update_active: false,
            pending_damage: Vec::new(),
            generation: 0,
            damage_journal: VecDeque::with_capacity(DAMAGE_JOURNAL_CAPACITY),
            bracketed_paste: false,
            mouse_modes: MouseModes::default(),
            synchronized_output: false,
            synchronized_output_epoch: 0,
            cursor_visible: true,
            cursor_style: CursorStyle::Default,
            pending_wrap: false,
            current_style: Style::default(),
            title: String::new(),
        }
    }

    pub fn grid(&self) -> &Grid {
        if self.alternate_active {
            &self.alternate
        } else {
            &self.primary
        }
    }

    fn grid_mut(&mut self) -> &mut Grid {
        if self.alternate_active {
            &mut self.alternate
        } else {
            &mut self.primary
        }
    }

    pub fn cols(&self) -> u16 {
        self.grid().cols()
    }

    pub fn rows(&self) -> u16 {
        self.grid().rows()
    }

    pub const fn cursor(&self) -> (u16, u16) {
        (self.cursor_row, self.cursor_col)
    }

    pub fn title(&self) -> &str {
        &self.title
    }

    pub(crate) fn set_title(&mut self, title: &str) {
        let mut end = title.len().min(MAX_TITLE_BYTES);
        while !title.is_char_boundary(end) {
            end -= 1;
        }
        self.title.clear();
        self.title.push_str(&title[..end]);
    }

    pub fn render_cursor(&self) -> (u16, u16) {
        self.cursor()
    }

    pub fn render_line_cells(&self, row: u16) -> Option<Vec<Cell>> {
        self.grid().line(row).map(|line| {
            (0..line.cols())
                .filter_map(|col| line.cell(col).cloned())
                .collect()
        })
    }

    pub fn render_line(&self, row: u16) -> Option<&Line> {
        self.grid().line(row)
    }

    pub const fn bracketed_paste(&self) -> bool {
        self.bracketed_paste
    }

    pub fn mouse_tracking(&self) -> MouseTrackingMode {
        self.mouse_modes.tracking()
    }

    pub fn application_mouse_enabled(&self) -> bool {
        self.mouse_tracking() != MouseTrackingMode::Off
    }

    pub fn set_mouse_mode(&mut self, mode: u16, enabled: bool) {
        match mode {
            9 => self.mouse_modes.x10 = enabled,
            1000 => self.mouse_modes.normal = enabled,
            1002 => self.mouse_modes.button = enabled,
            1003 => self.mouse_modes.any = enabled,
            1005 => self.mouse_modes.utf8 = enabled,
            1006 => self.mouse_modes.sgr = enabled,
            1015 => self.mouse_modes.urxvt = enabled,
            _ => {}
        }
    }

    /// Encode a normalized mouse event for the application running in this
    /// pane. Coordinates are zero-based and relative to the pane viewport.
    pub fn encode_mouse(&self, event: MouseEvent, column: u16, row: u16) -> Option<Vec<u8>> {
        let tracking = self.mouse_tracking();
        let allowed = match event.kind {
            MouseEventKind::Move => tracking == MouseTrackingMode::Any,
            MouseEventKind::Drag => {
                matches!(tracking, MouseTrackingMode::Button | MouseTrackingMode::Any)
            }
            MouseEventKind::Up => matches!(
                tracking,
                MouseTrackingMode::Normal | MouseTrackingMode::Button | MouseTrackingMode::Any
            ),
            _ => tracking != MouseTrackingMode::Off,
        };
        if !allowed {
            return None;
        }

        let mut code = mouse_button_code(event)?;
        if event.modifiers.contains(MouseModifiers::SHIFT) {
            code += 4;
        }
        if event.modifiers.contains(MouseModifiers::ALT) {
            code += 8;
        }
        if event.modifiers.contains(MouseModifiers::CONTROL) {
            code += 16;
        }
        let release = event.kind == MouseEventKind::Up;
        let x = u32::from(column) + 1;
        let y = u32::from(row) + 1;

        if self.mouse_modes.sgr {
            return Some(
                format!("\x1b[<{code};{x};{y}{}", if release { 'm' } else { 'M' }).into_bytes(),
            );
        }
        if self.mouse_modes.urxvt {
            return Some(format!("\x1b[{};{x};{y}M", code + 32).into_bytes());
        }

        let legacy_code = if release { 3 + (code & !3) } else { code };
        let mut out = Vec::with_capacity(12);
        out.extend_from_slice(b"\x1b[M");
        if self.mouse_modes.utf8 {
            push_utf8_mouse_value(&mut out, legacy_code + 32)?;
            push_utf8_mouse_value(&mut out, x + 32)?;
            push_utf8_mouse_value(&mut out, y + 32)?;
        } else {
            out.push((legacy_code + 32).min(255) as u8);
            out.push((x + 32).min(255) as u8);
            out.push((y + 32).min(255) as u8);
        }
        Some(out)
    }

    pub const fn synchronized_output(&self) -> bool {
        self.synchronized_output
    }

    pub const fn synchronized_output_epoch(&self) -> u64 {
        self.synchronized_output_epoch
    }

    pub const fn cursor_visible(&self) -> bool {
        self.cursor_visible
    }

    pub const fn cursor_style(&self) -> CursorStyle {
        self.cursor_style
    }

    pub const fn alternate_active(&self) -> bool {
        self.alternate_active
    }

    pub const fn current_style(&self) -> Style {
        self.current_style
    }

    pub const fn generation(&self) -> u64 {
        self.generation
    }

    pub fn history_added(&self) -> u64 {
        self.primary.history_added()
    }

    pub fn viewport_lines(&mut self, cols: u16, rows: u16, offset: usize) -> Vec<Line> {
        if self.alternate_active {
            return (0..rows.max(1))
                .map(|row| {
                    let mut line = self
                        .alternate
                        .line(row)
                        .cloned()
                        .unwrap_or_else(|| Line::blank(cols));
                    line.resize(cols.max(1));
                    line
                })
                .collect();
        }
        self.primary.viewport_lines_at_width(cols, rows, offset)
    }

    pub fn copy_lines(&mut self, cols: u16) -> (Vec<Line>, usize) {
        self.grid_mut().copy_lines_at_width(cols)
    }

    pub fn max_viewport_offset(&mut self, cols: u16) -> usize {
        if self.alternate_active {
            0
        } else {
            self.primary.max_viewport_offset_at_width(cols)
        }
    }

    pub fn line_generation(&self, row: u16) -> Option<u64> {
        self.grid().line(row).map(|line| line.generation())
    }

    pub fn damage_status_since(&self, consumed_generation: u64) -> DamageStatus {
        let oldest = self.damage_journal.front().map(|batch| batch.generation);
        let requires_full_redraw = consumed_generation > self.generation
            || (consumed_generation < self.generation
                && oldest.is_none_or(|oldest| oldest > consumed_generation.saturating_add(1)));
        let retained_batches = self
            .damage_journal
            .iter()
            .filter(|batch| batch.generation > consumed_generation)
            .count();
        DamageStatus {
            current_generation: self.generation,
            retained_batches,
            requires_full_redraw,
        }
    }

    pub fn damage_journal(&self) -> &VecDeque<DamageBatch> {
        &self.damage_journal
    }

    pub(crate) fn begin_update(&mut self) {
        debug_assert!(!self.update_active, "terminal updates must not nest");
        self.update_active = true;
        self.pending_primary_rows.fill(false);
        self.pending_alternate_rows.fill(false);
        self.pending_damage.clear();
    }

    pub(crate) fn record_damage(&mut self, damage: DamageOperation) {
        debug_assert!(self.update_active, "damage must belong to an update batch");
        if self.pending_damage == [DamageOperation::Full] {
            return;
        }
        if let Some(last) = self.pending_damage.last_mut() {
            match (last, &damage) {
                (
                    DamageOperation::PrintRun { end, .. },
                    DamageOperation::PrintRun {
                        start,
                        end: next_end,
                    },
                ) if end == start => {
                    *end = *next_end;
                    return;
                }
                (
                    DamageOperation::CursorMove { to, .. },
                    DamageOperation::CursorMove { from, to: next_to },
                ) if to == from => {
                    *to = *next_to;
                    return;
                }
                (
                    DamageOperation::ClearRange { row, start, end },
                    DamageOperation::ClearRange {
                        row: next_row,
                        start: next_start,
                        end: next_end,
                    },
                ) if row == next_row && *next_start <= *end => {
                    *start = (*start).min(*next_start);
                    *end = (*end).max(*next_end);
                    return;
                }
                _ => {}
            }
        }
        if self.pending_damage.len() >= MAX_DAMAGE_OPERATIONS_PER_BATCH {
            self.pending_damage.clear();
            self.pending_damage.push(DamageOperation::Full);
            return;
        }
        self.pending_damage.push(damage);
    }

    pub(crate) fn finish_update(&mut self) -> Option<u64> {
        debug_assert!(self.update_active, "terminal update is not active");
        self.update_active = false;
        if self.pending_damage.is_empty()
            && !self.pending_primary_rows.iter().any(|changed| *changed)
            && !self.pending_alternate_rows.iter().any(|changed| *changed)
        {
            return None;
        }

        self.generation = self.generation.saturating_add(1);
        let generation = self.generation;
        for (row, changed) in self.pending_primary_rows.iter().copied().enumerate() {
            if changed {
                self.primary.set_line_generation(row as u16, generation);
            }
        }
        for (row, changed) in self.pending_alternate_rows.iter().copied().enumerate() {
            if changed {
                self.alternate.set_line_generation(row as u16, generation);
            }
        }
        if self.pending_damage.is_empty() {
            self.pending_damage.push(DamageOperation::Full);
        }
        self.damage_journal.push_back(DamageBatch {
            generation,
            operations: std::mem::take(&mut self.pending_damage),
        });
        if self.damage_journal.len() > DAMAGE_JOURNAL_CAPACITY {
            self.damage_journal.pop_front();
        }
        Some(generation)
    }

    pub fn mark_full_damage(&mut self) {
        let owns_update = !self.update_active;
        if owns_update {
            self.begin_update();
        }
        self.mark_all_rows_changed();
        self.record_damage(DamageOperation::Full);
        if owns_update {
            let _ = self.finish_update();
        }
    }

    pub fn set_history_limit(&mut self, history_limit: usize) {
        self.primary.set_history_limit(history_limit);
    }

    pub fn put_char(&mut self, ch: char) {
        match ch {
            '\r' => self.carriage_return(),
            '\n' => self.linefeed(),
            '\x08' => self.backspace(),
            '\t' => self.tab(),
            _ if ch >= ' ' => {
                if !ch.is_ascii() && self.append_to_previous_grapheme(ch) {
                    return;
                }
                let width = scalar_width(ch);
                if width == 0 {
                    return;
                }
                if self.pending_wrap || width > self.remaining_cols() {
                    self.linefeed_wrapped(true);
                    self.cursor_col = 0;
                    self.pending_wrap = false;
                }
                let row = self.cursor_row;
                let col = self.cursor_col;
                let style = self.current_style;
                self.grid_mut().set(row, col, ch, width, style);
                self.mark_dirty(row);
                self.advance(width);
            }
            _ => {}
        }
    }

    fn append_to_previous_grapheme(&mut self, ch: char) -> bool {
        let col = if self.pending_wrap {
            self.cursor_col
        } else {
            if self.cursor_col == 0 {
                return false;
            }
            self.grid()
                .line(self.cursor_row)
                .map_or(self.cursor_col - 1, |line| {
                    line.previous_cell_start(self.cursor_col)
                })
        };

        let row = self.cursor_row;
        let Some((_, new_width)) = self.grid_mut().append_grapheme(row, col, ch) else {
            return false;
        };
        self.mark_dirty(row);

        let end = col.saturating_add(u16::from(new_width));
        if end >= self.cols() {
            self.cursor_col = self.cols().saturating_sub(1);
            self.pending_wrap = true;
        } else {
            self.cursor_col = end;
            self.pending_wrap = false;
        }
        true
    }

    pub fn put_run(&mut self, mut text: &str) {
        while !text.is_empty() {
            let ascii = text
                .as_bytes()
                .iter()
                .take_while(|byte| (0x20..=0x7e).contains(*byte))
                .count();
            if ascii == 0 {
                let ch = text.chars().next().expect("non-empty text");
                self.put_char(ch);
                text = &text[ch.len_utf8()..];
                continue;
            }

            if self.pending_wrap {
                self.linefeed_wrapped(true);
                self.cursor_col = 0;
                self.pending_wrap = false;
            }
            let available = usize::from(self.cols().saturating_sub(self.cursor_col));
            let count = ascii.min(available);
            let row = self.cursor_row;
            let col = self.cursor_col;
            let style = self.current_style;
            let written = self.grid_mut().line_mut(row).map_or(0, |line| {
                line.set_ascii_run(col, &text.as_bytes()[..count], style)
            });
            if written == 0 {
                break;
            }
            self.mark_dirty(row);
            if usize::from(self.cursor_col) + written >= usize::from(self.cols()) {
                self.cursor_col = self.cols().saturating_sub(1);
                self.pending_wrap = true;
            } else {
                self.cursor_col += written as u16;
            }
            text = &text[written..];
        }
    }

    pub fn backspace(&mut self) {
        if self.pending_wrap {
            self.pending_wrap = false;
            return;
        }
        if let Some(line) = self.grid().line(self.cursor_row) {
            self.cursor_col = line.previous_cell_start(self.cursor_col);
        } else {
            self.cursor_col = self.cursor_col.saturating_sub(1);
        }
    }

    pub fn erase_cell_before_cursor(&mut self) {
        self.pending_wrap = false;
        if self.cursor_col == 0 {
            return;
        }
        if let Some(line) = self.grid().line(self.cursor_row) {
            self.cursor_col = line.previous_cell_start(self.cursor_col);
        } else {
            self.cursor_col -= 1;
        }
        let row = self.cursor_row;
        let col = self.cursor_col;
        let style = self.current_style;
        self.grid_mut().set(row, col, ' ', 1, style);
        self.mark_dirty(row);
    }

    pub fn tab(&mut self) {
        self.pending_wrap = false;
        let next = ((self.cursor_col / 8) + 1) * 8;
        self.cursor_col = next.min(self.cols().saturating_sub(1));
    }

    pub fn carriage_return(&mut self) {
        self.pending_wrap = false;
        self.cursor_col = 0;
    }

    pub fn linefeed(&mut self) {
        self.linefeed_wrapped(false);
    }

    fn linefeed_wrapped(&mut self, wrapped: bool) {
        self.pending_wrap = false;
        if wrapped {
            let row = self.cursor_row;
            self.grid_mut().set_wrapped(row, true);
        }
        if self.cursor_row == self.scroll_bottom {
            let top = self.scroll_top;
            let bottom = self.scroll_bottom;
            if top == 0 && bottom == self.rows().saturating_sub(1) {
                self.grid_mut().scroll_up_whole_screen(1);
            } else {
                self.grid_mut().scroll_region_up(top, bottom, 1);
            }
            self.mark_range_dirty(top, bottom);
        } else {
            self.cursor_row = (self.cursor_row + 1).min(self.rows().saturating_sub(1));
        }
    }

    pub fn newline(&mut self) {
        self.carriage_return();
        self.linefeed();
    }

    pub fn move_to(&mut self, row: u16, col: u16) {
        self.pending_wrap = false;
        self.cursor_row = row.min(self.rows().saturating_sub(1));
        self.cursor_col = col.min(self.cols().saturating_sub(1));
    }

    pub fn move_up(&mut self, count: u16) {
        self.pending_wrap = false;
        self.cursor_row = self.cursor_row.saturating_sub(count.max(1));
    }

    pub fn move_down(&mut self, count: u16) {
        self.pending_wrap = false;
        self.cursor_row = (self.cursor_row + count.max(1)).min(self.rows().saturating_sub(1));
    }

    pub fn move_right(&mut self, count: u16) {
        self.pending_wrap = false;
        self.cursor_col = (self.cursor_col + count.max(1)).min(self.cols().saturating_sub(1));
    }

    pub fn move_left(&mut self, count: u16) {
        self.pending_wrap = false;
        self.cursor_col = self.cursor_col.saturating_sub(count.max(1));
    }

    pub fn clear_screen(&mut self) {
        self.pending_wrap = false;
        self.grid_mut().clear();
        self.move_to(0, 0);
        self.mark_all_rows_changed();
    }

    pub fn clear_line(&mut self, mode: u16) {
        self.pending_wrap = false;
        let row = self.cursor_row;
        let col = self.cursor_col;
        let cols = self.cols();
        let style = self.current_style;
        if let Some(line) = self.grid_mut().line_mut(row) {
            match mode {
                1 => line.clear_range_with_style(0, col.saturating_add(1), style),
                2 => line.clear_all_with_style(style),
                _ => line.clear_range_with_style(col, cols, style),
            }
        }
        self.mark_dirty(row);
    }

    pub fn clear_screen_mode(&mut self, mode: u16) {
        self.pending_wrap = false;
        match mode {
            2 | 3 => self.clear_screen(),
            1 => {
                let row = self.cursor_row;
                let col = self.cursor_col;
                let style = self.current_style;
                for r in 0..row {
                    if let Some(line) = self.grid_mut().line_mut(r) {
                        line.clear_all_with_style(style);
                    }
                    self.mark_dirty(r);
                }
                if let Some(line) = self.grid_mut().line_mut(row) {
                    line.clear_range_with_style(0, col.saturating_add(1), style);
                }
                self.mark_dirty(row);
            }
            _ => {
                let row = self.cursor_row;
                let col = self.cursor_col;
                let rows = self.rows();
                let cols = self.cols();
                let style = self.current_style;
                if let Some(line) = self.grid_mut().line_mut(row) {
                    line.clear_range_with_style(col, cols, style);
                }
                self.mark_dirty(row);
                for r in row.saturating_add(1)..rows {
                    if let Some(line) = self.grid_mut().line_mut(r) {
                        line.clear_all_with_style(style);
                    }
                    self.mark_dirty(r);
                }
            }
        }
    }

    pub fn insert_blank_chars(&mut self, count: u16) {
        self.pending_wrap = false;
        let row = self.cursor_row;
        let col = self.cursor_col;
        let style = self.current_style;
        if let Some(line) = self.grid_mut().line_mut(row) {
            line.insert_blank_with_style(col, count, style);
        }
        self.mark_dirty(row);
    }

    pub fn delete_chars(&mut self, count: u16) {
        self.pending_wrap = false;
        let row = self.cursor_row;
        let col = self.cursor_col;
        let style = self.current_style;
        if let Some(line) = self.grid_mut().line_mut(row) {
            line.delete_with_style(col, count, style);
        }
        self.mark_dirty(row);
    }

    pub fn erase_chars(&mut self, count: u16) {
        self.pending_wrap = false;
        let row = self.cursor_row;
        let col = self.cursor_col;
        let end = col.saturating_add(count.max(1));
        let style = self.current_style;
        if let Some(line) = self.grid_mut().line_mut(row) {
            line.clear_range_with_style(col, end, style);
        }
        self.mark_dirty(row);
    }

    pub fn insert_lines(&mut self, count: u16) {
        self.pending_wrap = false;
        let row = self.cursor_row;
        let bottom = self.scroll_bottom;
        self.grid_mut().scroll_region_down(row, bottom, count);
        self.mark_range_dirty(row, bottom);
    }

    pub fn delete_lines(&mut self, count: u16) {
        self.pending_wrap = false;
        let row = self.cursor_row;
        let bottom = self.scroll_bottom;
        self.grid_mut().scroll_region_up(row, bottom, count);
        self.mark_range_dirty(row, bottom);
    }

    pub fn set_scroll_region(&mut self, top: u16, bottom: u16) {
        if top < bottom && bottom < self.rows() {
            self.scroll_top = top;
            self.scroll_bottom = bottom;
        } else {
            self.scroll_top = 0;
            self.scroll_bottom = self.rows().saturating_sub(1);
        }
        self.move_to(0, 0);
    }

    pub fn set_alternate(&mut self, enabled: bool) {
        self.pending_wrap = false;
        if self.alternate_active == enabled {
            return;
        }
        self.alternate_active = enabled;
        if enabled {
            self.alternate.clear();
        }
        self.move_to(0, 0);
        self.mark_all_rows_changed();
    }

    pub fn set_bracketed_paste(&mut self, enabled: bool) {
        self.bracketed_paste = enabled;
    }

    pub fn set_synchronized_output(&mut self, enabled: bool) {
        if self.synchronized_output == enabled {
            return;
        }
        let owns_update = !self.update_active;
        if owns_update {
            self.begin_update();
        }
        self.synchronized_output = enabled;
        if !enabled {
            self.synchronized_output_epoch = self.synchronized_output_epoch.wrapping_add(1);
            self.mark_all_rows_changed();
        }
        if owns_update {
            self.record_damage(DamageOperation::ModeChange {
                mode: 2026,
                enabled,
            });
            let _ = self.finish_update();
        }
    }

    pub fn set_cursor_visible(&mut self, visible: bool) {
        self.cursor_visible = visible;
    }

    pub fn set_cursor_style(&mut self, style: CursorStyle) {
        self.cursor_style = style;
    }

    pub fn reset_style(&mut self) {
        self.current_style = Style::default();
    }

    pub fn set_bold(&mut self, enabled: bool) {
        self.current_style.bold = enabled;
    }

    pub fn set_dim(&mut self, enabled: bool) {
        self.current_style.dim = enabled;
    }

    pub fn set_italic(&mut self, enabled: bool) {
        self.current_style.italic = enabled;
    }

    pub fn set_underline(&mut self, enabled: bool) {
        self.current_style.underline = enabled;
    }

    pub fn set_reverse(&mut self, enabled: bool) {
        self.current_style.reverse = enabled;
    }

    pub fn set_hidden(&mut self, enabled: bool) {
        self.current_style.hidden = enabled;
    }

    pub fn set_strikethrough(&mut self, enabled: bool) {
        self.current_style.strikethrough = enabled;
    }

    pub fn set_fg(&mut self, color: Color) {
        self.current_style.fg = color;
    }

    pub fn set_bg(&mut self, color: Color) {
        self.current_style.bg = color;
    }

    pub fn save_cursor(&mut self) {
        self.saved_cursor = Some((self.cursor_row, self.cursor_col));
    }

    pub fn restore_cursor(&mut self) {
        if let Some((row, col)) = self.saved_cursor {
            self.move_to(row, col);
        }
    }

    pub fn resize(&mut self, cols: u16, rows: u16) {
        let cols = cols.max(1);
        let rows = rows.max(1);
        if self.cols() == cols && self.rows() == rows {
            return;
        }
        let owns_update = !self.update_active;
        if owns_update {
            self.begin_update();
        }
        self.pending_wrap = false;
        let (cursor_row, cursor_col) = if self.alternate_active {
            self.primary.resize(cols, rows);
            self.alternate
                .resize_without_reflow(cols, rows, self.cursor_row, self.cursor_col)
        } else {
            let cursor =
                self.primary
                    .resize_with_cursor(cols, rows, self.cursor_row, self.cursor_col);
            self.alternate.resize(cols, rows);
            cursor
        };
        self.cursor_row = cursor_row;
        self.cursor_col = cursor_col;
        self.pending_primary_rows
            .resize(rows.max(1) as usize, false);
        self.pending_alternate_rows
            .resize(rows.max(1) as usize, false);
        self.scroll_top = 0;
        self.scroll_bottom = rows.max(1) - 1;
        self.pending_primary_rows.fill(true);
        self.pending_alternate_rows.fill(true);
        if owns_update {
            self.record_damage(DamageOperation::Full);
            let _ = self.finish_update();
        }
    }

    fn advance(&mut self, width: u8) {
        let width = u16::from(width);
        if self.cursor_col + width >= self.cols() {
            self.cursor_col = self.cols().saturating_sub(1);
            self.pending_wrap = true;
        } else {
            self.cursor_col += width;
        }
    }

    fn remaining_cols(&self) -> u8 {
        self.cols().saturating_sub(self.cursor_col).min(2) as u8
    }

    fn mark_dirty(&mut self, row: u16) {
        let changed_rows = if self.alternate_active {
            &mut self.pending_alternate_rows
        } else {
            &mut self.pending_primary_rows
        };
        if let Some(changed) = changed_rows.get_mut(row as usize) {
            *changed = true;
        }
    }

    fn mark_range_dirty(&mut self, top: u16, bottom: u16) {
        for row in top..=bottom {
            self.mark_dirty(row);
        }
    }

    fn mark_all_rows_changed(&mut self) {
        if self.alternate_active {
            self.pending_alternate_rows.fill(true);
        } else {
            self.pending_primary_rows.fill(true);
        }
    }

    pub(crate) const fn scroll_region(&self) -> (u16, u16) {
        (self.scroll_top, self.scroll_bottom)
    }
}

fn mouse_button_code(event: MouseEvent) -> Option<u32> {
    let button = match event.button {
        MouseButton::Left => 0,
        MouseButton::Middle => 1,
        MouseButton::Right => 2,
        MouseButton::None => 3,
    };
    Some(match event.kind {
        MouseEventKind::Down | MouseEventKind::Up => button,
        MouseEventKind::Drag => button + 32,
        MouseEventKind::Move => 3 + 32,
        MouseEventKind::ScrollUp => 64,
        MouseEventKind::ScrollDown => 65,
        MouseEventKind::ScrollLeft => 66,
        MouseEventKind::ScrollRight => 67,
    })
}

fn push_utf8_mouse_value(out: &mut Vec<u8>, value: u32) -> Option<()> {
    let ch = char::from_u32(value)?;
    let mut encoded = [0; 4];
    out.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
    Some(())
}

#[cfg(test)]
mod tests {
    use super::{DamageOperation, Screen, DAMAGE_JOURNAL_CAPACITY};
    use crate::TerminalEngine;

    #[test]
    fn batches_advance_pane_and_line_generations_once() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 3);

        assert_eq!(engine.feed(&mut screen, b"first\r\nsecond"), Some(1));
        assert_eq!(screen.generation(), 1);
        assert_eq!(screen.line_generation(0), Some(1));
        assert_eq!(screen.line_generation(1), Some(1));
        assert_eq!(screen.line_generation(2), Some(0));
        assert!(screen.damage_journal()[0]
            .operations
            .iter()
            .any(|operation| matches!(operation, DamageOperation::PrintRun { .. })));
    }

    #[test]
    fn clients_observe_damage_without_clearing_shared_state() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);
        engine.feed(&mut screen, b"one");
        let first_client = screen.damage_status_since(0);
        let second_client = screen.damage_status_since(0);

        assert_eq!(first_client, second_client);
        assert_eq!(first_client.retained_batches, 1);
        assert_eq!(screen.damage_status_since(1).retained_batches, 0);
        assert_eq!(screen.damage_status_since(0).retained_batches, 1);
    }

    #[test]
    fn journal_overflow_requires_only_lagging_clients_to_redraw_fully() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);
        for _ in 0..=DAMAGE_JOURNAL_CAPACITY {
            engine.feed(&mut screen, b"x\r");
        }

        assert_eq!(screen.damage_journal().len(), DAMAGE_JOURNAL_CAPACITY);
        assert!(screen.damage_status_since(0).requires_full_redraw);
        assert!(
            !screen
                .damage_status_since(screen.generation() - 1)
                .requires_full_redraw
        );
    }

    #[test]
    fn resize_to_current_dimensions_is_a_true_no_op() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(20, 2);
        engine.feed(&mut screen, b"content");
        let generation = screen.generation();
        let journal = screen.damage_journal().clone();

        screen.resize(20, 2);

        assert_eq!(screen.generation(), generation);
        assert_eq!(screen.damage_journal(), &journal);
    }
}
