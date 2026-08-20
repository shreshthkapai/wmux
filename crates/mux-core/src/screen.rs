use crate::grid::Grid;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Screen {
    primary: Grid,
    alternate: Grid,
    use_alternate: bool,
    cursor_row: u16,
    cursor_column: u16,
    saved_cursor: Option<(u16, u16)>,
    scroll_top: u16,
    scroll_bottom: u16,
    bracketed_paste: bool,
    dirty_rows: Vec<bool>,
}

impl Screen {
    pub fn new(columns: u16, rows: u16) -> Self {
        let rows = rows.max(1);
        Self {
            primary: Grid::new(columns, rows),
            alternate: Grid::new(columns, rows),
            use_alternate: false,
            cursor_row: 0,
            cursor_column: 0,
            saved_cursor: None,
            scroll_top: 0,
            scroll_bottom: rows.saturating_sub(1),
            bracketed_paste: false,
            dirty_rows: vec![true; rows as usize],
        }
    }

    pub fn grid(&self) -> &Grid {
        if self.use_alternate {
            &self.alternate
        } else {
            &self.primary
        }
    }

    fn grid_mut(&mut self) -> &mut Grid {
        if self.use_alternate {
            &mut self.alternate
        } else {
            &mut self.primary
        }
    }

    pub fn columns(&self) -> u16 {
        self.grid().columns()
    }

    pub fn rows(&self) -> u16 {
        self.grid().rows()
    }

    pub fn cursor_row(&self) -> u16 {
        self.cursor_row
    }

    pub fn cursor_column(&self) -> u16 {
        self.cursor_column
    }

    pub fn bracketed_paste(&self) -> bool {
        self.bracketed_paste
    }

    pub fn set_bracketed_paste(&mut self, enabled: bool) {
        self.bracketed_paste = enabled;
    }

    pub fn dirty_rows(&self) -> Vec<u16> {
        self.dirty_rows
            .iter()
            .enumerate()
            .filter_map(|(row, dirty)| dirty.then_some(row as u16))
            .collect()
    }

    pub fn clear_dirty(&mut self) {
        self.dirty_rows.fill(false);
    }

    pub fn mark_all_dirty(&mut self) {
        self.dirty_rows.fill(true);
    }

    pub fn write_char(&mut self, ch: char) {
        match ch {
            '\n' => self.linefeed(),
            '\r' => self.carriage_return(),
            '\x08' => self.backspace(),
            '\t' => self.tab(),
            _ if ch >= ' ' => {
                let row = self.cursor_row;
                let column = self.cursor_column;
                self.grid_mut().set_char(row, column, ch);
                self.mark_dirty(row);
                self.advance_column();
            }
            _ => {}
        }
    }

    pub fn linefeed(&mut self) {
        if self.cursor_row == self.scroll_bottom {
            let top = self.scroll_top;
            let bottom = self.scroll_bottom;
            if top == 0 && bottom + 1 == self.rows() {
                self.grid_mut().scroll_up();
            } else {
                self.grid_mut().scroll_up_region(top, bottom, 1);
            }
            self.mark_dirty_range(top, bottom);
        } else if self.cursor_row + 1 >= self.rows() {
            self.grid_mut().scroll_up();
            self.mark_all_dirty();
        } else {
            self.cursor_row += 1;
        }
    }

    pub fn carriage_return(&mut self) {
        self.cursor_column = 0;
    }

    pub fn newline(&mut self) {
        self.carriage_return();
        self.linefeed();
    }

    pub fn backspace(&mut self) {
        self.cursor_column = self.cursor_column.saturating_sub(1);
    }

    pub fn tab(&mut self) {
        let next_tab = ((self.cursor_column / 8) + 1) * 8;
        self.cursor_column = next_tab.min(self.columns().saturating_sub(1));
    }

    pub fn move_cursor_to(&mut self, row: u16, column: u16) {
        self.cursor_row = row.min(self.rows().saturating_sub(1));
        self.cursor_column = column.min(self.columns().saturating_sub(1));
    }

    pub fn move_cursor_up(&mut self, amount: u16) {
        self.cursor_row = self.cursor_row.saturating_sub(amount.max(1));
    }

    pub fn move_cursor_down(&mut self, amount: u16) {
        self.cursor_row = self
            .cursor_row
            .saturating_add(amount.max(1))
            .min(self.rows().saturating_sub(1));
    }

    pub fn move_cursor_right(&mut self, amount: u16) {
        self.cursor_column = self
            .cursor_column
            .saturating_add(amount.max(1))
            .min(self.columns().saturating_sub(1));
    }

    pub fn move_cursor_left(&mut self, amount: u16) {
        self.cursor_column = self.cursor_column.saturating_sub(amount.max(1));
    }

    pub fn clear_screen(&mut self) {
        self.grid_mut().clear_screen();
        self.mark_all_dirty();
        self.move_cursor_to(0, 0);
    }

    pub fn clear_to_end_of_screen(&mut self) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        let rows = self.rows();
        self.grid_mut().clear_line_right(cursor_row, cursor_column);
        self.mark_dirty(cursor_row);
        for row in cursor_row.saturating_add(1)..rows {
            self.grid_mut().clear_line(row);
            self.mark_dirty(row);
        }
    }

    pub fn clear_to_start_of_screen(&mut self) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        self.grid_mut().clear_line_left(cursor_row, cursor_column);
        self.mark_dirty(cursor_row);
        for row in 0..cursor_row {
            self.grid_mut().clear_line(row);
            self.mark_dirty(row);
        }
    }

    pub fn clear_line(&mut self) {
        let cursor_row = self.cursor_row;
        self.grid_mut().clear_line(cursor_row);
        self.mark_dirty(cursor_row);
    }

    pub fn clear_line_right(&mut self) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        self.grid_mut().clear_line_right(cursor_row, cursor_column);
        self.mark_dirty(cursor_row);
    }

    pub fn clear_line_left(&mut self) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        self.grid_mut().clear_line_left(cursor_row, cursor_column);
        self.mark_dirty(cursor_row);
    }

    pub fn erase_chars(&mut self, count: u16) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        self.grid_mut()
            .erase_chars(cursor_row, cursor_column, count.max(1));
        self.mark_dirty(cursor_row);
    }

    pub fn delete_chars(&mut self, count: u16) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        self.grid_mut()
            .delete_chars(cursor_row, cursor_column, count.max(1));
        self.mark_dirty(cursor_row);
    }

    pub fn insert_blank_chars(&mut self, count: u16) {
        let cursor_row = self.cursor_row;
        let cursor_column = self.cursor_column;
        self.grid_mut()
            .insert_blank_chars(cursor_row, cursor_column, count.max(1));
        self.mark_dirty(cursor_row);
    }

    pub fn insert_blank_lines(&mut self, count: u16) {
        let cursor_row = self.cursor_row;
        let bottom = self.scroll_bottom;
        self.grid_mut()
            .insert_blank_lines(cursor_row, bottom, count.max(1));
        self.mark_dirty_range(cursor_row, bottom);
    }

    pub fn delete_lines(&mut self, count: u16) {
        let cursor_row = self.cursor_row;
        let bottom = self.scroll_bottom;
        self.grid_mut()
            .delete_lines(cursor_row, bottom, count.max(1));
        self.mark_dirty_range(cursor_row, bottom);
    }

    pub fn scroll_up(&mut self, count: u16) {
        let top = self.scroll_top;
        let bottom = self.scroll_bottom;
        self.grid_mut().scroll_up_region(top, bottom, count.max(1));
        self.mark_dirty_range(top, bottom);
    }

    pub fn scroll_down(&mut self, count: u16) {
        let top = self.scroll_top;
        let bottom = self.scroll_bottom;
        self.grid_mut()
            .scroll_down_region(top, bottom, count.max(1));
        self.mark_dirty_range(top, bottom);
    }

    pub fn set_scroll_region(&mut self, top: u16, bottom: u16) {
        let max_bottom = self.rows().saturating_sub(1);
        let top = top.min(max_bottom);
        let bottom = bottom.min(max_bottom);
        if top < bottom {
            self.scroll_top = top;
            self.scroll_bottom = bottom;
        } else {
            self.reset_scroll_region();
        }
        self.move_cursor_to(0, 0);
    }

    pub fn reset_scroll_region(&mut self) {
        self.scroll_top = 0;
        self.scroll_bottom = self.rows().saturating_sub(1);
    }

    pub fn save_cursor(&mut self) {
        self.saved_cursor = Some((self.cursor_row, self.cursor_column));
    }

    pub fn restore_cursor(&mut self) {
        if let Some((row, column)) = self.saved_cursor {
            self.move_cursor_to(row, column);
        }
    }

    pub fn set_alternate_screen(&mut self, enabled: bool) {
        if self.use_alternate == enabled {
            return;
        }
        self.use_alternate = enabled;
        self.move_cursor_to(0, 0);
        if enabled {
            self.alternate.clear_screen();
        }
        self.mark_all_dirty();
    }

    pub fn resize(&mut self, columns: u16, rows: u16) {
        self.primary.resize(columns, rows);
        self.alternate.resize(columns, rows);
        self.reset_scroll_region();
        self.dirty_rows.resize(self.rows() as usize, true);
        self.mark_all_dirty();
        self.move_cursor_to(self.cursor_row, self.cursor_column);
    }

    fn advance_column(&mut self) {
        if self.cursor_column + 1 >= self.columns() {
            self.cursor_column = 0;
            self.linefeed();
        } else {
            self.cursor_column += 1;
        }
    }

    fn mark_dirty(&mut self, row: u16) {
        if let Some(dirty) = self.dirty_rows.get_mut(row as usize) {
            *dirty = true;
        }
    }

    fn mark_dirty_range(&mut self, top: u16, bottom: u16) {
        for row in top..=bottom {
            self.mark_dirty(row);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::Screen;

    #[test]
    fn writes_printable_text_and_wraps() {
        let mut screen = Screen::new(4, 2);

        for ch in "abcde".chars() {
            screen.write_char(ch);
        }

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "abcd");
        assert_eq!(screen.grid().line(1).unwrap().visible_text(), "e");
        assert_eq!(screen.cursor_row(), 1);
        assert_eq!(screen.cursor_column(), 1);
    }

    #[test]
    fn tracks_dirty_rows_for_writes_and_clears() {
        let mut screen = Screen::new(4, 3);
        screen.clear_dirty();

        screen.write_char('x');
        assert_eq!(screen.dirty_rows(), vec![0]);

        screen.clear_dirty();
        screen.move_cursor_to(1, 0);
        screen.clear_to_end_of_screen();
        assert_eq!(screen.dirty_rows(), vec![1, 2]);
    }

    #[test]
    fn tracks_bracketed_paste_mode() {
        let mut screen = Screen::new(4, 3);

        screen.set_bracketed_paste(true);
        assert!(screen.bracketed_paste());

        screen.set_bracketed_paste(false);
        assert!(!screen.bracketed_paste());
    }

    #[test]
    fn edits_characters_inside_line() {
        let mut screen = Screen::new(8, 1);
        for ch in "abcdef".chars() {
            screen.write_char(ch);
        }

        screen.move_cursor_to(0, 2);
        screen.delete_chars(2);
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "abef");

        screen.insert_blank_chars(2);
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "ab  ef");

        screen.erase_chars(4);
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "ab");
    }

    #[test]
    fn scrolls_inside_scroll_region() {
        let mut screen = Screen::new(6, 4);
        for ch in "one\r\ntwo\r\nthree\r\nfour".chars() {
            screen.write_char(ch);
        }

        screen.set_scroll_region(1, 2);
        screen.move_cursor_to(2, 0);
        screen.linefeed();

        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "one");
        assert_eq!(screen.grid().line(1).unwrap().visible_text(), "three");
        assert_eq!(screen.grid().line(2).unwrap().visible_text(), "");
        assert_eq!(screen.grid().line(3).unwrap().visible_text(), "four");
    }

    #[test]
    fn scrolls_primary_grid_into_scrollback() {
        let mut screen = Screen::new(5, 2);

        for ch in "one\r\ntwo\r\nthree".chars() {
            screen.write_char(ch);
        }

        assert!(screen.grid().scrollback_len() > 0);
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "three");
    }

    #[test]
    fn alternate_screen_does_not_destroy_primary_grid() {
        let mut screen = Screen::new(10, 2);

        for ch in "primary".chars() {
            screen.write_char(ch);
        }
        screen.set_alternate_screen(true);
        for ch in "alt".chars() {
            screen.write_char(ch);
        }
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "alt");

        screen.set_alternate_screen(false);
        assert_eq!(screen.grid().line(0).unwrap().visible_text(), "primary");
    }
}
