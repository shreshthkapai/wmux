use std::collections::BTreeMap;

use crate::{
    Cell, Color, CursorStyle, DamageOperation, Line, PaneId, Rect, Screen, ServerState, SessionId,
    Style, WindowId,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RenderCapabilities {
    pub scroll_region: bool,
}

impl Default for RenderCapabilities {
    fn default() -> Self {
        Self {
            scroll_region: true,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PaneSpan {
    pub pane: PaneId,
    pub rect: Rect,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PaneViewport {
    pub pane: PaneId,
    pub offset: usize,
    pub lines: Vec<Line>,
    pub cursor: Option<(u16, u16)>,
}

pub struct PaneSceneOverrides<'a> {
    pub previous_frame_panes: &'a [PaneId],
    pub retained_frames: &'a [RetainedPaneFrame],
    pub viewports: &'a [PaneViewport],
    pub previous: Option<&'a RenderState>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct BorderSpan {
    row: u16,
    col: u16,
    cells: Vec<char>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StructuralScene {
    window: WindowId,
    cols: u16,
    rows: u16,
    active_pane: PaneId,
    panes: Vec<PaneSpan>,
    borders: Vec<BorderSpan>,
}

impl StructuralScene {
    pub fn pane_ids(&self) -> impl Iterator<Item = PaneId> + '_ {
        self.panes.iter().map(|span| span.pane)
    }

    pub fn pane_at(&self, column: u16, row: u16) -> Option<(PaneId, u16, u16)> {
        self.panes.iter().find_map(|span| {
            let inside = column >= span.rect.x
                && column < span.rect.x.saturating_add(span.rect.cols)
                && row >= span.rect.y
                && row < span.rect.y.saturating_add(span.rect.rows);
            if inside {
                Some((span.pane, column - span.rect.x, row - span.rect.y))
            } else {
                None
            }
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RenderState {
    cols: u16,
    rows: u16,
    lines: Vec<Line>,
    cursor: (u16, u16),
    bracketed_paste: bool,
    cursor_visible: bool,
    cursor_style: CursorStyle,
    valid: bool,
}

impl RenderState {
    pub fn new(cols: u16, rows: u16) -> Self {
        Self {
            cols: cols.max(1),
            rows: rows.max(1),
            lines: vec![Line::blank(cols); rows.max(1) as usize],
            cursor: (0, 0),
            bracketed_paste: false,
            cursor_visible: true,
            cursor_style: CursorStyle::Default,
            valid: false,
        }
    }

    pub fn invalidate(&mut self) {
        self.valid = false;
    }

    fn matches_size(&self, cols: u16, rows: u16) -> bool {
        self.valid && self.cols == cols.max(1) && self.rows == rows.max(1)
    }

    fn sync_scene(&mut self, scene: &Scene) {
        self.cols = scene.cols;
        self.rows = scene.rows;
        self.lines = scene.lines.clone();
        self.cursor = scene.cursor;
        self.bracketed_paste = scene.bracketed_paste;
        self.cursor_visible = scene.cursor_visible;
        self.cursor_style = scene.cursor_style;
        self.valid = true;
    }
}

pub fn render_full(screen: &Screen, state: &mut RenderState) -> Vec<u8> {
    render_full_scene(&Scene::from_screen(screen), state)
}

pub fn render_diff(screen: &Screen, state: &mut RenderState) -> Vec<u8> {
    render_diff_scene(&Scene::from_screen(screen), state)
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Scene {
    cols: u16,
    rows: u16,
    lines: Vec<Line>,
    cursor: (u16, u16),
    bracketed_paste: bool,
    cursor_visible: bool,
    cursor_style: CursorStyle,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct RetainedPaneFrame {
    pub pane: PaneId,
    cols: u16,
    rows: u16,
    lines: Vec<Line>,
    cursor: (u16, u16),
    bracketed_paste: bool,
    cursor_visible: bool,
    cursor_style: CursorStyle,
}

impl RetainedPaneFrame {
    pub fn capture(pane: PaneId, screen: &Screen) -> Self {
        Self {
            pane,
            cols: screen.cols(),
            rows: screen.rows(),
            lines: visible_lines(screen),
            cursor: screen.render_cursor(),
            bracketed_paste: screen.bracketed_paste(),
            cursor_visible: screen.cursor_visible(),
            cursor_style: screen.cursor_style(),
        }
    }
}

impl Scene {
    fn from_screen(screen: &Screen) -> Self {
        Self {
            cols: screen.cols(),
            rows: screen.rows(),
            lines: visible_lines(screen),
            cursor: screen.render_cursor(),
            bracketed_paste: screen.bracketed_paste(),
            cursor_visible: screen.cursor_visible(),
            cursor_style: screen.cursor_style(),
        }
    }
}

pub fn build_window_scene(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
) -> Option<Scene> {
    build_window_scene_inner(
        state,
        session,
        cols,
        rows,
        PaneSceneOverrides {
            previous_frame_panes: &[],
            retained_frames: &[],
            viewports: &[],
            previous: None,
        },
        None,
    )
}

pub fn build_window_structure(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
) -> Option<StructuralScene> {
    let window_id = state.active_window_for_session(session)?;
    let window = state.window(window_id)?;
    let full = Rect::new(0, 0, cols.max(1), rows.max(1));
    let panes = if let Some(zoomed) = window.zoomed {
        vec![PaneSpan {
            pane: zoomed,
            rect: full,
        }]
    } else {
        window
            .layout
            .rects(full)
            .into_iter()
            .map(|(pane, rect)| PaneSpan { pane, rect })
            .collect()
    };
    let borders = if window.zoomed.is_none() && panes.len() > 1 {
        border_spans(window.layout.borders(full))
    } else {
        Vec::new()
    };
    Some(StructuralScene {
        window: window_id,
        cols: cols.max(1),
        rows: rows.max(1),
        active_pane: window.active_pane,
        panes,
        borders,
    })
}

fn border_spans(mut cells: Vec<(u16, u16, char)>) -> Vec<BorderSpan> {
    cells.sort_unstable_by_key(|(x, y, _)| (*y, *x));
    let mut spans: Vec<BorderSpan> = Vec::new();
    for (x, y, ch) in cells {
        if let Some(last) = spans.last_mut() {
            if last.row == y && last.col.saturating_add(last.cells.len() as u16) == x {
                last.cells.push(ch);
                continue;
            }
        }
        spans.push(BorderSpan {
            row: y,
            col: x,
            cells: vec![ch],
        });
    }
    spans
}

pub fn build_window_scene_with_retained_panes(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
    previous_frame_panes: &[PaneId],
    retained_frames: &[RetainedPaneFrame],
    previous: &RenderState,
) -> Option<Scene> {
    build_window_scene_with_viewports(
        state,
        session,
        cols,
        rows,
        PaneSceneOverrides {
            previous_frame_panes,
            retained_frames,
            viewports: &[],
            previous: Some(previous),
        },
    )
}

pub fn build_window_scene_with_viewports(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
    overrides: PaneSceneOverrides<'_>,
) -> Option<Scene> {
    build_window_scene_inner(state, session, cols, rows, overrides, None)
}

pub fn build_window_scene_with_client_overlay(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
    overrides: PaneSceneOverrides<'_>,
    confirmation_prompt: Option<&str>,
) -> Option<Scene> {
    build_window_scene_inner(state, session, cols, rows, overrides, confirmation_prompt)
}

fn build_window_scene_inner(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
    overrides: PaneSceneOverrides<'_>,
    confirmation_prompt: Option<&str>,
) -> Option<Scene> {
    let structure = build_window_structure(state, session, cols, rows)?;
    let mut lines = vec![Line::blank(cols); rows.max(1) as usize];

    let mut cursor = (0, 0);
    let mut bracketed_paste = false;
    let mut cursor_visible = true;
    let mut cursor_style = CursorStyle::Default;
    for PaneSpan {
        pane: pane_id,
        rect,
    } in structure.panes.iter().copied()
    {
        let Some(pane) = state.pane(pane_id) else {
            continue;
        };
        let retained = overrides
            .retained_frames
            .iter()
            .find(|frame| frame.pane == pane_id);
        let viewport = overrides
            .viewports
            .iter()
            .find(|viewport| viewport.pane == pane_id);
        if let Some(viewport) = viewport {
            draw_viewport(&mut lines, viewport, rect);
        } else if let Some(retained) = retained {
            draw_retained_frame(&mut lines, retained, rect);
        } else if overrides.previous_frame_panes.contains(&pane_id)
            && overrides
                .previous
                .is_some_and(|previous| previous.matches_size(cols, rows))
        {
            copy_previous_rect(&mut lines, rect, overrides.previous.expect("checked above"));
        } else {
            draw_pane(&mut lines, pane_id, rect, state);
        }
        if pane_id == structure.active_pane {
            let (row, col) = viewport
                .and_then(|viewport| viewport.cursor)
                .unwrap_or_else(|| {
                    retained.map_or_else(|| pane.screen.render_cursor(), |frame| frame.cursor)
                });
            let (row, col) = viewport_cursor(rect, row, col);
            cursor = (
                rect.y.saturating_add(row).min(rows.saturating_sub(1)),
                rect.x.saturating_add(col).min(cols.saturating_sub(1)),
            );
            bracketed_paste = retained.map_or_else(
                || pane.screen.bracketed_paste(),
                |frame| frame.bracketed_paste,
            );
            cursor_visible = retained.map_or_else(
                || pane.screen.cursor_visible(),
                |frame| frame.cursor_visible,
            );
            if let Some(viewport) = viewport {
                cursor_visible = viewport.cursor.is_some();
            }
            cursor_style =
                retained.map_or_else(|| pane.screen.cursor_style(), |frame| frame.cursor_style);
        }
    }

    for span in &structure.borders {
        for (offset, ch) in span.cells.iter().copied().enumerate() {
            put_scene_cell(
                &mut lines,
                span.col.saturating_add(offset as u16),
                span.row,
                Cell::printable(ch, 1, Style::default()),
            );
        }
    }

    if let Some(prompt) = confirmation_prompt {
        draw_confirmation_prompt(&mut lines, cols, prompt);
        cursor_visible = false;
    }

    Some(Scene {
        cols: cols.max(1),
        rows: rows.max(1),
        lines,
        cursor,
        bracketed_paste,
        cursor_visible,
        cursor_style,
    })
}

fn draw_confirmation_prompt(lines: &mut [Line], cols: u16, prompt: &str) {
    let Some(line) = lines.last_mut() else {
        return;
    };
    *line = Line::blank(cols);
    let mut column = 0_u16;
    for character in prompt.chars() {
        let width = crate::scalar_width(character).min(2);
        if width == 0 {
            continue;
        }
        let width = u16::from(width);
        if column.saturating_add(width) > cols.max(1) {
            break;
        }
        line.set(column, character, width as u8, Style::default());
        column += width;
    }
}

pub fn render_damage_from_structure(
    state: &ServerState,
    structure: &StructuralScene,
    consumed: &BTreeMap<PaneId, u64>,
    render_state: &mut RenderState,
    capabilities: RenderCapabilities,
) -> Option<Vec<u8>> {
    if !render_state.matches_size(structure.cols, structure.rows) {
        return None;
    }

    let mut out = Vec::new();
    let mut touched = false;
    let mut erase = Vec::new();
    for span in &structure.panes {
        let pane = state.pane(span.pane)?;
        if pane.screen.synchronized_output() {
            continue;
        }
        let consumed_generation = consumed.get(&span.pane).copied().unwrap_or(0);
        let status = pane.screen.damage_status_since(consumed_generation);
        if status.requires_full_redraw {
            return None;
        }
        if status.current_generation == consumed_generation {
            continue;
        }

        if capabilities.scroll_region
            && structure.panes.len() == 1
            && span.rect.x == 0
            && span.rect.cols == structure.cols
        {
            apply_scroll_operations(
                &pane.screen,
                span.rect,
                consumed_generation,
                render_state,
                &mut out,
            );
        }

        for source_row in 0..span.rect.rows.min(pane.screen.rows()) {
            if pane.screen.line_generation(source_row).unwrap_or(0) <= consumed_generation {
                continue;
            }
            let target_row = span.rect.y.saturating_add(source_row);
            let previous = render_state.lines.get(target_row as usize)?.clone();
            let wanted = compose_pane_row(&previous, &pane.screen, source_row, span.rect);
            if previous == wanted {
                continue;
            }
            if !touched {
                out.extend_from_slice(b"\x1b[?25l\x1b[0m");
                touched = true;
            }
            render_changed_span(target_row, &previous, &wanted, &mut out, &mut erase);
            render_state.lines[target_row as usize] = wanted;
        }
    }
    out.extend_from_slice(&erase);

    let active = state.pane(structure.active_pane)?;
    if active.screen.synchronized_output() {
        if !out.is_empty() {
            push_scene_cursor(
                render_state.cursor,
                Some(render_state.cursor_visible),
                None,
                &mut out,
            );
        }
        return Some(out);
    }
    let active_span = structure
        .panes
        .iter()
        .find(|span| span.pane == structure.active_pane)?;
    let (row, col) = active.screen.render_cursor();
    let (row, col) = viewport_cursor(active_span.rect, row, col);
    let cursor = (
        active_span
            .rect
            .y
            .saturating_add(row)
            .min(structure.rows - 1),
        active_span
            .rect
            .x
            .saturating_add(col)
            .min(structure.cols - 1),
    );
    let paste = active.screen.bracketed_paste();
    let visible = active.screen.cursor_visible();
    let cursor_style = active.screen.cursor_style();
    let visibility_changed = render_state.cursor_visible != visible;
    let style_changed = render_state.cursor_style != cursor_style;
    if render_state.bracketed_paste != paste {
        out.extend_from_slice(paste_mode(paste));
        render_state.bracketed_paste = paste;
    }
    if !out.is_empty() || render_state.cursor != cursor || visibility_changed || style_changed {
        push_scene_cursor(
            cursor,
            (touched || visibility_changed).then_some(visible),
            style_changed.then_some(cursor_style),
            &mut out,
        );
        render_state.cursor = cursor;
        render_state.cursor_visible = visible;
        render_state.cursor_style = cursor_style;
    }
    Some(out)
}

fn compose_pane_row(previous: &Line, screen: &Screen, source_row: u16, rect: Rect) -> Line {
    if rect.x == 0 && rect.cols == screen.cols() && previous.cols() == rect.cols {
        return screen
            .render_line(source_row)
            .cloned()
            .unwrap_or_else(|| Line::blank(rect.cols));
    }
    let mut wanted = previous.clone();
    let source = screen.render_line(source_row);
    for offset in 0..rect.cols {
        let cell = source
            .and_then(|line| line.cell(offset))
            .cloned()
            .unwrap_or_else(Cell::blank);
        wanted.replace_cell(
            rect.x.saturating_add(offset),
            clip_cell_to_width(cell, offset, rect.cols),
        );
    }
    wanted
}

fn apply_scroll_operations(
    screen: &Screen,
    rect: Rect,
    consumed_generation: u64,
    state: &mut RenderState,
    out: &mut Vec<u8>,
) {
    for batch in screen
        .damage_journal()
        .iter()
        .filter(|batch| batch.generation > consumed_generation)
    {
        for operation in &batch.operations {
            let DamageOperation::ScrollRegion { top, bottom, lines } = *operation else {
                continue;
            };
            if lines == 0 || top > bottom || bottom >= rect.rows {
                continue;
            }
            let target_top = rect.y.saturating_add(top);
            let target_bottom = rect.y.saturating_add(bottom);
            out.extend_from_slice(b"\x1b[");
            push_decimal(out, target_top + 1);
            out.push(b';');
            push_decimal(out, target_bottom + 1);
            out.extend_from_slice(b"r\x1b[");
            push_decimal(out, target_top + 1);
            out.extend_from_slice(b";1H");
            let count = lines.unsigned_abs();
            if lines > 0 {
                push_csi_number(out, count, b'S');
            } else {
                push_csi_number(out, count, b'T');
            }
            out.extend_from_slice(b"\x1b[r");
            sync_scrolled_baseline(state, target_top, target_bottom, lines);
        }
    }
}

fn sync_scrolled_baseline(state: &mut RenderState, top: u16, bottom: u16, lines: i32) {
    let range = top as usize..=bottom as usize;
    let count = lines.unsigned_abs() as usize;
    if count == 0 || count > range.clone().count() {
        return;
    }
    if lines > 0 {
        state.lines[range].rotate_left(count);
        for row in bottom as usize + 1 - count..=bottom as usize {
            state.lines[row] = Line::blank(state.cols);
        }
    } else {
        state.lines[range].rotate_right(count);
        for row in top as usize..top as usize + count {
            state.lines[row] = Line::blank(state.cols);
        }
    }
}

pub fn render_full_scene(scene: &Scene, state: &mut RenderState) -> Vec<u8> {
    render_full_scene_with_capabilities(scene, state, RenderCapabilities::default())
}

pub fn render_full_scene_with_capabilities(
    scene: &Scene,
    state: &mut RenderState,
    _capabilities: RenderCapabilities,
) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(b"\x1b[?25l\x1b[0m");
    out.extend_from_slice(paste_mode(scene.bracketed_paste));
    for row in 0..scene.rows {
        out.extend_from_slice(b"\x1b[0m\x1b[");
        push_decimal(&mut out, row + 1);
        out.extend_from_slice(b";1H");
        if let Some(line) = scene.lines.get(row as usize) {
            render_line_then_clear_tail(line, &mut out);
        } else {
            out.extend_from_slice(b"\x1b[2K");
        }
    }
    push_scene_cursor(
        scene.cursor,
        Some(scene.cursor_visible),
        Some(scene.cursor_style),
        &mut out,
    );
    state.sync_scene(scene);
    out
}

pub fn render_diff_scene(scene: &Scene, state: &mut RenderState) -> Vec<u8> {
    render_diff_scene_with_capabilities(scene, state, RenderCapabilities::default())
}

pub fn render_diff_scene_with_capabilities(
    scene: &Scene,
    state: &mut RenderState,
    capabilities: RenderCapabilities,
) -> Vec<u8> {
    if !state.valid || state.cols != scene.cols || state.rows != scene.rows {
        return render_full_scene_with_capabilities(scene, state, capabilities);
    }

    let mut out = Vec::new();
    if state.bracketed_paste != scene.bracketed_paste {
        out.extend_from_slice(paste_mode(scene.bracketed_paste));
        state.bracketed_paste = scene.bracketed_paste;
    }
    let mut touched = false;
    let mut erase = Vec::new();
    for row in 0..scene.rows {
        let Some(wanted) = scene.lines.get(row as usize) else {
            state.invalidate();
            return render_full_scene_with_capabilities(scene, state, capabilities);
        };
        let Some(previous) = state.lines.get_mut(row as usize) else {
            state.invalidate();
            return render_full_scene_with_capabilities(scene, state, capabilities);
        };
        if previous == wanted {
            continue;
        }
        if !touched {
            out.extend_from_slice(b"\x1b[?25l\x1b[0m");
            touched = true;
        }
        render_changed_span(row, previous, wanted, &mut out, &mut erase);
        *previous = wanted.clone();
    }
    out.extend_from_slice(&erase);

    let visibility_changed = state.cursor_visible != scene.cursor_visible;
    let style_changed = state.cursor_style != scene.cursor_style;
    if touched || state.cursor != scene.cursor || visibility_changed || style_changed {
        push_scene_cursor(
            scene.cursor,
            (touched || visibility_changed).then_some(scene.cursor_visible),
            style_changed.then_some(scene.cursor_style),
            &mut out,
        );
        state.cursor = scene.cursor;
        state.cursor_visible = scene.cursor_visible;
        state.cursor_style = scene.cursor_style;
    }
    out
}

fn visible_lines(screen: &Screen) -> Vec<Line> {
    (0..screen.rows())
        .map(|row| {
            screen
                .render_line(row)
                .cloned()
                .unwrap_or_else(|| Line::blank(screen.cols()))
        })
        .collect()
}

fn paste_mode(enabled: bool) -> &'static [u8] {
    if enabled {
        b"\x1b[?2004h"
    } else {
        b"\x1b[?2004l"
    }
}

fn cursor_visibility(visible: bool) -> &'static [u8] {
    if visible {
        b"\x1b[?25h"
    } else {
        b"\x1b[?25l"
    }
}

fn push_scene_cursor(
    cursor: (u16, u16),
    visibility: Option<bool>,
    style: Option<CursorStyle>,
    out: &mut Vec<u8>,
) {
    let (row, col) = cursor;
    if visibility == Some(false) {
        out.extend_from_slice(cursor_visibility(false));
    }
    out.extend_from_slice(b"\x1b[0m");
    push_cursor_position(out, row, col);
    if let Some(style) = style {
        out.extend_from_slice(b"\x1b[");
        push_decimal(out, style.decscusr());
        out.extend_from_slice(b" q");
    }
    if visibility == Some(true) {
        out.extend_from_slice(cursor_visibility(true));
    }
}

fn render_line_then_clear_tail(line: &Line, out: &mut Vec<u8>) {
    let end = line.stored_len();
    if end == 0 {
        out.extend_from_slice(b"\x1b[2K");
        return;
    }
    render_cells_exact(line, 0, end, out);
    out.extend_from_slice(b"\x1b[K");
}

fn render_changed_span(
    row: u16,
    previous: &Line,
    wanted: &Line,
    paint: &mut Vec<u8>,
    erase: &mut Vec<u8>,
) {
    let Some((start, end)) = changed_span(previous, wanted) else {
        return;
    };
    let mut index = start;
    while index < end {
        if wanted.cell(index as u16).is_none_or(Cell::is_blank_default) {
            let run = (index..end)
                .take_while(|offset| {
                    wanted
                        .cell(*offset as u16)
                        .is_none_or(Cell::is_blank_default)
                })
                .count();
            erase.extend_from_slice(b"\x1b[0m");
            push_cursor_position(erase, row, index as u16);
            if (index..wanted.cols() as usize).all(|offset| {
                wanted
                    .cell(offset as u16)
                    .is_none_or(Cell::is_blank_default)
            }) {
                erase.extend_from_slice(b"\x1b[K");
                break;
            }
            push_csi_number(erase, run, b'X');
            index += run;
            continue;
        }

        let run = (index..end)
            .take_while(|offset| {
                wanted
                    .cell(*offset as u16)
                    .is_some_and(|cell| !cell.is_blank_default())
            })
            .count();
        paint.extend_from_slice(b"\x1b[0m");
        push_cursor_position(paint, row, index as u16);
        render_cells_exact(wanted, index, index + run, paint);
        index += run;
    }
}

fn changed_span(previous: &Line, wanted: &Line) -> Option<(usize, usize)> {
    let len = usize::from(previous.cols().max(wanted.cols()));
    let mut start = None;
    let mut end = 0;
    for index in 0..len {
        let previous_cell = previous.cell(index as u16).unwrap_or(&DEFAULT_RENDER_CELL);
        let wanted_cell = wanted.cell(index as u16).unwrap_or(&DEFAULT_RENDER_CELL);
        if previous_cell != wanted_cell {
            start.get_or_insert(index);
            end = index + 1;
        }
    }

    let mut start = start?;
    start = widen_span_start_for_wide_cells(start, previous);
    start = widen_span_start_for_wide_cells(start, wanted);
    end = widen_span_end_for_wide_cells(end, previous);
    end = widen_span_end_for_wide_cells(end, wanted);
    Some((start, end.min(len)))
}

fn widen_span_start_for_wide_cells(mut start: usize, line: &Line) -> usize {
    while start > 0 && line.cell(start as u16).is_some_and(Cell::is_continuation) {
        start -= 1;
    }
    if start > 0
        && line
            .cell((start - 1) as u16)
            .is_some_and(|cell| cell.width() == 2)
    {
        start -= 1;
    }
    start
}

fn widen_span_end_for_wide_cells(mut end: usize, line: &Line) -> usize {
    if end < line.cols() as usize && line.cell(end as u16).is_some_and(Cell::is_continuation) {
        end += 1;
    }
    if end > 0
        && line
            .cell((end - 1) as u16)
            .is_some_and(|cell| cell.width() == 2)
    {
        end += 1;
    }
    end.min(line.cols() as usize)
}

fn render_cells_exact(line: &Line, start: usize, end: usize, out: &mut Vec<u8>) {
    let mut style = Style::default();
    let mut index = start;
    while index < end {
        let cell = line.cell(index as u16).unwrap_or(&DEFAULT_RENDER_CELL);
        if cell.is_continuation() {
            index += 1;
            continue;
        }
        if cell.is_blank_default() {
            let run = default_blank_run(line, index, end);
            if run >= 10 {
                out.extend_from_slice(b"\x1b[0m");
                style = Style::default();
                push_csi_number(out, run, b'X');
                if index + run < end {
                    push_csi_number(out, run, b'C');
                }
                index += run;
                continue;
            }
        }
        if cell.style() != style {
            push_style(cell.style(), out);
            style = cell.style();
        }
        if usize::from(cell.width()) > usize::from(line.cols()).saturating_sub(index) {
            out.push(b' ');
        } else if cell.style().hidden {
            out.extend(std::iter::repeat_n(b' ', usize::from(cell.width().max(1))));
        } else {
            cell.text().write_utf8(out);
        }
        index += 1;
    }
    if style != Style::default() {
        out.extend_from_slice(b"\x1b[0m");
    }
}

static DEFAULT_RENDER_CELL: Cell = Cell::const_blank_for_render();

fn default_blank_run(line: &Line, start: usize, end: usize) -> usize {
    (start..end)
        .take_while(|index| line.cell(*index as u16).is_none_or(Cell::is_blank_default))
        .count()
}

fn push_style(style: Style, out: &mut Vec<u8>) {
    out.extend_from_slice(b"\x1b[");
    out.push(b'0');
    push_style_flag(out, style.bold, 1);
    push_style_flag(out, style.dim, 2);
    push_style_flag(out, style.italic, 3);
    push_style_flag(out, style.underline, 4);
    push_style_flag(out, style.reverse, 7);
    push_style_flag(out, style.hidden, 8);
    push_style_flag(out, style.strikethrough, 9);
    push_color(style.fg, true, out);
    push_color(style.bg, false, out);
    out.push(b'm');
}

fn push_color(color: Color, foreground: bool, out: &mut Vec<u8>) {
    match color {
        Color::Indexed(index) if index < 8 => {
            out.push(b';');
            let code = if foreground { 30 + index } else { 40 + index };
            push_decimal(out, code);
        }
        Color::Indexed(index) if index < 16 => {
            out.push(b';');
            let code = if foreground {
                90 + index - 8
            } else {
                100 + index - 8
            };
            push_decimal(out, code);
        }
        Color::Indexed(index) => {
            out.extend_from_slice(if foreground { b";38;5;" } else { b";48;5;" });
            push_decimal(out, index);
        }
        Color::Rgb(red, green, blue) => {
            out.extend_from_slice(if foreground { b";38;2;" } else { b";48;2;" });
            push_decimal(out, red);
            out.push(b';');
            push_decimal(out, green);
            out.push(b';');
            push_decimal(out, blue);
        }
        Color::Default => {}
    }
}

fn push_style_flag(out: &mut Vec<u8>, enabled: bool, code: u8) {
    if enabled {
        out.push(b';');
        push_decimal(out, code);
    }
}

fn push_cursor_position(out: &mut Vec<u8>, row: u16, col: u16) {
    out.extend_from_slice(b"\x1b[");
    push_decimal(out, row + 1);
    out.push(b';');
    push_decimal(out, col + 1);
    out.push(b'H');
}

fn push_csi_number(out: &mut Vec<u8>, value: impl DecimalValue, final_byte: u8) {
    out.extend_from_slice(b"\x1b[");
    push_decimal(out, value);
    out.push(final_byte);
}

fn push_decimal(out: &mut Vec<u8>, value: impl DecimalValue) {
    let mut value = value.into_u64();
    let mut digits = [0_u8; 20];
    let mut start = digits.len();
    loop {
        start -= 1;
        digits[start] = b'0' + (value % 10) as u8;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    out.extend_from_slice(&digits[start..]);
}

trait DecimalValue {
    fn into_u64(self) -> u64;
}

macro_rules! decimal_values {
    ($($kind:ty),+ $(,)?) => {
        $(
            impl DecimalValue for $kind {
                fn into_u64(self) -> u64 {
                    self as u64
                }
            }
        )+
    };
}

decimal_values!(u8, u16, u32, u64, usize);

fn draw_pane(lines: &mut [Line], pane_id: PaneId, rect: Rect, state: &ServerState) {
    let Some(pane) = state.pane(pane_id) else {
        return;
    };
    draw_screen(lines, &pane.screen, rect);
}

fn draw_screen(lines: &mut [Line], screen: &Screen, rect: Rect) {
    let viewport = pane_viewport_lines(screen, rect.cols, rect.rows);
    for (row, source) in viewport.iter().enumerate() {
        for (offset, cell) in source.cells().iter().enumerate() {
            if offset >= rect.cols as usize {
                break;
            }
            put_scene_cell(
                lines,
                rect.x.saturating_add(offset as u16),
                rect.y.saturating_add(row as u16),
                clip_cell_to_width(cell.clone(), offset as u16, rect.cols),
            );
        }
    }
}

fn draw_retained_frame(lines: &mut [Line], frame: &RetainedPaneFrame, rect: Rect) {
    let row_count = usize::from(rect.rows.min(frame.rows));
    for (row, source) in frame.lines.iter().take(row_count).enumerate() {
        let col_count = rect.cols.min(frame.cols);
        for offset in 0..col_count {
            let Some(cell) = source.cell(offset) else {
                continue;
            };
            put_scene_cell(
                lines,
                rect.x.saturating_add(offset),
                rect.y.saturating_add(row as u16),
                clip_cell_to_width(cell.clone(), offset, rect.cols),
            );
        }
    }
}

fn draw_viewport(lines: &mut [Line], viewport: &PaneViewport, rect: Rect) {
    for (row, source) in viewport.lines.iter().take(rect.rows as usize).enumerate() {
        for offset in 0..rect.cols {
            let cell = source.cell(offset).cloned().unwrap_or_else(Cell::blank);
            put_scene_cell(
                lines,
                rect.x.saturating_add(offset),
                rect.y.saturating_add(row as u16),
                clip_cell_to_width(cell, offset, rect.cols),
            );
        }
    }
}

fn copy_previous_rect(lines: &mut [Line], rect: Rect, previous: &RenderState) {
    for y in rect.y..rect.y.saturating_add(rect.rows) {
        let Some(previous_line) = previous.lines.get(y as usize) else {
            continue;
        };
        for x in rect.x..rect.x.saturating_add(rect.cols) {
            let Some(cell) = previous_line.cell(x) else {
                continue;
            };
            if cell.is_blank_default() {
                continue;
            }
            put_scene_cell(
                lines,
                x,
                y,
                clip_cell_to_width(cell.clone(), x.saturating_sub(rect.x), rect.cols),
            );
        }
    }
}

fn pane_viewport_lines(screen: &Screen, cols: u16, rows: u16) -> Vec<Line> {
    let cols = cols.max(1);
    let rows = rows.max(1);
    absolute_viewport_lines(screen, cols, rows)
}

fn viewport_cursor(rect: Rect, row: u16, col: u16) -> (u16, u16) {
    let cols = rect.cols.max(1);
    (
        row.min(rect.rows.saturating_sub(1)),
        col.min(cols.saturating_sub(1)),
    )
}

fn absolute_viewport_lines(screen: &Screen, cols: u16, rows: u16) -> Vec<Line> {
    let mut out = Vec::with_capacity(rows as usize);
    for row in 0..rows {
        let mut line = screen
            .render_line(row)
            .cloned()
            .unwrap_or_else(|| Line::blank(cols));
        line.resize(cols);
        out.push(line);
    }
    out
}

fn put_scene_cell(lines: &mut [Line], x: u16, y: u16, cell: Cell) {
    let Some(line) = lines.get_mut(y as usize) else {
        return;
    };
    let Some(target) = line.cell(x) else {
        return;
    };
    let replacement = if !target.text().is_single_char(' ')
        && !cell.text().is_single_char(' ')
        && target.text() != cell.text()
    {
        Cell::printable('+', 1, Style::default())
    } else {
        cell
    };
    line.replace_cell(x, replacement);
}

fn clip_cell_to_width(cell: Cell, column: u16, cols: u16) -> Cell {
    if !cell.is_continuation()
        && usize::from(cell.width()) > usize::from(cols).saturating_sub(usize::from(column))
    {
        Cell::blank_with_style(cell.style())
    } else {
        cell
    }
}

#[cfg(test)]
mod tests {
    use super::{
        build_window_scene, build_window_scene_with_client_overlay,
        build_window_scene_with_retained_panes, build_window_scene_with_viewports,
        build_window_structure, draw_screen, render_damage_from_structure, render_diff,
        render_full_scene_with_capabilities, PaneSceneOverrides, PaneViewport, RenderCapabilities,
        RenderState, RetainedPaneFrame,
    };
    use crate::{Line, Rect, Screen, ServerState, SplitDirection, Style, TerminalEngine};
    use std::collections::BTreeMap;

    fn assert_host_matches_screen(host: &Screen, source: &Screen) {
        assert_eq!(host.cols(), source.cols());
        assert_eq!(host.rows(), source.rows());
        for row in 0..source.rows() {
            assert_eq!(
                host.render_line_cells(row),
                source.render_line_cells(row),
                "rendered host row {row} diverged"
            );
        }
        assert_eq!(host.cursor(), source.render_cursor());
        assert_eq!(host.cursor_visible(), source.cursor_visible());
        assert_eq!(host.cursor_style(), source.cursor_style());
        assert_eq!(host.bracketed_paste(), source.bracketed_paste());
    }

    fn line(text: &str, cols: u16) -> Line {
        let mut line = Line::blank(cols);
        for (column, ch) in text.chars().enumerate() {
            line.set(column as u16, ch, 1, Style::default());
        }
        line
    }

    #[test]
    fn first_render_is_full_then_diffs_changed_span() {
        let mut screen = Screen::new(12, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(12, 3);

        engine.feed(&mut screen, b"abc");
        let full = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(full.contains("\x1b[1;1Habc\x1b[K"));
        assert!(!full.contains("\x1b[1;1H\x1b[2Kabc"));

        engine.feed(&mut screen, b"d");
        let diff = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(diff.contains("\x1b[1;4Hd"));
        assert!(!diff.contains("\x1b[1;1H\x1b[2K"));
        assert!(!diff.contains("\x1b[2;1H\x1b[2K"));
    }

    #[test]
    fn full_and_diff_render_emit_complete_grapheme_bytes() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(8, 2);
        let mut state = RenderState::new(8, 2);

        engine.feed(&mut screen, "e\u{301}".as_bytes());
        let full = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(full.contains("e\u{301}"));

        engine.feed(&mut screen, b"\r");
        engine.feed(&mut screen, "a\u{301}".as_bytes());
        let diff = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(diff.contains("a\u{301}"));
    }

    #[test]
    fn renderer_masks_a_logical_wide_cell_that_cannot_fit() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(2, 2);
        let mut diff_state = RenderState::new(2, 2);
        let _ = render_diff(&screen, &mut diff_state);
        engine.feed(&mut screen, "a\u{1f1ec}\u{1f1e7}".as_bytes());

        let diff = String::from_utf8(render_diff(&screen, &mut diff_state)).unwrap();
        let mut full_state = RenderState::new(2, 2);
        let full = String::from_utf8(render_diff(&screen, &mut full_state)).unwrap();

        assert!(!diff.contains("\u{1f1ec}\u{1f1e7}"));
        assert!(!full.contains("\u{1f1ec}\u{1f1e7}"));
        assert_eq!(full_state.lines[0].text(), "a\u{1f1ec}\u{1f1e7}");

        screen.resize(3, 2);
        let grown = String::from_utf8(render_diff(&screen, &mut full_state)).unwrap();
        assert!(grown.contains("\u{1f1ec}\u{1f1e7}"));
    }

    #[test]
    fn renderer_restores_a_wide_grapheme_after_one_column_resize() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(2, 2);
        let mut state = RenderState::new(2, 2);
        engine.feed(&mut screen, "\u{1f469}\u{200d}\u{1f4bb}".as_bytes());
        let initial = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(initial.contains("\u{1f469}\u{200d}\u{1f4bb}"));

        screen.resize(1, 2);
        let narrow = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(!narrow.contains("\u{1f469}\u{200d}\u{1f4bb}"));
        assert_eq!(state.lines[0].text(), "\u{1f469}\u{200d}\u{1f4bb}");

        screen.resize(2, 2);
        let grown = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(grown.contains("\u{1f469}\u{200d}\u{1f4bb}"));
    }

    #[test]
    fn pane_compositor_clips_a_wide_cell_at_its_local_right_edge() {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(2, 1);
        engine.feed(&mut screen, "a\u{1f1ec}\u{1f1e7}".as_bytes());
        let mut lines = vec![Line::blank(4)];
        lines[0].set(2, '|', 1, Style::default());

        draw_screen(&mut lines, &screen, Rect::new(0, 0, 2, 1));

        assert_eq!(lines[0].cell(1).unwrap().width(), 1);
        assert_eq!(lines[0].cell(1).unwrap().ch(), ' ');
        assert_eq!(lines[0].cell(2).unwrap().ch(), '|');
        assert_eq!(lines[0].text(), "a |");
    }

    #[test]
    fn emitted_transactions_reconstruct_authoritative_tui_state() {
        let mut source = Screen::new(40, 8);
        let mut source_engine = TerminalEngine::new();
        let mut host = Screen::new(40, 8);
        let mut host_engine = TerminalEngine::new();
        let mut render_state = RenderState::new(40, 8);

        let updates: &[&[u8]] = &[
            b"\x1b[2J\x1b[Hcodex transcript\r\n\r\n> prompt\x1b[8;1H\x1b[2m gpt-5.6 high ~\x1b[0m\x1b[3;9H",
            b"\x1b[?2026h\x1b[?25l\x1b[3;1H\x1b[K> promp\x1b[8;1H\x1b[K\x1b[2m gpt-5.6 high ~\x1b[0m\x1b[3;8H\x1b[5 q\x1b[?25h\x1b[?2026l",
            b"\x1b[?25l\x1b[3;1H\x1b[K> prompt again\x1b[3;15H\x1b[6 q\x1b[?25h",
            b"\x1b[1;1H\x1b[Lnew line\x1b[4;1H\x1b[2PXY\x1b[4;3H",
            b"\x1b[?2004h\x1b[8;1H\x1b[Kstatus settled\x1b[4;3H",
        ];

        for update in updates {
            source_engine.feed(&mut source, update);
            let frame = render_diff(&source, &mut render_state);
            host_engine.feed(&mut host, &frame);
            assert_host_matches_screen(&host, &source);
        }
    }

    #[test]
    fn client_baseline_shares_immutable_scene_lines() {
        let mut screen = Screen::new(160, 44);
        let mut engine = TerminalEngine::new();
        engine.feed(&mut screen, b"shared snapshot");
        let scene = super::Scene::from_screen(&screen);
        let mut state = RenderState::new(160, 44);

        let _ = super::render_full_scene(&scene, &mut state);

        assert!(scene.lines[0].shares_cells_with(&state.lines[0]));
        assert_eq!(scene.lines[43].stored_len(), 0);
        assert!(scene.lines[43].shares_cells_with(&state.lines[43]));
    }

    #[test]
    fn ordinary_damage_renders_without_rebuilding_a_dense_scene() {
        let mut server = ServerState::new();
        let created = server.create_session("damage", 12, 3);
        let structure = build_window_structure(&server, created.session, 12, 3).unwrap();
        let scene = build_window_scene(&server, created.session, 12, 3).unwrap();
        let mut render_state = RenderState::new(12, 3);
        let capabilities = RenderCapabilities {
            scroll_region: true,
        };
        let _ = render_full_scene_with_capabilities(&scene, &mut render_state, capabilities);

        let pane = server.pane_mut(created.pane).unwrap();
        pane.terminal.feed(&mut pane.screen, b"abc");
        let output = render_damage_from_structure(
            &server,
            &structure,
            &BTreeMap::new(),
            &mut render_state,
            capabilities,
        )
        .unwrap();
        let output = String::from_utf8(output).unwrap();

        assert!(output.contains("\x1b[1;1Habc"));
        assert!(!output.contains("\x1b[2;1H\x1b[2K"));
        assert!(!output.contains("\x1b[?2026h"));
    }

    #[test]
    fn structural_scene_is_stable_until_layout_changes() {
        let mut server = ServerState::new();
        let created = server.create_session("structure", 80, 24);
        let first = build_window_structure(&server, created.session, 80, 24).unwrap();
        let same = build_window_structure(&server, created.session, 80, 24).unwrap();
        assert_eq!(first, same);

        server
            .split_pane(created.window, None, SplitDirection::LeftRight, 80, 24)
            .unwrap();
        let split = build_window_structure(&server, created.session, 80, 24).unwrap();
        assert_ne!(first, split);
        assert_eq!(split.pane_ids().count(), 2);
    }

    #[test]
    fn structural_scene_hit_tests_panes_but_not_borders() {
        let mut server = ServerState::new();
        let created = server.create_session("hit", 12, 4);
        server
            .split_pane(created.window, None, SplitDirection::LeftRight, 12, 4)
            .unwrap();
        let structure = build_window_structure(&server, created.session, 12, 4).unwrap();

        assert_eq!(structure.pane_at(1, 1).map(|hit| hit.0), Some(created.pane));
        assert_eq!(structure.pane_at(5, 1), None);
        assert!(structure.pane_at(8, 1).is_some());
    }

    #[test]
    fn pane_viewport_overrides_live_rows_and_hides_cursor() {
        let mut server = ServerState::new();
        let created = server.create_session("scroll", 10, 3);
        let pane = server.pane_mut(created.pane).unwrap();
        pane.terminal.feed(&mut pane.screen, b"live");
        let previous = RenderState::new(10, 3);
        let viewport = PaneViewport {
            pane: created.pane,
            offset: 2,
            lines: vec![
                line("old one", 10),
                line("old two", 10),
                line("old three", 10),
            ],
            cursor: None,
        };
        let scene = build_window_scene_with_viewports(
            &server,
            created.session,
            10,
            3,
            PaneSceneOverrides {
                previous_frame_panes: &[],
                retained_frames: &[],
                viewports: &[viewport],
                previous: Some(&previous),
            },
        )
        .unwrap();

        assert_eq!(scene.lines[0].text(), "old one");
        assert!(!scene.cursor_visible);
    }

    #[test]
    fn confirmation_overlay_clips_the_last_scene_line_without_mutating_the_pane() {
        let mut server = ServerState::new();
        let created = server.create_session("confirm", 8, 3);
        let pane = server.pane_mut(created.pane).unwrap();
        pane.terminal.feed(&mut pane.screen, b"pane data");
        let authoritative = pane.screen.render_line(2).unwrap().clone();
        let previous = RenderState::new(8, 3);

        let scene = build_window_scene_with_client_overlay(
            &server,
            created.session,
            8,
            3,
            PaneSceneOverrides {
                previous_frame_panes: &[],
                retained_frames: &[],
                viewports: &[],
                previous: Some(&previous),
            },
            Some("kill λ pane?"),
        )
        .unwrap();

        assert_eq!(scene.lines[2].text(), "kill λ p");
        assert!(!scene.cursor_visible);
        assert_eq!(
            server.pane(created.pane).unwrap().screen.render_line(2),
            Some(&authoritative)
        );
    }

    #[test]
    fn synchronized_pane_damage_is_published_only_after_commit() {
        let mut server = ServerState::new();
        let created = server.create_session("sync", 12, 3);
        let structure = build_window_structure(&server, created.session, 12, 3).unwrap();
        let scene = build_window_scene(&server, created.session, 12, 3).unwrap();
        let mut render_state = RenderState::new(12, 3);
        let _ = super::render_full_scene(&scene, &mut render_state);

        let pane = server.pane_mut(created.pane).unwrap();
        pane.terminal.feed(&mut pane.screen, b"\x1b[?2026hpartial");
        let held_generation = pane.generation();
        let held = render_damage_from_structure(
            &server,
            &structure,
            &BTreeMap::new(),
            &mut render_state,
            RenderCapabilities::default(),
        )
        .unwrap();
        assert!(held.is_empty());

        let pane = server.pane_mut(created.pane).unwrap();
        pane.terminal.feed(&mut pane.screen, b"\x1b[?2026l");
        let committed = render_damage_from_structure(
            &server,
            &structure,
            &BTreeMap::from([(created.pane, held_generation)]),
            &mut render_state,
            RenderCapabilities::default(),
        )
        .unwrap();
        assert!(String::from_utf8(committed).unwrap().contains("partial"));
    }

    #[test]
    fn diff_clears_to_end_for_blank_suffix_changes() {
        let mut screen = Screen::new(12, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(12, 3);

        engine.feed(&mut screen, b"abcdef");
        let _ = render_diff(&screen, &mut state);

        engine.feed(&mut screen, b"\r\x1b[K");
        let diff = String::from_utf8(render_diff(&screen, &mut state)).unwrap();

        assert!(diff.contains("\x1b[1;1H\x1b[K"));
        assert!(!diff.contains("      "));
    }

    #[test]
    fn cursor_visibility_and_shape_are_committed_after_footer_damage() {
        let mut screen = Screen::new(20, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(20, 3);

        engine.feed(&mut screen, b"gpt-5.6 high ~");
        let _ = render_diff(&screen, &mut state);

        engine.feed(&mut screen, b"\x1b[?25l");
        let hidden = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        assert!(hidden.contains("\x1b[?25l"));

        engine.feed(&mut screen, b"\x1b[1;14H\x1b[K\x1b[5 q\x1b[?25h");
        let frame = String::from_utf8(render_diff(&screen, &mut state)).unwrap();
        let erase = frame.find("\x1b[1;14H\x1b[K").unwrap();
        let shape = frame.find("\x1b[5 q").unwrap();
        let show = frame.find("\x1b[?25h").unwrap();

        assert!(erase < shape);
        assert!(shape < show);
        assert_eq!(frame[..erase].find("\x1b[?25h"), None);
    }

    #[test]
    fn cursor_only_motion_does_not_restart_visibility_or_shape() {
        let mut screen = Screen::new(20, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(20, 3);

        engine.feed(&mut screen, b"footer ~\x1b[5 q");
        let _ = render_diff(&screen, &mut state);
        engine.feed(&mut screen, b"\x1b[D");
        let frame = String::from_utf8(render_diff(&screen, &mut state)).unwrap();

        assert!(frame.contains("\x1b[1;8H"));
        assert!(!frame.contains("\x1b[?25"));
        assert!(!frame.contains(" q"));
    }

    #[test]
    fn direct_damage_finishes_with_authoritative_cursor_state() {
        let mut server = ServerState::new();
        let created = server.create_session("cursor", 20, 3);
        {
            let pane = server.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, b"footer ~");
        }
        let structure = build_window_structure(&server, created.session, 20, 3).unwrap();
        let scene = build_window_scene(&server, created.session, 20, 3).unwrap();
        let mut render_state = RenderState::new(20, 3);
        let capabilities = RenderCapabilities {
            scroll_region: true,
        };
        let _ = render_full_scene_with_capabilities(&scene, &mut render_state, capabilities);
        let consumed = server.pane(created.pane).unwrap().generation();

        {
            let pane = server.pane_mut(created.pane).unwrap();
            pane.terminal
                .feed(&mut pane.screen, b"\x1b[1;8H\x1b[K\x1b[6 q\x1b[?25h");
        }
        let frame = render_damage_from_structure(
            &server,
            &structure,
            &BTreeMap::from([(created.pane, consumed)]),
            &mut render_state,
            capabilities,
        )
        .unwrap();
        let frame = String::from_utf8(frame).unwrap();
        let erase = frame.find("\x1b[1;8H\x1b[K").unwrap();
        let shape = frame.find("\x1b[6 q").unwrap();
        let show = frame.rfind("\x1b[?25h").unwrap();

        assert!(frame.starts_with("\x1b[?25l"));
        assert!(erase < shape);
        assert!(shape < show);
    }

    #[test]
    fn diff_uses_erase_chars_for_large_default_blank_runs() {
        let mut screen = Screen::new(20, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(20, 3);

        engine.feed(&mut screen, b"aXXXXXXXXXXXXz");
        let _ = render_diff(&screen, &mut state);

        engine.feed(&mut screen, b"\x1b[1;2H            ");
        let diff = String::from_utf8(render_diff(&screen, &mut state)).unwrap();

        assert!(diff.contains("\x1b[0m\x1b[1;2H\x1b[12X"));
        assert!(!diff.contains("            "));
    }

    #[test]
    fn diff_paints_visible_cells_before_destructive_erases() {
        let mut screen = Screen::new(12, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(12, 3);

        engine.feed(&mut screen, b"oldold\r\noldold");
        let _ = render_diff(&screen, &mut state);

        engine.feed(&mut screen, b"\x1b[1;1Hnew\x1b[K\x1b[2;1Hnewer");
        let diff = String::from_utf8(render_diff(&screen, &mut state)).unwrap();

        let paint_first = diff.find("\x1b[1;1Hnew").unwrap();
        let paint_second = diff.find("\x1b[2;1Hnewer").unwrap();
        let clear_first = diff.find("\x1b[1;4H\x1b[K").unwrap();
        assert!(paint_first < clear_first);
        assert!(paint_second < clear_first);
    }

    #[test]
    fn render_preserves_sgr_styles() {
        let mut screen = Screen::new(12, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(12, 3);

        engine.feed(&mut screen, b"\x1b[1;31mred");
        let full = String::from_utf8(render_diff(&screen, &mut state)).unwrap();

        assert!(full.contains("\x1b[0;1;31mred\x1b[0m"));
    }

    #[test]
    fn full_render_draws_content_before_clearing_tail() {
        let mut screen = Screen::new(12, 3);
        let mut engine = TerminalEngine::new();
        let mut state = RenderState::new(12, 3);

        engine.feed(&mut screen, b"visible");
        let full = String::from_utf8(render_diff(&screen, &mut state)).unwrap();

        let draw = full.find("\x1b[1;1Hvisible").unwrap();
        let clear_tail = full[draw..].find("\x1b[K").unwrap();
        assert!(clear_tail > 0);
        assert!(!full.contains("\x1b[1;1H\x1b[2Kvisible"));
    }

    #[test]
    fn pane_scene_uses_resized_core_screen_after_split() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 12, 4);
        {
            let pane = state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, b"abcdefghijkl");
        }
        let _second = state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                12,
                4,
            )
            .unwrap();

        let scene = build_window_scene(&state, created.session, 12, 4).unwrap();
        let first = scene.lines[0].text();
        let second = scene.lines[1].text();

        assert!(first.starts_with("abcde|"));
        assert!(second.starts_with("fghij|"));
        assert_eq!(scene.lines[0].cell(5).map(|cell| cell.ch()), Some('|'));
    }

    #[test]
    fn retained_frame_uses_captured_lines_not_live_or_previous_client_pixels() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 12, 3);
        {
            let pane = state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, b"live");
        }
        let mut previous = RenderState::new(12, 3);
        let _ = render_diff(&Screen::new(12, 3), &mut previous);
        let mut retained_screen = Screen::new(12, 3);
        let mut engine = TerminalEngine::new();
        engine.feed(&mut retained_screen, b"retained");
        let retained = RetainedPaneFrame::capture(created.pane, &retained_screen);
        engine.feed(&mut retained_screen, b"\x1b[2Jtemporary blank");

        let scene = build_window_scene_with_retained_panes(
            &state,
            created.session,
            12,
            3,
            &[],
            &[retained],
            &previous,
        )
        .unwrap();
        let first = scene.lines[0].text();

        assert!(first.starts_with("retained"));
        assert!(!first.starts_with("live"));
        assert!(!first.starts_with("temporary blank"));
    }
}
