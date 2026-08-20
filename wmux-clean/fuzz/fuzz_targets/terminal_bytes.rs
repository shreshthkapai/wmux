#![no_main]

use libfuzzer_sys::fuzz_target;
use wmux_core::{render_full, RenderState, Screen, TerminalEngine};

const MAX_INPUT_BYTES: usize = 64 * 1024;
const MAX_HISTORY_ROWS: usize = 128;

fuzz_target!(|data: &[u8]| {
    if data.len() < 4 {
        return;
    }

    let cols = 1 + u16::from(data[0] % 120);
    let rows = 1 + u16::from(data[1] % 50);
    let max_chunk = 1 + usize::from(data[2] % 64);
    let resize_cols = 1 + u16::from(data[3] % 120);
    let resize_rows = 1 + u16::from(data[0].wrapping_add(data[3]) % 50);
    let payload_end = data.len().min(4 + MAX_INPUT_BYTES);
    let payload = &data[4..payload_end];

    let mut screen = Screen::new(cols, rows);
    screen.set_history_limit(MAX_HISTORY_ROWS);
    let mut terminal = TerminalEngine::new();
    let mut offset = 0;
    while offset < payload.len() {
        let selector = payload[offset];
        let chunk_len = 1 + usize::from(selector) % max_chunk;
        let end = offset.saturating_add(chunk_len).min(payload.len());
        terminal.feed(&mut screen, &payload[offset..end]);
        offset = end;
    }

    screen.resize(resize_cols, resize_rows);
    let _ = screen.copy_lines(resize_cols);
    let mut render_state = RenderState::new(resize_cols, resize_rows);
    std::hint::black_box(render_full(&screen, &mut render_state));
});
