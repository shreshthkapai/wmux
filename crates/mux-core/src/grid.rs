#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GridCell {
    pub ch: char,
}

impl GridCell {
    pub const fn blank() -> Self {
        Self { ch: ' ' }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct GridLine {
    cells: Vec<GridCell>,
}

impl GridLine {
    pub fn blank(columns: u16) -> Self {
        Self {
            cells: vec![GridCell::blank(); columns as usize],
        }
    }

    pub fn cells(&self) -> &[GridCell] {
        &self.cells
    }

    pub fn set_char(&mut self, column: u16, ch: char) {
        if let Some(cell) = self.cells.get_mut(column as usize) {
            cell.ch = ch;
        }
    }

    pub fn clear_range(&mut self, start: u16, end: u16) {
        let start = start.min(self.cells.len() as u16) as usize;
        let end = end.min(self.cells.len() as u16) as usize;
        for cell in &mut self.cells[start..end] {
            *cell = GridCell::blank();
        }
    }

    pub fn erase_chars(&mut self, column: u16, count: u16) {
        let start = column.min(self.cells.len() as u16) as usize;
        let end = column.saturating_add(count).min(self.cells.len() as u16) as usize;
        for cell in &mut self.cells[start..end] {
            *cell = GridCell::blank();
        }
    }

    pub fn delete_chars(&mut self, column: u16, count: u16) {
        let start = column.min(self.cells.len() as u16) as usize;
        let count = count as usize;
        if count == 0 || start >= self.cells.len() {
            return;
        }

        let len = self.cells.len();
        for index in start..len {
            let source = index.saturating_add(count);
            self.cells[index] = if source < len {
                self.cells[source].clone()
            } else {
                GridCell::blank()
            };
        }
    }

    pub fn insert_blank_chars(&mut self, column: u16, count: u16) {
        let start = column.min(self.cells.len() as u16) as usize;
        let count = count as usize;
        if count == 0 || start >= self.cells.len() {
            return;
        }

        let len = self.cells.len();
        for index in (start..len).rev() {
            self.cells[index] = if index >= start + count {
                self.cells[index - count].clone()
            } else {
                GridCell::blank()
            };
        }
    }

    pub fn resize(&mut self, columns: u16) {
        self.cells.resize(columns as usize, GridCell::blank());
    }

    pub fn visible_text(&self) -> String {
        let mut text = self.cells.iter().map(|cell| cell.ch).collect::<String>();
        while text.ends_with(' ') {
            text.pop();
        }
        text
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Grid {
    columns: u16,
    rows: u16,
    lines: Vec<GridLine>,
    scrollback: Vec<GridLine>,
    max_scrollback: usize,
}

impl Grid {
    pub fn new(columns: u16, rows: u16) -> Self {
        let columns = columns.max(1);
        let rows = rows.max(1);
        Self {
            columns,
            rows,
            lines: vec![GridLine::blank(columns); rows as usize],
            scrollback: Vec::new(),
            max_scrollback: 10_000,
        }
    }

    pub fn columns(&self) -> u16 {
        self.columns
    }

    pub fn rows(&self) -> u16 {
        self.rows
    }

    pub fn line(&self, row: u16) -> Option<&GridLine> {
        self.lines.get(row as usize)
    }

    pub fn set_char(&mut self, row: u16, column: u16, ch: char) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.set_char(column, ch);
        }
    }

    pub fn clear_screen(&mut self) {
        for line in &mut self.lines {
            line.clear_range(0, self.columns);
        }
    }

    pub fn clear_line(&mut self, row: u16) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.clear_range(0, self.columns);
        }
    }

    pub fn clear_line_right(&mut self, row: u16, column: u16) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.clear_range(column, self.columns);
        }
    }

    pub fn clear_line_left(&mut self, row: u16, column: u16) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.clear_range(0, column.saturating_add(1));
        }
    }

    pub fn erase_chars(&mut self, row: u16, column: u16, count: u16) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.erase_chars(column, count);
        }
    }

    pub fn delete_chars(&mut self, row: u16, column: u16, count: u16) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.delete_chars(column, count);
        }
    }

    pub fn insert_blank_chars(&mut self, row: u16, column: u16, count: u16) {
        if let Some(line) = self.lines.get_mut(row as usize) {
            line.insert_blank_chars(column, count);
        }
    }

    pub fn insert_blank_lines(&mut self, row: u16, bottom: u16, count: u16) {
        let row = row.min(self.rows.saturating_sub(1)) as usize;
        let bottom = bottom.min(self.rows.saturating_sub(1)) as usize;
        if row > bottom {
            return;
        }

        let count = (count as usize).min(bottom - row + 1);
        for _ in 0..count {
            self.lines.insert(row, GridLine::blank(self.columns));
            self.lines.remove(bottom + 1);
        }
    }

    pub fn delete_lines(&mut self, row: u16, bottom: u16, count: u16) {
        let row = row.min(self.rows.saturating_sub(1)) as usize;
        let bottom = bottom.min(self.rows.saturating_sub(1)) as usize;
        if row > bottom {
            return;
        }

        let count = (count as usize).min(bottom - row + 1);
        for _ in 0..count {
            self.lines.remove(row);
            self.lines.insert(bottom, GridLine::blank(self.columns));
        }
    }

    pub fn scroll_up_region(&mut self, top: u16, bottom: u16, count: u16) {
        self.delete_lines(top, bottom, count);
    }

    pub fn scroll_down_region(&mut self, top: u16, bottom: u16, count: u16) {
        self.insert_blank_lines(top, bottom, count);
    }

    pub fn scroll_up(&mut self) {
        if self.lines.is_empty() {
            return;
        }

        let removed = self.lines.remove(0);
        self.scrollback.push(removed);
        if self.scrollback.len() > self.max_scrollback {
            self.scrollback.remove(0);
        }
        self.lines.push(GridLine::blank(self.columns));
    }

    pub fn resize(&mut self, columns: u16, rows: u16) {
        let columns = columns.max(1);
        let rows = rows.max(1);

        for line in &mut self.lines {
            line.resize(columns);
        }

        match rows.cmp(&self.rows) {
            std::cmp::Ordering::Greater => {
                self.lines
                    .extend((0..rows - self.rows).map(|_| GridLine::blank(columns)));
            }
            std::cmp::Ordering::Less => {
                while self.lines.len() > rows as usize {
                    let removed = self.lines.remove(0);
                    self.scrollback.push(removed);
                }
                if self.scrollback.len() > self.max_scrollback {
                    let excess = self.scrollback.len() - self.max_scrollback;
                    self.scrollback.drain(0..excess);
                }
            }
            std::cmp::Ordering::Equal => {}
        }

        self.columns = columns;
        self.rows = rows;
    }

    pub fn scrollback_len(&self) -> usize {
        self.scrollback.len()
    }
}
