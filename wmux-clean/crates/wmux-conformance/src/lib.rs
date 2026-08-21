use std::{fmt::Write as _, io::Cursor};

use wmux_core::{
    build_window_scene, execute, parse_command_text, render_full_scene, route_key, ClientInput,
    Color, Command, CommandEffect, CommandOutcome, CommandQueue, CommandSource, ConfirmationState,
    InputMode, InputRoute, KeyCode, KeyEvent, KeyModifiers, KeyTableName, QueuedCommand,
    RenderState, ResolveContext, Screen, ServerState, Style, TargetKind, TargetResolver,
    TargetSpec, TerminalEngine,
};
use wmux_platform::{MouseButton, MouseEvent, MouseEventKind, MouseModifiers};
use wmux_protocol::{
    encode_frame, read_message, Message, WireKeyCode, WireKeyEvent, WireKeyModifiers,
};

const COLS: u16 = 80;
const ROWS: u16 = 24;
pub const EXPECTED_PORTABLE_FINGERPRINT: u64 = 0x00b7_63c7_26b9_d162;

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
        semantic_key_routing_case()?,
        command_target_binding_case()?,
        command_effects_case()?,
        mouse_routing_case(),
        unicode_terminal_text_case()?,
        terminal_modes_and_title_case()?,
        bounded_control_recovery_case()?,
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

fn execute_text(
    state: &mut ServerState,
    client: wmux_core::ClientId,
    raw: &str,
) -> Result<Vec<CommandOutcome>, String> {
    let commands = parse_command_text(raw).map_err(|error| error.to_string())?;
    let command_count = commands.len();
    let mut outcomes = Vec::with_capacity(command_count);
    for (index, command) in commands.iter().cloned().enumerate() {
        let outcome = execute(
            state,
            QueuedCommand {
                invocation: 1,
                sequence: index as u64 + 1,
                client,
                command,
                source: CommandSource::ClientRequest,
                final_in_list: index + 1 == command_count,
            },
        );
        if !outcome.ok {
            return Err(outcome.message);
        }
        outcomes.push(outcome);
    }
    Ok(outcomes)
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
        b"WMX6\xff\xff\xff\xff\xff".to_vec(),
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
        Message::Key(WireKeyEvent {
            code: WireKeyCode::Left,
            modifiers: WireKeyModifiers::CONTROL,
            raw: key,
        }),
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

fn semantic_key_routing_case() -> Result<CaseResult, String> {
    let mut state = ServerState::new();
    let first = state.add_client();
    let second = state.add_client();
    let prefix = || KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]);

    if route_key(&mut state, first, InputMode::Normal, prefix(), 0) != InputRoute::Consumed {
        return Err("prefix key was not consumed".to_string());
    }
    if state.clients[&second].key_table != KeyTableName::ROOT {
        return Err("one client's prefix changed another client".to_string());
    }
    let second_passthrough = match route_key(
        &mut state,
        second,
        InputMode::Normal,
        KeyEvent::new(KeyCode::character('c', KeyModifiers::NONE), b"c".to_vec()),
        1,
    ) {
        InputRoute::PaneBytes(bytes) => bytes,
        _ => return Err("unprefixed key did not pass through".to_string()),
    };
    let first_commands = match route_key(
        &mut state,
        first,
        InputMode::Normal,
        KeyEvent::new(KeyCode::character('c', KeyModifiers::NONE), b"c".to_vec()),
        2,
    ) {
        InputRoute::Commands(commands) => commands,
        _ => return Err("prefixed key did not resolve its binding".to_string()),
    };

    if route_key(&mut state, second, InputMode::Normal, prefix(), 3) != InputRoute::Consumed {
        return Err("second prefix key was not consumed".to_string());
    }
    let second_commands = match route_key(
        &mut state,
        second,
        InputMode::Normal,
        KeyEvent::new(KeyCode::character('n', KeyModifiers::NONE), b"n".to_vec()),
        4,
    ) {
        InputRoute::Commands(commands) => commands,
        _ => return Err("second binding did not resolve".to_string()),
    };

    let unicode = match route_key(
        &mut state,
        first,
        InputMode::Normal,
        KeyEvent::new(
            KeyCode::character('λ', KeyModifiers::NONE),
            "λ".as_bytes().to_vec(),
        ),
        5,
    ) {
        InputRoute::PaneBytes(bytes) => bytes,
        _ => return Err("unbound Unicode key did not pass through".to_string()),
    };
    let paste = ClientInput::Paste(b"paste".to_vec()).into_pty_bytes(true);

    let mut queue = CommandQueue::default();
    queue
        .push_list(first, first_commands, CommandSource::KeyBinding)
        .map_err(|error| error.to_string())?;
    queue
        .push_list(second, second_commands, CommandSource::KeyBinding)
        .map_err(|error| error.to_string())?;
    let first_queued = queue.pop().ok_or("first binding was not queued")?;
    if first_queued.client != first || !matches!(first_queued.command, Command::NewWindow { .. }) {
        return Err("first binding queue order changed".to_string());
    }
    let first_sequence = first_queued.sequence;
    let first_completion = queue
        .finish(first_queued, Ok(String::new()))
        .ok_or("first binding did not complete")?;
    if first_completion.requires_reply() {
        return Err("binding completion unexpectedly requires a reply".to_string());
    }
    let second_queued = queue.pop().ok_or("second binding was not queued")?;
    if second_queued.client != second
        || !matches!(second_queued.command, Command::SelectWindow { .. })
        || second_queued.sequence <= first_sequence
    {
        return Err("second binding queue order changed".to_string());
    }
    let second_sequence = second_queued.sequence;
    let _ = queue.finish(second_queued, Ok(String::new()));

    let mut hash = Fnv64::new();
    hash.bytes(&second_passthrough);
    hash.bytes(&unicode);
    hash.bytes(&paste);
    hash.u64(first_sequence);
    hash.u64(second_sequence);
    hash.byte(state.clients[&first].key_table.raw() as u8);
    hash.byte(state.clients[&second].key_table.raw() as u8);
    Ok(CaseResult {
        name: "semantic-key-routing",
        fingerprint: hash.finish(),
    })
}

fn command_target_binding_case() -> Result<CaseResult, String> {
    let mut state = ServerState::new();
    let client = state.add_client();
    let created = state.create_session("work", COLS, ROWS);
    state
        .attach_client(client, created.session)
        .ok_or("command case attach failed")?;

    let outcomes = execute_text(
        &mut state,
        client,
        "new-window -n logs; bind-key -r -T prefix C-j 'select-pane -D'",
    )?;
    if outcomes.len() != 2
        || !outcomes[0]
            .effects
            .iter()
            .any(|effect| matches!(effect, CommandEffect::EnsurePane { .. }))
    {
        return Err("parsed command list did not create the named window".to_string());
    }

    let context = ResolveContext::for_client(&state, client).map_err(|error| error.to_string())?;
    let target = TargetSpec::parse("work:logs.0").map_err(|error| error.to_string())?;
    let resolved = TargetResolver::new(&state)
        .resolve(&context, TargetKind::Pane, &target)
        .map_err(|error| error.to_string())?;
    let pane = resolved.pane.ok_or("pane target did not resolve")?;
    if Some(pane) != state.clients[&client].attached_pane {
        return Err("named pane target did not resolve to the active pane".to_string());
    }

    let prefix = KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]);
    let binding = KeyEvent::new(
        KeyCode::parse("C-j").map_err(|error| error.to_string())?,
        vec![0x0a],
    );
    if route_key(&mut state, client, InputMode::Normal, prefix, 10) != InputRoute::Consumed {
        return Err("custom binding prefix was not consumed".to_string());
    }
    let first = match route_key(&mut state, client, InputMode::Normal, binding.clone(), 11) {
        InputRoute::Commands(commands) => commands,
        _ => return Err("custom binding did not route to commands".to_string()),
    };
    let repeated = match route_key(&mut state, client, InputMode::Normal, binding, 12) {
        InputRoute::Commands(commands) => commands,
        _ => return Err("repeatable binding did not remain active".to_string()),
    };
    if !matches!(first[0], Command::SelectPane { .. }) || first != repeated {
        return Err("custom binding command changed during repeat".to_string());
    }

    let mut hash = Fnv64::new();
    hash.u64(resolved.session.ok_or("resolved session missing")?.raw());
    hash.u64(resolved.window.ok_or("resolved window missing")?.raw());
    hash.u64(pane.raw());
    hash.usize(first.len());
    hash.byte(state.clients[&client].key_table.raw() as u8);
    Ok(CaseResult {
        name: "command-target-binding",
        fingerprint: hash.finish(),
    })
}

fn command_effects_case() -> Result<CaseResult, String> {
    let mut state = ServerState::new();
    let client = state.add_client();
    let work = state.create_session("work", COLS, ROWS);
    let other = state.create_session("other", COLS, ROWS);
    state
        .attach_client(client, work.session)
        .ok_or("effect case attach failed")?;

    let send = execute_text(&mut state, client, "send-keys -l -N 2 xy")?
        .pop()
        .ok_or("send-keys outcome missing")?;
    let sent = send
        .effects
        .iter()
        .find_map(|effect| match effect {
            CommandEffect::PaneInput { pane, bytes } => Some((*pane, bytes.clone())),
            _ => None,
        })
        .ok_or("send-keys pane effect missing")?;
    if sent != (work.pane, b"xyxy".to_vec()) {
        return Err("send-keys emitted unexpected bytes".to_string());
    }

    let confirmation = execute_text(&mut state, client, "confirm-before -p 'kill?' 'kill-pane'")?
        .pop()
        .ok_or("confirmation outcome missing")?
        .effects
        .into_iter()
        .find_map(|effect| match effect {
            CommandEffect::Confirm {
                client: effect_client,
                prompt,
                commands,
            } if effect_client == client => Some(ConfirmationState { prompt, commands }),
            _ => None,
        })
        .ok_or("confirmation effect missing")?;
    let confirmation_fingerprint = hash_bytes(confirmation.prompt.as_bytes());
    state.clients.get_mut(&client).unwrap().confirmation = Some(confirmation.clone());
    let reject = KeyEvent::new(KeyCode::character('n', KeyModifiers::NONE), b"n".to_vec());
    if route_key(&mut state, client, InputMode::Normal, reject, 20) != InputRoute::Consumed
        || state.clients[&client].confirmation.is_some()
    {
        return Err("confirmation rejection changed state incorrectly".to_string());
    }
    state.clients.get_mut(&client).unwrap().confirmation = Some(confirmation);
    let accept = KeyEvent::new(KeyCode::character('y', KeyModifiers::NONE), b"y".to_vec());
    let accepted = match route_key(&mut state, client, InputMode::Normal, accept, 21) {
        InputRoute::Commands(commands) => commands,
        _ => return Err("confirmation acceptance did not route commands".to_string()),
    };
    if !matches!(accepted[0], Command::KillPane) {
        return Err("confirmation accepted the wrong command".to_string());
    }

    let switched = execute_text(&mut state, client, "switch-client -n; refresh-client")?;
    if state.clients[&client].attached_session != Some(other.session)
        || !switched.iter().all(|outcome| {
            outcome.effects.iter().any(
                |effect| matches!(effect, CommandEffect::RefreshClient { client: refreshed } if *refreshed == client),
            )
        })
    {
        return Err("switch/refresh effects did not update the requesting client".to_string());
    }

    let mut hash = Fnv64::new();
    hash.bytes(&sent.1);
    hash.u64(confirmation_fingerprint);
    hash.usize(accepted.len());
    hash.u64(other.session.raw());
    Ok(CaseResult {
        name: "command-effects",
        fingerprint: hash.finish(),
    })
}

fn unicode_terminal_text_case() -> Result<CaseResult, String> {
    let fixture = "\u{3bb}> cafe\u{301} \u{2764}\u{fe0f} \u{1f469}\u{200d}\u{1f4bb} \u{1f1ec}\u{1f1e7} \u{754c}";
    let mut screen = Screen::new(48, 4);
    let mut terminal = TerminalEngine::new();
    for chunk in fixture.as_bytes().chunks(2) {
        terminal.feed(&mut screen, chunk);
    }

    let line = screen.grid().line(0).ok_or("Unicode fixture row missing")?;
    if line.text() != fixture {
        return Err(format!(
            "Unicode fixture changed: expected {fixture:?}, got {:?}",
            line.text()
        ));
    }
    for (column, text) in [
        (6, "e\u{301}"),
        (8, "\u{2764}\u{fe0f}"),
        (11, "\u{1f469}\u{200d}\u{1f4bb}"),
        (14, "\u{1f1ec}\u{1f1e7}"),
        (17, "\u{754c}"),
    ] {
        let cell = line.cell(column).ok_or("Unicode fixture cell missing")?;
        if cell.text().to_string() != text {
            return Err(format!("Unicode cell {column} lost complete text"));
        }
    }
    for column in [8, 11, 14, 17] {
        if line.width_at(column) != 2 || line.width_at(column + 1) != 0 {
            return Err(format!(
                "Unicode cell {column} lost its wide-cell invariant"
            ));
        }
    }

    Ok(CaseResult {
        name: "unicode-terminal-text",
        fingerprint: hash_screen(&screen),
    })
}

fn terminal_modes_and_title_case() -> Result<CaseResult, String> {
    let fixture = b"primary\x1b]2;wmux;portable\x1b\\\x1b[?1049halt\x1b[?1049l\x1b[1;8H\x1b[?2026hdone\x1b[?2026l\x1b[?2004h";
    let mut screen = Screen::new(32, 4);
    let mut terminal = TerminalEngine::new();
    for chunk in fixture.chunks(3) {
        terminal.feed(&mut screen, chunk);
    }

    if screen.title() != "wmux;portable" {
        return Err("OSC title was not preserved".to_string());
    }
    if screen.alternate_active() {
        return Err("alternate-screen exit did not restore primary state".to_string());
    }
    if screen.grid().line(0).map(|line| line.text()).as_deref() != Some("primarydone") {
        return Err("alternate-screen transition changed primary text".to_string());
    }
    if !screen.bracketed_paste() || screen.synchronized_output() {
        return Err("terminal modes did not settle to the expected state".to_string());
    }

    Ok(CaseResult {
        name: "terminal-modes-and-title",
        fingerprint: hash_screen(&screen),
    })
}

fn bounded_control_recovery_case() -> Result<CaseResult, String> {
    let mut cases = vec![
        ("DCS", b"\x1bPq".to_vec(), b"\x1b\\".to_vec()),
        ("SOS", b"\x1bX".to_vec(), b"\x1b\\".to_vec()),
        ("PM", b"\x1b^".to_vec(), b"\x1b\\".to_vec()),
        ("APC", b"\x1b_".to_vec(), b"\x1b\\".to_vec()),
        ("OSC", b"\x1b]0;".to_vec(), b"\x07".to_vec()),
    ];
    let mut over_parameterized_csi = b"\x1b[".to_vec();
    over_parameterized_csi.extend_from_slice("1;".repeat(40).as_bytes());
    over_parameterized_csi.extend_from_slice(b"1m");
    cases.push(("CSI", over_parameterized_csi, Vec::new()));

    let mut hash = Fnv64::new();
    for (name, introducer, terminator) in cases {
        let mut fixture = Vec::with_capacity(introducer.len() + terminator.len() + 4101);
        fixture.extend_from_slice(&introducer);
        if name != "CSI" {
            fixture.extend(std::iter::repeat_n(b'x', 4096));
        }
        fixture.extend_from_slice(&terminator);
        fixture.extend_from_slice(b"after");

        let mut screen = Screen::new(16, 2);
        let mut terminal = TerminalEngine::new();
        for chunk in fixture.chunks(7) {
            terminal.feed(&mut screen, chunk);
        }
        if screen.grid().line(0).map(|line| line.text()).as_deref() != Some("after") {
            return Err(format!("{name} did not recover to printable ground state"));
        }
        hash.u64(hash_screen(&screen));
    }

    Ok(CaseResult {
        name: "bounded-control-recovery",
        fingerprint: hash.finish(),
    })
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
    hash.byte(u8::from(screen.synchronized_output()));
    hash.u64(screen.synchronized_output_epoch());
    hash.u16(screen.title().len() as u16);
    hash.bytes(screen.title().as_bytes());
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
        assert_eq!(report.cases.len(), 13);
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
