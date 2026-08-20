use std::{fmt::Write as _, io::Cursor};

use wmux_core::{
    build_window_scene, render_full_scene, ClientInput, Color, RenderState, Screen, ServerState,
    Style, TerminalEngine,
};
use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};
use wmux_protocol::{encode_frame, read_message, Message};

const COLS: u16 = 80;
const ROWS: u16 = 24;
pub const EXPECTED_PORTABLE_FINGERPRINT: u64 = 0x0340_7628_c53b_7958;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CaseResult {
    pub name: &'static str,
    pub fingerprint: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ConformanceReport {
    pub cases: Vec<CaseResult>,
    pub suite_fingerprint: u64,
}

pub fn run_portable_suite() -> Result<ConformanceReport, String> {
    let cases = vec![
        vt_replay_case(),
        detach_reattach_case()?,
        multiple_clients_case()?,
        resize_reflow_case(),
        malformed_input_case()?,
        key_and_paste_case(),
        mouse_routing_case(),
    ];
    let mut suite_fingerprint = Fnv64::new();
    for case in &cases {
        suite_fingerprint.bytes(case.name.as_bytes());
        suite_fingerprint.u64(case.fingerprint);
    }
    Ok(ConformanceReport {
        cases,
        suite_fingerprint: suite_fingerprint.finish(),
    })
}

pub fn verify_portable_suite() -> Result<ConformanceReport, String> {
    let report = run_portable_suite()?;
    if report.suite_fingerprint != EXPECTED_PORTABLE_FINGERPRINT {
        return Err(format!(
            "portable fingerprint changed: expected {EXPECTED_PORTABLE_FINGERPRINT:016x}, got {:016x}",
            report.suite_fingerprint
        ));
    }
    Ok(report)
}

fn vt_replay_case() -> CaseResult {
    let fixture = concat!(
        "\x1b[2J\x1b[H",
        "wmux \x1b[1;38;5;2mportable\x1b[0m terminal\r\n",
        "wide: \u{754c} emoji: \u{1f680}\r\n",
        "\x1b[4;7Hpositioned",
        "\x1b[6;1Habcdef\x1b[3D\x1b[2PXY",
        "\x1b[8;1H\x1b[4munderline\x1b[0m",
        "\x1b[?2004h\x1b[?25l\x1b[5 q\x1b[?25h"
    )
    .as_bytes();
    let mut screen = Screen::new(COLS, ROWS);
    let mut terminal = TerminalEngine::new();
    for chunk in fixture.chunks(7) {
        terminal.feed(&mut screen, chunk);
    }
    CaseResult {
        name: "vt-replay-grid",
        fingerprint: hash_screen(&screen),
    }
}

fn detach_reattach_case() -> Result<CaseResult, String> {
    let mut state = ServerState::new();
    let created = state.create_session("persistent", COLS, ROWS);
    let first = state.add_client();
    state
        .attach_client(first, created.session)
        .ok_or("first attach failed")?;
    {
        let pane = state.pane_mut(created.pane).ok_or("pane missing")?;
        pane.terminal
            .feed(&mut pane.screen, b"before detach\r\nstill running");
    }
    let before = hash_screen(&state.pane(created.pane).ok_or("pane missing")?.screen);
    state.detach_client(first);
    state.remove_client(first);
    if !state.sessions.contains_key(&created.session) || !state.panes.contains_key(&created.pane) {
        return Err("detaching destroyed persistent state".to_string());
    }
    let second = state.add_client();
    state
        .attach_client(second, created.session)
        .ok_or("reattach failed")?;
    let after = hash_screen(&state.pane(created.pane).ok_or("pane missing")?.screen);
    if before != after {
        return Err("reattach changed the authoritative pane grid".to_string());
    }
    Ok(CaseResult {
        name: "detach-reattach-persistence",
        fingerprint: after,
    })
}

fn multiple_clients_case() -> Result<CaseResult, String> {
    let mut state = ServerState::new();
    let created = state.create_session("shared", COLS, ROWS);
    let first = state.add_client();
    let second = state.add_client();
    state
        .attach_client(first, created.session)
        .ok_or("first client attach failed")?;
    state
        .attach_client(second, created.session)
        .ok_or("second client attach failed")?;
    {
        let pane = state.pane_mut(created.pane).ok_or("pane missing")?;
        pane.terminal.feed(
            &mut pane.screen,
            b"\x1b[2J\x1b[Hshared state\r\n\x1b[38;5;6mclient view\x1b[0m",
        );
    }
    let scene = build_window_scene(&state, created.session, COLS, ROWS)
        .ok_or("failed to compose shared scene")?;
    let first_frame = render_full_scene(&scene, &mut RenderState::new(COLS, ROWS));
    let second_frame = render_full_scene(&scene, &mut RenderState::new(COLS, ROWS));
    if first_frame != second_frame {
        return Err("clients rendered different frames from the same scene".to_string());
    }
    Ok(CaseResult {
        name: "multiple-client-consistency",
        fingerprint: hash_bytes(&first_frame),
    })
}

fn resize_reflow_case() -> CaseResult {
    let mut screen = Screen::new(32, 8);
    screen.set_history_limit(256);
    let mut terminal = TerminalEngine::new();
    for line in 0..120 {
        let mut text = String::new();
        write!(
            &mut text,
            "logical-line-{line:03}-abcdefghijklmnopqrstuvwxyz\r\n"
        )
        .expect("string write");
        terminal.feed(&mut screen, text.as_bytes());
    }
    screen.resize(17, 8);
    let narrow = hash_screen(&screen);
    screen.resize(47, 12);
    let wide = hash_screen(&screen);
    let mut hash = Fnv64::new();
    hash.u64(narrow);
    hash.u64(wide);
    hash.usize(screen.grid().history_len());
    CaseResult {
        name: "resize-reflow",
        fingerprint: hash.finish(),
    }
}

fn malformed_input_case() -> Result<CaseResult, String> {
    let mut screen = Screen::new(COLS, ROWS);
    let mut terminal = TerminalEngine::new();
    for malformed in [
        b"\xff\xfe\xf0\x28\x8c\x28".as_slice(),
        b"\x1b[999999999999999999999999;?;m".as_slice(),
        b"\x1b]unterminated title".as_slice(),
        b"\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18m".as_slice(),
    ] {
        terminal.feed(&mut screen, malformed);
    }

    let malformed_frames = [
        b"NOPE\x01\x00\x00\x00x".to_vec(),
        b"WMX5\xff\xff\xff\xff\xff".to_vec(),
    ];
    for frame in malformed_frames {
        if read_message(Cursor::new(frame)).is_ok() {
            return Err("malformed IPC frame was accepted".to_string());
        }
    }
    Ok(CaseResult {
        name: "malformed-input-resilience",
        fingerprint: hash_screen(&screen),
    })
}

fn mouse_routing_case() -> CaseResult {
    let mut screen = Screen::new(COLS, ROWS);
    let mut terminal = TerminalEngine::new();
    terminal.feed(&mut screen, b"\x1b[?1002h\x1b[?1006h");
    let event = MouseEvent {
        kind: MouseEventKind::Drag,
        button: MouseButton::Left,
        modifiers: MouseModifiers::new(MouseModifiers::ALT),
        column: 12,
        row: 8,
    };
    let application = screen
        .encode_mouse(event, event.column, event.row)
        .expect("button tracking accepts drag");
    assert_eq!(application, b"\x1b[<40;13;9M");

    let message = Message::Mouse(event);
    let frame = encode_frame(&message);
    assert_eq!(read_message(Cursor::new(&frame)).unwrap(), Some(message));
    let mut hash = Fnv64::new();
    hash.bytes(&application);
    hash.bytes(&frame);
    CaseResult {
        name: "mouse-mode-routing",
        fingerprint: hash.finish(),
    }
}

fn key_and_paste_case() -> CaseResult {
    let key = ClientInput::Bytes(b"\x1b[1;5D".to_vec()).into_pty_bytes(false);
    let plain = ClientInput::Paste(b"alpha\r\nbeta".to_vec()).into_pty_bytes(false);
    let bracketed = ClientInput::Paste(b"alpha\r\nbeta".to_vec()).into_pty_bytes(true);
    assert_eq!(key, b"\x1b[1;5D");
    assert_eq!(plain, b"alpha\r\nbeta");
    assert_eq!(bracketed, b"\x1b[200~alpha\r\nbeta\x1b[201~");

    let messages = [
        Message::Key(key),
        Message::Paste(plain),
        Message::Paste(bracketed),
    ];
    let mut hash = Fnv64::new();
    for message in messages {
        let frame = encode_frame(&message);
        let decoded = read_message(Cursor::new(&frame))
            .expect("valid input frame")
            .expect("one input frame");
        assert_eq!(decoded, message);
        hash.bytes(&frame);
    }
    CaseResult {
        name: "key-paste-behavior",
        fingerprint: hash.finish(),
    }
}

fn hash_screen(screen: &Screen) -> u64 {
    let mut hash = Fnv64::new();
    hash.u16(screen.cols());
    hash.u16(screen.rows());
    hash.u16(screen.cursor().0);
    hash.u16(screen.cursor().1);
    hash.usize(screen.grid().history_len());
    hash.byte(u8::from(screen.bracketed_paste()));
    hash.byte(u8::from(screen.cursor_visible()));
    hash.byte(screen.cursor_style().decscusr());
    hash.byte(u8::from(screen.alternate_active()));
    for row in 0..screen.rows() {
        for cell in screen
            .render_line_cells(row)
            .expect("visible screen row exists")
        {
            let mut text = Vec::with_capacity(cell.text().byte_len());
            cell.text().write_utf8(&mut text);
            hash.byte(text.len() as u8);
            hash.bytes(&text);
            hash.byte(cell.width());
            hash.byte(u8::from(cell.is_continuation()));
            hash_style(&mut hash, cell.style());
        }
    }
    hash.finish()
}

fn hash_style(hash: &mut Fnv64, style: Style) {
    hash_color(hash, style.fg);
    hash_color(hash, style.bg);
    for flag in [
        style.bold,
        style.dim,
        style.italic,
        style.underline,
        style.reverse,
        style.hidden,
        style.strikethrough,
    ] {
        hash.byte(u8::from(flag));
    }
}

fn hash_color(hash: &mut Fnv64, color: Color) {
    match color {
        Color::Default => hash.byte(0),
        Color::Indexed(index) => {
            hash.byte(1);
            hash.byte(index);
        }
        Color::Rgb(red, green, blue) => {
            hash.byte(2);
            hash.byte(red);
            hash.byte(green);
            hash.byte(blue);
        }
    }
}

fn hash_bytes(bytes: &[u8]) -> u64 {
    let mut hash = Fnv64::new();
    hash.bytes(bytes);
    hash.finish()
}

struct Fnv64(u64);

impl Fnv64 {
    const fn new() -> Self {
        Self(0xcbf29ce484222325)
    }

    fn byte(&mut self, byte: u8) {
        self.0 ^= u64::from(byte);
        self.0 = self.0.wrapping_mul(0x100000001b3);
    }

    fn bytes(&mut self, bytes: &[u8]) {
        for byte in bytes {
            self.byte(*byte);
        }
    }

    fn u16(&mut self, value: u16) {
        self.bytes(&value.to_le_bytes());
    }

    fn u64(&mut self, value: u64) {
        self.bytes(&value.to_le_bytes());
    }

    fn usize(&mut self, value: usize) {
        self.u64(value as u64);
    }

    const fn finish(self) -> u64 {
        self.0
    }
}

#[cfg(test)]
mod tests {
    use super::{
        hash_screen, run_portable_suite, verify_portable_suite, EXPECTED_PORTABLE_FINGERPRINT,
    };
    use wmux_core::{Screen, TerminalEngine};

    #[test]
    fn portable_semantic_suite_passes() {
        let report = verify_portable_suite().unwrap();
        assert_eq!(report.cases.len(), 7);
        assert_eq!(report.suite_fingerprint, EXPECTED_PORTABLE_FINGERPRINT);
    }

    #[test]
    fn portable_semantic_suite_is_deterministic() {
        assert_eq!(run_portable_suite().unwrap(), run_portable_suite().unwrap());
    }

    #[test]
    fn screen_hash_includes_every_byte_of_combined_cell_text() {
        let mut scalar = Screen::new(8, 2);
        let mut combined = Screen::new(8, 2);
        let mut scalar_terminal = TerminalEngine::new();
        let mut combined_terminal = TerminalEngine::new();
        scalar_terminal.feed(&mut scalar, b"e");
        combined_terminal.feed(&mut combined, "e\u{301}".as_bytes());

        assert_ne!(hash_screen(&scalar), hash_screen(&combined));
    }
}
