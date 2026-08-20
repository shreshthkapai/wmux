use crate::screen::Screen;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RenderState {
    columns: u16,
    rows: u16,
    lines: Vec<String>,
    cursor_row: u16,
    cursor_column: u16,
    bracketed_paste: bool,
    initialized: bool,
}

impl RenderState {
    pub fn new(columns: u16, rows: u16) -> Self {
        let columns = columns.max(1);
        let rows = rows.max(1);
        Self {
            columns,
            rows,
            lines: vec![String::new(); rows as usize],
            cursor_row: 0,
            cursor_column: 0,
            bracketed_paste: false,
            initialized: false,
        }
    }

    pub fn invalidate(&mut self) {
        self.initialized = false;
    }

    fn sync_from_screen(&mut self, screen: &Screen) {
        self.columns = screen.columns();
        self.rows = screen.rows();
        self.lines = visible_lines(screen);
        self.cursor_row = screen.cursor_row();
        self.cursor_column = screen.cursor_column();
        self.bracketed_paste = screen.bracketed_paste();
        self.initialized = true;
    }
}

pub fn render_full(screen: &Screen) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(b"\x1b[?25l");
    out.extend_from_slice(bracketed_paste_sequence(screen.bracketed_paste()));

    for row in 0..screen.rows() {
        out.extend_from_slice(format!("\x1b[{};1H\x1b[2K", row + 1).as_bytes());
        if let Some(line) = screen.grid().line(row) {
            out.extend_from_slice(line.visible_text().as_bytes());
        }
    }

    out.extend_from_slice(
        format!(
            "\x1b[{};{}H\x1b[?25h",
            screen.cursor_row() + 1,
            screen.cursor_column() + 1
        )
        .as_bytes(),
    );
    out
}

pub fn render_full_into_state(screen: &Screen, state: &mut RenderState) -> Vec<u8> {
    let bytes = render_full(screen);
    state.sync_from_screen(screen);
    bytes
}

pub fn render_diff(screen: &Screen, state: &mut RenderState, dirty_rows: &[u16]) -> Vec<u8> {
    if !state.initialized || state.columns != screen.columns() || state.rows != screen.rows() {
        return render_full_into_state(screen, state);
    }

    let mut out = Vec::new();
    let mut changed = false;

    if state.bracketed_paste != screen.bracketed_paste() {
        out.extend_from_slice(bracketed_paste_sequence(screen.bracketed_paste()));
        state.bracketed_paste = screen.bracketed_paste();
    }

    for row in dirty_rows.iter().copied() {
        if row >= screen.rows() {
            continue;
        }

        let desired = screen
            .grid()
            .line(row)
            .map(|line| line.visible_text())
            .unwrap_or_default();

        let Some(previous) = state.lines.get_mut(row as usize) else {
            state.invalidate();
            return render_full_into_state(screen, state);
        };

        if *previous == desired {
            continue;
        }

        if !changed {
            out.extend_from_slice(b"\x1b[?25l\x1b[0m");
            changed = true;
        }

        out.extend_from_slice(format!("\x1b[{};1H\x1b[2K", row + 1).as_bytes());
        out.extend_from_slice(desired.as_bytes());
        *previous = desired;
    }

    if changed
        || state.cursor_row != screen.cursor_row()
        || state.cursor_column != screen.cursor_column()
    {
        out.extend_from_slice(
            format!(
                "\x1b[{};{}H\x1b[?25h",
                screen.cursor_row() + 1,
                screen.cursor_column() + 1
            )
            .as_bytes(),
        );
        state.cursor_row = screen.cursor_row();
        state.cursor_column = screen.cursor_column();
    }

    out
}

fn bracketed_paste_sequence(enabled: bool) -> &'static [u8] {
    if enabled {
        b"\x1b[?2004h"
    } else {
        b"\x1b[?2004l"
    }
}

fn visible_lines(screen: &Screen) -> Vec<String> {
    (0..screen.rows())
        .map(|row| {
            screen
                .grid()
                .line(row)
                .map(|line| line.visible_text())
                .unwrap_or_default()
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{render_diff, render_full, RenderState};
    use crate::screen::Screen;

    #[test]
    fn renders_screen_lines_and_cursor_position() {
        let mut screen = Screen::new(10, 2);
        for ch in "hello".chars() {
            screen.write_char(ch);
        }

        let rendered = String::from_utf8(render_full(&screen)).unwrap();

        assert!(rendered.contains("\x1b[1;1H\x1b[2Khello"));
        assert!(rendered.ends_with("\x1b[1;6H\x1b[?25h"));
    }

    #[test]
    fn first_diff_render_is_full_render() {
        let mut screen = Screen::new(10, 2);
        for ch in "hello".chars() {
            screen.write_char(ch);
        }
        let mut state = RenderState::new(10, 2);

        let rendered = String::from_utf8(render_diff(&screen, &mut state, &[0])).unwrap();

        assert!(rendered.contains("\x1b[1;1H\x1b[2Khello"));
        assert!(rendered.contains("\x1b[2;1H\x1b[2K"));
    }

    #[test]
    fn diff_render_only_emits_changed_dirty_lines() {
        let mut screen = Screen::new(10, 3);
        for ch in "alpha".chars() {
            screen.write_char(ch);
        }
        let mut state = RenderState::new(10, 3);
        let _ = render_diff(&screen, &mut state, &[0, 1, 2]);

        screen.clear_dirty();
        screen.move_cursor_to(1, 0);
        for ch in "beta".chars() {
            screen.write_char(ch);
        }

        let rendered =
            String::from_utf8(render_diff(&screen, &mut state, &screen.dirty_rows())).unwrap();

        assert!(!rendered.contains("\x1b[1;1H\x1b[2Kalpha"));
        assert!(rendered.contains("\x1b[2;1H\x1b[2Kbeta"));
        assert!(rendered.ends_with("\x1b[2;5H\x1b[?25h"));
    }

    #[test]
    fn diff_render_can_emit_cursor_only_update() {
        let mut screen = Screen::new(10, 2);
        let mut state = RenderState::new(10, 2);
        let _ = render_diff(&screen, &mut state, &[0, 1]);

        screen.clear_dirty();
        screen.move_cursor_to(1, 2);

        let rendered =
            String::from_utf8(render_diff(&screen, &mut state, &screen.dirty_rows())).unwrap();

        assert_eq!(rendered, "\x1b[2;3H\x1b[?25h");
    }

    #[test]
    fn diff_render_emits_bracketed_paste_mode_changes() {
        let mut screen = Screen::new(10, 2);
        let mut state = RenderState::new(10, 2);
        let _ = render_diff(&screen, &mut state, &[0, 1]);

        screen.clear_dirty();
        screen.set_bracketed_paste(true);

        let rendered =
            String::from_utf8(render_diff(&screen, &mut state, &screen.dirty_rows())).unwrap();

        assert_eq!(rendered, "\x1b[?2004h");
    }
}
