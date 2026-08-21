mod allocation;
#[cfg(test)]
mod legacy_terminal;

use allocation::{begin_measurement, delta, AllocationSnapshot, CountingAllocator};
use std::{
    collections::{BTreeMap, VecDeque},
    env,
    hint::black_box,
    io::{self, Write},
    process,
    time::{Duration, Instant},
};
use wmux_core::{
    build_window_scene, build_window_structure, parse_command_text, render_damage_from_structure,
    render_diff, render_diff_scene, render_full_scene, route_key, ClientId, Command, CommandList,
    CommandQueue, CommandSource, InputMode, InputRoute, KeyCode, KeyEvent, KeyModifiers,
    RenderCapabilities, RenderState, Screen, ServerState, SplitDirection, TerminalEngine,
};
use wmux_protocol::{write_message, Message};

#[global_allocator]
static ALLOCATOR: CountingAllocator = CountingAllocator;

const COLS: u16 = 160;
const ROWS: u16 = 44;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum Suite {
    Smoke,
    Full,
}

impl Suite {
    fn parse(value: &str) -> Option<Self> {
        match value {
            "smoke" => Some(Self::Smoke),
            "full" => Some(Self::Full),
            _ => None,
        }
    }

    const fn name(self) -> &'static str {
        match self {
            Self::Smoke => "smoke",
            Self::Full => "full",
        }
    }

    const fn config(self) -> WorkloadConfig {
        match self {
            Self::Smoke => WorkloadConfig {
                tui_frames: 20,
                paste_bytes: 256 * 1024,
                paste_iterations: 2,
                history_lines: 2_000,
                history_resize_iterations: 20,
                split_rounds: 10,
                backlog_chunks: 128,
                clients: 2,
            },
            Self::Full => WorkloadConfig {
                tui_frames: 400,
                paste_bytes: 8 * 1024 * 1024,
                paste_iterations: 8,
                history_lines: 100_000,
                history_resize_iterations: 200,
                split_rounds: 250,
                backlog_chunks: 8_192,
                clients: 8,
            },
        }
    }
}

#[derive(Clone, Copy, Debug)]
struct WorkloadConfig {
    tui_frames: usize,
    paste_bytes: usize,
    paste_iterations: usize,
    history_lines: usize,
    history_resize_iterations: usize,
    split_rounds: usize,
    backlog_chunks: usize,
    clients: usize,
}

#[derive(Clone, Copy, Debug)]
enum TuiFlavor {
    Codex,
    Claude,
}

impl TuiFlavor {
    const fn name(self) -> &'static str {
        match self {
            Self::Codex => "codex",
            Self::Claude => "claude",
        }
    }
}

#[derive(Debug, Default)]
struct RawResult {
    latencies_ns: Vec<u64>,
    ipc_bytes: u64,
    reference_bytes: u64,
    max_queue_depth: usize,
    final_queue_depth: usize,
    violations: usize,
    client_write: Duration,
    checksum: u64,
}

#[derive(Debug)]
struct Report {
    scenario: &'static str,
    suite: &'static str,
    profile: &'static str,
    elapsed: Duration,
    input_bytes: u64,
    operations: u64,
    allocations: AllocationSnapshot,
    raw: RawResult,
}

#[derive(Debug)]
struct Options {
    suite: Suite,
    scenario: Option<String>,
    json: bool,
    list: bool,
    gate: bool,
}

const SCENARIOS: &[&str] = &[
    "parser-codex",
    "parser-claude",
    "frame-codex",
    "frame-claude",
    "hybrid-frame-codex",
    "hybrid-frame-claude",
    "scene-frame-codex",
    "idle-input-render",
    "damage-proportional",
    "large-paste",
    "history-resize-100k",
    "split-storm",
    "detach-backlog",
    "multiple-clients",
    "key-unbound",
    "key-prefix-binding",
    "command-queue",
    "command-text",
];

fn main() {
    let options = parse_options().unwrap_or_else(|message| {
        eprintln!("{message}");
        print_usage();
        process::exit(2);
    });
    if options.list {
        for scenario in SCENARIOS {
            println!("{scenario}");
        }
        return;
    }

    let config = options.suite.config();
    let mut reports = Vec::new();
    run_selected(
        &options,
        "parser-codex",
        || parser_workload(config, options.suite, TuiFlavor::Codex),
        &mut reports,
    );
    run_selected(
        &options,
        "parser-claude",
        || parser_workload(config, options.suite, TuiFlavor::Claude),
        &mut reports,
    );
    run_selected(
        &options,
        "frame-codex",
        || frame_workload(config, options.suite, TuiFlavor::Codex),
        &mut reports,
    );
    run_selected(
        &options,
        "frame-claude",
        || frame_workload(config, options.suite, TuiFlavor::Claude),
        &mut reports,
    );
    run_selected(
        &options,
        "hybrid-frame-codex",
        || hybrid_frame_workload(config, options.suite, TuiFlavor::Codex),
        &mut reports,
    );
    run_selected(
        &options,
        "hybrid-frame-claude",
        || hybrid_frame_workload(config, options.suite, TuiFlavor::Claude),
        &mut reports,
    );
    run_selected(
        &options,
        "scene-frame-codex",
        || scene_frame_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "idle-input-render",
        || idle_input_render_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "damage-proportional",
        || damage_proportional_workload(options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "large-paste",
        || paste_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "history-resize-100k",
        || history_resize_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "split-storm",
        || split_storm_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "detach-backlog",
        || detach_backlog_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "multiple-clients",
        || multiple_clients_workload(config, options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "key-unbound",
        || key_unbound_workload(options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "key-prefix-binding",
        || key_prefix_binding_workload(options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "command-queue",
        || command_queue_workload(options.suite),
        &mut reports,
    );
    run_selected(
        &options,
        "command-text",
        || command_text_workload(options.suite),
        &mut reports,
    );

    if reports.is_empty() {
        eprintln!(
            "unknown scenario: {}",
            options.scenario.as_deref().unwrap_or_default()
        );
        process::exit(2);
    }
    if options.json {
        for report in &reports {
            print_json(report);
        }
    } else {
        print_human(&reports);
    }
    if options.gate {
        if let Err(failures) = evaluate_performance_gate(&options, &reports) {
            for failure in failures {
                eprintln!("performance gate failed: {failure}");
            }
            process::exit(1);
        }
        eprintln!("performance gate passed");
    }
}

fn run_selected(
    options: &Options,
    name: &str,
    run: impl FnOnce() -> Report,
    reports: &mut Vec<Report>,
) {
    if options
        .scenario
        .as_deref()
        .is_none_or(|selected| selected == name)
    {
        reports.push(run());
    }
}

fn parse_options() -> Result<Options, String> {
    let mut suite = Suite::Smoke;
    let mut scenario = None;
    let mut json = false;
    let mut list = false;
    let mut gate = false;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--suite" => {
                let value = args.next().ok_or("missing value for --suite")?;
                suite = Suite::parse(&value).ok_or_else(|| format!("unknown suite: {value}"))?;
            }
            "--scenario" => scenario = Some(args.next().ok_or("missing value for --scenario")?),
            "--json" => json = true,
            "--list" => list = true,
            "--gate" => gate = true,
            "-h" | "--help" => {
                print_usage();
                process::exit(0);
            }
            _ => return Err(format!("unknown argument: {arg}")),
        }
    }
    Ok(Options {
        suite,
        scenario,
        json,
        list,
        gate,
    })
}

fn print_usage() {
    eprintln!(
        "usage: wmux-bench [--suite smoke|full] [--scenario NAME] [--json] [--list] [--gate]"
    );
}

fn measure(
    scenario: &'static str,
    suite: Suite,
    input_bytes: u64,
    operations: u64,
    run: impl FnOnce() -> RawResult,
) -> Report {
    let allocations_before = begin_measurement();
    let started = Instant::now();
    let raw = run();
    let elapsed = started.elapsed();
    let allocations = delta(allocations_before);
    black_box(raw.checksum);
    Report {
        scenario,
        suite: suite.name(),
        profile: if cfg!(debug_assertions) {
            "debug"
        } else {
            "release"
        },
        elapsed,
        input_bytes,
        operations,
        allocations,
        raw,
    }
}

fn parser_workload(config: WorkloadConfig, suite: Suite, flavor: TuiFlavor) -> Report {
    let frames = tui_frames(flavor, config.tui_frames, COLS, ROWS);
    let input_bytes = total_bytes(&frames);
    let scenario = match flavor {
        TuiFlavor::Codex => "parser-codex",
        TuiFlavor::Claude => "parser-claude",
    };
    measure(scenario, suite, input_bytes, input_bytes, move || {
        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(COLS, ROWS);
        for frame in &frames {
            engine.feed(&mut screen, frame);
        }
        RawResult {
            checksum: checksum_screen(&screen) ^ flavor.name().len() as u64,
            ..RawResult::default()
        }
    })
}

fn frame_workload(config: WorkloadConfig, suite: Suite, flavor: TuiFlavor) -> Report {
    let frames = tui_frames(flavor, config.tui_frames, COLS, ROWS);
    let input_bytes = total_bytes(&frames);
    let scenario = match flavor {
        TuiFlavor::Codex => "frame-codex",
        TuiFlavor::Claude => "frame-claude",
    };
    measure(
        scenario,
        suite,
        input_bytes,
        frames.len() as u64,
        move || {
            let mut engine = TerminalEngine::new();
            let mut screen = Screen::new(COLS, ROWS);
            let mut render_state = RenderState::new(COLS, ROWS);
            let mut ipc_sink = CountingSink::default();
            let mut client_sink = CountingSink::default();
            let mut latencies = Vec::with_capacity(frames.len());
            let mut client_write = Duration::ZERO;

            for frame in &frames {
                let frame_started = Instant::now();
                engine.feed(&mut screen, frame);
                let rendered = render_diff(&screen, &mut render_state);
                latencies.push(nanos(frame_started.elapsed()));
                let message = Message::Output(rendered);
                write_message(&mut ipc_sink, &message).expect("encode output message");
                let write_started = Instant::now();
                let Message::Output(bytes) = &message else {
                    unreachable!("constructed output message");
                };
                client_sink.write_all(bytes).expect("counting sink write");
                client_write += write_started.elapsed();
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: ipc_sink.bytes,
                client_write,
                checksum: ipc_sink.checksum ^ client_sink.checksum ^ checksum_screen(&screen),
                ..RawResult::default()
            }
        },
    )
}

fn hybrid_frame_workload(config: WorkloadConfig, suite: Suite, flavor: TuiFlavor) -> Report {
    let frames = tui_frames(flavor, config.tui_frames, COLS, ROWS);
    let input_bytes = total_bytes(&frames);
    let scenario = match flavor {
        TuiFlavor::Codex => "hybrid-frame-codex",
        TuiFlavor::Claude => "hybrid-frame-claude",
    };
    let mut state = ServerState::new();
    let created = state.create_session("hybrid", COLS, ROWS);
    let structure =
        build_window_structure(&state, created.session, COLS, ROWS).expect("hybrid structure");
    let initial = build_window_scene(&state, created.session, COLS, ROWS).expect("initial scene");
    let mut render_state = RenderState::new(COLS, ROWS);
    black_box(render_full_scene(&initial, &mut render_state));

    measure(
        scenario,
        suite,
        input_bytes,
        frames.len() as u64,
        move || {
            let mut consumed = BTreeMap::new();
            let mut ipc_sink = CountingSink::default();
            let mut client_sink = CountingSink::default();
            let mut latencies = Vec::with_capacity(frames.len());
            let mut client_write = Duration::ZERO;
            for frame in &frames {
                let frame_started = Instant::now();
                {
                    let pane = state.pane_mut(created.pane).expect("hybrid pane");
                    pane.terminal.feed(&mut pane.screen, frame);
                }
                let rendered = render_damage_from_structure(
                    &state,
                    &structure,
                    &consumed,
                    &mut render_state,
                    RenderCapabilities::default(),
                )
                .expect("valid hybrid baseline");
                consumed.insert(
                    created.pane,
                    state.pane(created.pane).expect("hybrid pane").generation(),
                );
                latencies.push(nanos(frame_started.elapsed()));
                let message = Message::Output(rendered);
                write_message(&mut ipc_sink, &message).expect("encode hybrid output");
                let write_started = Instant::now();
                let Message::Output(bytes) = &message else {
                    unreachable!("constructed output message");
                };
                client_sink.write_all(bytes).expect("counting sink write");
                client_write += write_started.elapsed();
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: ipc_sink.bytes,
                client_write,
                checksum: ipc_sink.checksum ^ client_sink.checksum,
                ..RawResult::default()
            }
        },
    )
}

fn idle_input_render_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let iterations = config.tui_frames.max(20);
    let mut state = ServerState::new();
    let created = state.create_session("idle-echo", COLS, ROWS);
    let structure =
        build_window_structure(&state, created.session, COLS, ROWS).expect("idle structure");
    let initial = build_window_scene(&state, created.session, COLS, ROWS).expect("idle scene");
    let mut render_state = RenderState::new(COLS, ROWS);
    black_box(render_full_scene(&initial, &mut render_state));
    let mut consumed = BTreeMap::from([(
        created.pane,
        state.pane(created.pane).expect("idle pane").generation(),
    )]);

    measure(
        "idle-input-render",
        suite,
        iterations as u64,
        iterations as u64,
        move || {
            let mut latencies = Vec::with_capacity(iterations);
            let mut sink = CountingSink::default();
            for _ in 0..iterations {
                let started = Instant::now();
                {
                    let pane = state.pane_mut(created.pane).expect("idle pane");
                    pane.terminal.feed(&mut pane.screen, b"x");
                }
                let rendered = render_damage_from_structure(
                    &state,
                    &structure,
                    &consumed,
                    &mut render_state,
                    RenderCapabilities::default(),
                )
                .expect("valid idle baseline");
                consumed.insert(
                    created.pane,
                    state.pane(created.pane).expect("idle pane").generation(),
                );
                sink.write_all(&rendered).expect("write idle frame");
                latencies.push(nanos(started.elapsed()));
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: sink.bytes,
                checksum: sink.checksum
                    ^ checksum_screen(&state.pane(created.pane).unwrap().screen),
                ..RawResult::default()
            }
        },
    )
}

fn damage_proportional_workload(suite: Suite) -> Report {
    let mut state = ServerState::new();
    let created = state.create_session("damage", COLS, ROWS);
    let structure =
        build_window_structure(&state, created.session, COLS, ROWS).expect("damage structure");
    let initial = build_window_scene(&state, created.session, COLS, ROWS).expect("damage scene");
    let mut render_state = RenderState::new(COLS, ROWS);
    black_box(render_full_scene(&initial, &mut render_state));
    let consumed = BTreeMap::from([(
        created.pane,
        state.pane(created.pane).expect("damage pane").generation(),
    )]);

    measure("damage-proportional", suite, 1, 1, move || {
        let started = Instant::now();
        {
            let pane = state.pane_mut(created.pane).expect("damage pane");
            pane.terminal.feed(&mut pane.screen, b"x");
        }
        let rendered = render_damage_from_structure(
            &state,
            &structure,
            &consumed,
            &mut render_state,
            RenderCapabilities::default(),
        )
        .expect("valid damage baseline");
        let latency = nanos(started.elapsed());
        let current = build_window_scene(&state, created.session, COLS, ROWS)
            .expect("authoritative damage scene");
        let full = render_full_scene(&current, &mut RenderState::new(COLS, ROWS));
        RawResult {
            latencies_ns: vec![latency],
            ipc_bytes: rendered.len() as u64,
            reference_bytes: full.len() as u64,
            violations: usize::from(contains_blank_frame(&rendered)),
            checksum: hash_pair(&rendered, &full),
            ..RawResult::default()
        }
    })
}

fn scene_frame_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let frames = tui_frames(TuiFlavor::Codex, config.tui_frames, COLS, ROWS);
    let input_bytes = total_bytes(&frames);
    let mut state = ServerState::new();
    let created = state.create_session("scene", COLS, ROWS);
    let mut render_state = RenderState::new(COLS, ROWS);
    measure(
        "scene-frame-codex",
        suite,
        input_bytes,
        frames.len() as u64,
        move || {
            let mut sink = CountingSink::default();
            let mut latencies = Vec::with_capacity(frames.len());
            for frame in &frames {
                let started = Instant::now();
                {
                    let pane = state.pane_mut(created.pane).expect("scene pane");
                    pane.terminal.feed(&mut pane.screen, frame);
                }
                let scene = build_window_scene(&state, created.session, COLS, ROWS)
                    .expect("authoritative scene");
                let rendered = render_diff_scene(&scene, &mut render_state);
                write_message(&mut sink, &Message::Output(rendered)).expect("encode scene output");
                latencies.push(nanos(started.elapsed()));
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: sink.bytes,
                checksum: sink.checksum,
                ..RawResult::default()
            }
        },
    )
}

fn paste_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let payload = deterministic_bytes(config.paste_bytes);
    let message = Message::Paste(payload);
    let input_bytes = (config.paste_bytes * config.paste_iterations) as u64;
    measure(
        "large-paste",
        suite,
        input_bytes,
        config.paste_iterations as u64,
        move || {
            let mut sink = CountingSink::default();
            let mut latencies = Vec::with_capacity(config.paste_iterations);
            for _ in 0..config.paste_iterations {
                let started = Instant::now();
                write_message(&mut sink, &message).expect("encode paste message");
                latencies.push(nanos(started.elapsed()));
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: sink.bytes,
                checksum: sink.checksum,
                ..RawResult::default()
            }
        },
    )
}

fn history_resize_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let mut engine = TerminalEngine::new();
    let mut screen = Screen::new(40, 30);
    screen.set_history_limit(config.history_lines + 30);
    let mut batch = Vec::with_capacity(64 * 1024);
    let mut history_input_bytes = 0_u64;
    for line in 0..config.history_lines {
        let before = batch.len();
        write!(&mut batch, "history {line:08}\r\n").expect("write history fixture");
        history_input_bytes += (batch.len() - before) as u64;
        if batch.len() >= 64 * 1024 {
            engine.feed(&mut screen, &batch);
            batch.clear();
        }
    }
    engine.feed(&mut screen, &batch);
    let retained = screen.grid().history_len();
    assert!(retained >= config.history_lines.saturating_sub(31));
    assert_eq!(config.history_resize_iterations % 2, 0);

    measure(
        "history-resize-100k",
        suite,
        history_input_bytes,
        config.history_resize_iterations as u64,
        move || {
            let mut latencies = Vec::with_capacity(config.history_resize_iterations);
            for iteration in 0..config.history_resize_iterations {
                let columns = if iteration % 2 == 0 { 31 } else { 40 };
                let started = Instant::now();
                screen.resize(columns, 30);
                latencies.push(nanos(started.elapsed()));
            }
            RawResult {
                latencies_ns: latencies,
                checksum: checksum_screen(&screen).wrapping_add((retained as u64).rotate_left(17)),
                ..RawResult::default()
            }
        },
    )
}

fn split_storm_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let mut state = ServerState::new();
    let created = state.create_session("bench", COLS, ROWS);
    let mut render_state = RenderState::new(COLS, ROWS);
    let initial = build_window_scene(&state, created.session, COLS, ROWS).expect("initial scene");
    black_box(render_full_scene(&initial, &mut render_state));

    measure(
        "split-storm",
        suite,
        0,
        (config.split_rounds * 2) as u64,
        move || {
            let mut latencies = Vec::with_capacity(config.split_rounds * 2);
            let mut ipc_sink = CountingSink::default();
            let mut violations = 0;
            for round in 0..config.split_rounds {
                let direction = if round % 2 == 0 {
                    SplitDirection::LeftRight
                } else {
                    SplitDirection::TopBottom
                };
                let started = Instant::now();
                let pane = state
                    .split_pane(created.window, None, direction, COLS, ROWS)
                    .expect("split pane");
                let scene =
                    build_window_scene(&state, created.session, COLS, ROWS).expect("split scene");
                let rendered = render_diff_scene(&scene, &mut render_state);
                violations += usize::from(contains_blank_frame(&rendered));
                write_message(&mut ipc_sink, &Message::Output(rendered))
                    .expect("encode split output");
                latencies.push(nanos(started.elapsed()));

                let started = Instant::now();
                state.kill_pane(pane).expect("kill split pane");
                let scene =
                    build_window_scene(&state, created.session, COLS, ROWS).expect("joined scene");
                let rendered = render_diff_scene(&scene, &mut render_state);
                violations += usize::from(contains_blank_frame(&rendered));
                write_message(&mut ipc_sink, &Message::Output(rendered))
                    .expect("encode joined output");
                latencies.push(nanos(started.elapsed()));
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: ipc_sink.bytes,
                violations,
                checksum: ipc_sink.checksum ^ state.panes.len() as u64,
                ..RawResult::default()
            }
        },
    )
}

fn detach_backlog_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let source = tui_frames(TuiFlavor::Codex, 4, COLS, ROWS).concat();
    let mut backlog = VecDeque::with_capacity(config.backlog_chunks);
    for index in 0..config.backlog_chunks {
        let start = (index * 1024) % source.len();
        let mut chunk = Vec::with_capacity(1024);
        let mut cursor = start;
        while chunk.len() < 1024 {
            let remaining = 1024 - chunk.len();
            let available = (source.len() - cursor).min(remaining);
            chunk.extend_from_slice(&source[cursor..cursor + available]);
            cursor = 0;
        }
        backlog.push_back(chunk);
    }
    let input_bytes = backlog.iter().map(Vec::len).sum::<usize>() as u64;
    let max_queue_depth = backlog.len();

    measure(
        "detach-backlog",
        suite,
        input_bytes,
        max_queue_depth as u64,
        move || {
            let mut engine = TerminalEngine::new();
            let mut screen = Screen::new(COLS, ROWS);
            let mut batch = Vec::with_capacity(256 * 1024);
            while !backlog.is_empty() {
                batch.clear();
                for _ in 0..256 {
                    let Some(chunk) = backlog.pop_front() else {
                        break;
                    };
                    batch.extend_from_slice(&chunk);
                }
                engine.feed(&mut screen, &batch);
            }
            let mut render_state = RenderState::new(COLS, ROWS);
            let rendered = render_diff(&screen, &mut render_state);
            let mut ipc_sink = CountingSink::default();
            write_message(&mut ipc_sink, &Message::Output(rendered))
                .expect("encode backlog output");
            RawResult {
                ipc_bytes: ipc_sink.bytes,
                max_queue_depth,
                final_queue_depth: backlog.len(),
                checksum: checksum_screen(&screen) ^ ipc_sink.checksum,
                ..RawResult::default()
            }
        },
    )
}

fn multiple_clients_workload(config: WorkloadConfig, suite: Suite) -> Report {
    let frames = tui_frames(TuiFlavor::Claude, config.tui_frames, COLS, ROWS);
    let input_bytes = total_bytes(&frames);
    measure(
        "multiple-clients",
        suite,
        input_bytes,
        (frames.len() * config.clients) as u64,
        move || {
            let mut engine = TerminalEngine::new();
            let mut screen = Screen::new(COLS, ROWS);
            let mut clients = (0..config.clients)
                .map(|_| RenderState::new(COLS, ROWS))
                .collect::<Vec<_>>();
            let mut latencies = Vec::with_capacity(frames.len());
            let mut ipc_sink = CountingSink::default();
            for frame in &frames {
                let started = Instant::now();
                engine.feed(&mut screen, frame);
                for client in &mut clients {
                    let rendered = render_diff(&screen, client);
                    write_message(&mut ipc_sink, &Message::Output(rendered))
                        .expect("encode multi-client output");
                }
                latencies.push(nanos(started.elapsed()));
            }
            RawResult {
                latencies_ns: latencies,
                ipc_bytes: ipc_sink.bytes,
                max_queue_depth: config.clients,
                checksum: checksum_screen(&screen) ^ ipc_sink.checksum,
                ..RawResult::default()
            }
        },
    )
}

fn phase_4_iterations(suite: Suite) -> (u64, u64, usize, u64) {
    match suite {
        Suite::Smoke => (100_000, 50_000, 50_000, 10_000),
        Suite::Full => (10_000_000, 5_000_000, 1_000_000, 250_000),
    }
}

fn key_unbound_workload(suite: Suite) -> Report {
    let (routes, _, _, _) = phase_4_iterations(suite);
    let mut state = ServerState::new();
    let client = state.add_client();
    let key = KeyCode::character('q', KeyModifiers::NONE);
    let mut raw = vec![b'q'];

    measure("key-unbound", suite, routes, routes, move || {
        let mut checksum = 0_u64;
        let mut violations = 0;
        for now in 0..routes {
            match route_key(
                &mut state,
                client,
                InputMode::Normal,
                KeyEvent::new(key, raw),
                now,
            ) {
                InputRoute::PaneBytes(bytes) => {
                    checksum = checksum
                        .wrapping_mul(0x100000001b3)
                        .wrapping_add(u64::from(bytes[0]));
                    raw = bytes;
                }
                _ => {
                    violations += 1;
                    raw = vec![b'q'];
                }
            }
        }
        RawResult {
            violations,
            checksum,
            ..RawResult::default()
        }
    })
}

fn key_prefix_binding_workload(suite: Suite) -> Report {
    let (_, pairs, _, _) = phase_4_iterations(suite);
    let mut state = ServerState::new();
    let client = state.add_client();
    let prefix = KeyCode::ctrl('b');
    let binding = KeyCode::character('c', KeyModifiers::NONE);

    measure(
        "key-prefix-binding",
        suite,
        pairs.saturating_mul(2),
        pairs,
        move || {
            let mut checksum = 0_u64;
            let mut violations = 0;
            for now in 0..pairs {
                if route_key(
                    &mut state,
                    client,
                    InputMode::Normal,
                    KeyEvent::new(prefix, Vec::new()),
                    now.saturating_mul(2),
                ) != InputRoute::Consumed
                {
                    violations += 1;
                }
                match route_key(
                    &mut state,
                    client,
                    InputMode::Normal,
                    KeyEvent::new(binding, Vec::new()),
                    now.saturating_mul(2).saturating_add(1),
                ) {
                    InputRoute::Commands(commands) => {
                        checksum = checksum
                            .wrapping_mul(0x100000001b3)
                            .wrapping_add(commands.len() as u64);
                        if !matches!(&commands[0], Command::NewWindow { .. }) {
                            violations += 1;
                        }
                    }
                    _ => violations += 1,
                }
            }
            RawResult {
                violations,
                checksum,
                ..RawResult::default()
            }
        },
    )
}

fn command_queue_workload(suite: Suite) -> Report {
    const CLIENTS: usize = 8;
    let (_, _, commands, _) = phase_4_iterations(suite);
    let mut queue = CommandQueue::default();
    for client_index in 0..CLIENTS {
        let mut remaining = commands / CLIENTS + usize::from(client_index < commands % CLIENTS);
        while remaining > 0 {
            let count = remaining.min(256);
            let list = CommandList::new(vec![Command::ListSessions; count])
                .expect("benchmark command list is bounded");
            queue
                .push_list(
                    ClientId::new(client_index as u64 + 1),
                    list,
                    CommandSource::KeyBinding,
                )
                .expect("benchmark queue accepts commands");
            remaining -= count;
        }
    }

    measure(
        "command-queue",
        suite,
        commands as u64,
        commands as u64,
        move || {
            let mut checksum = 0_u64;
            let mut processed = 0_usize;
            while let Some(queued) = queue.pop() {
                checksum = checksum
                    .wrapping_mul(0x100000001b3)
                    .wrapping_add(queued.client.raw())
                    .wrapping_add(queued.sequence);
                if !matches!(queued.command, Command::ListSessions) {
                    checksum ^= u64::MAX;
                }
                processed += 1;
                let _ = queue.finish(queued, Ok(String::new()));
            }
            RawResult {
                max_queue_depth: commands,
                final_queue_depth: usize::from(!queue.is_empty()),
                violations: usize::from(processed != commands),
                checksum: checksum ^ processed as u64,
                ..RawResult::default()
            }
        },
    )
}

fn command_text_workload(suite: Suite) -> Report {
    let (_, _, _, lists) = phase_4_iterations(suite);
    let fixture = "list-sessions; select-window -t +1; send-keys -l x; refresh-client".to_string();
    let input_bytes = fixture.len() as u64 * lists;
    measure("command-text", suite, input_bytes, lists, move || {
        let mut checksum = 0_u64;
        let mut violations = 0;
        for _ in 0..lists {
            match parse_command_text(&fixture) {
                Ok(commands) => {
                    checksum = checksum
                        .wrapping_mul(0x100000001b3)
                        .wrapping_add(commands.len() as u64);
                    for command in commands.iter() {
                        checksum = checksum.wrapping_add(match command {
                            Command::ListSessions => 1,
                            Command::SelectWindow { .. } => 2,
                            Command::SendKeys { .. } => 3,
                            Command::RefreshClient => 4,
                            _ => {
                                violations += 1;
                                0
                            }
                        });
                    }
                }
                Err(_) => violations += 1,
            }
        }
        RawResult {
            violations,
            checksum,
            ..RawResult::default()
        }
    })
}

fn tui_frames(flavor: TuiFlavor, count: usize, cols: u16, rows: u16) -> Vec<Vec<u8>> {
    (0..count)
        .map(|frame| tui_frame(flavor, frame, cols, rows))
        .collect()
}

fn tui_frame(flavor: TuiFlavor, frame: usize, cols: u16, rows: u16) -> Vec<u8> {
    let mut out = Vec::with_capacity(cols as usize * rows as usize);
    out.extend_from_slice(b"\x1b[?2026h\x1b[?25l\x1b[0m");
    if frame == 0 {
        out.extend_from_slice(b"\x1b[2J");
    }
    for row in 0..rows.saturating_sub(4) {
        write!(&mut out, "\x1b[{};1H", row + 1).expect("write cursor sequence");
        let color = match flavor {
            TuiFlavor::Codex => 2 + (row % 5),
            TuiFlavor::Claude => 3 + (row % 4),
        };
        write!(&mut out, "\x1b[38;5;{color}m").expect("write color sequence");
        let marker = flavor.name();
        write!(
            &mut out,
            "{marker} frame={frame:04} row={row:02}  • deterministic terminal workload"
        )
        .expect("write workload row");
        let fill = cols.saturating_sub(72).min(48) as usize;
        out.extend(std::iter::repeat_n(b' ', fill));
        out.extend_from_slice(b"\x1b[K");
    }
    write!(
        &mut out,
        "\x1b[{};1H\x1b[0m╭─ {} prompt ─{}╮\x1b[K",
        rows.saturating_sub(2),
        flavor.name(),
        frame
    )
    .expect("write prompt border");
    write!(
        &mut out,
        "\x1b[{};1H│ explain the current change {:04}                              │\x1b[K",
        rows.saturating_sub(1),
        frame
    )
    .expect("write prompt");
    write!(
        &mut out,
        "\x1b[{};3H\x1b[?25h\x1b[?2026l",
        rows.saturating_sub(1)
    )
    .expect("write final cursor");
    out
}

fn deterministic_bytes(size: usize) -> Vec<u8> {
    let pattern = b"wmux paste workload\r\n0123456789abcdef\r\n";
    (0..size)
        .map(|index| pattern[index % pattern.len()])
        .collect()
}

fn total_bytes(frames: &[Vec<u8>]) -> u64 {
    frames.iter().map(Vec::len).sum::<usize>() as u64
}

fn checksum_screen(screen: &Screen) -> u64 {
    let (row, col) = screen.cursor();
    ((row as u64) << 48)
        ^ ((col as u64) << 32)
        ^ screen.grid().history_len() as u64
        ^ screen.cols() as u64
        ^ ((screen.rows() as u64) << 16)
}

fn nanos(duration: Duration) -> u64 {
    duration.as_nanos().min(u64::MAX as u128) as u64
}

fn contains_blank_frame(bytes: &[u8]) -> bool {
    [b"\x1b[2J".as_slice(), b"\x1b[3J".as_slice()]
        .iter()
        .any(|sequence| {
            bytes
                .windows(sequence.len())
                .any(|window| window == *sequence)
        })
}

fn hash_pair(first: &[u8], second: &[u8]) -> u64 {
    let mut hash = 0xcbf2_9ce4_8422_2325_u64;
    for byte in first.iter().chain(second) {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(0x100_0000_01b3);
    }
    hash
}

#[derive(Default)]
struct CountingSink {
    bytes: u64,
    writes: u64,
    checksum: u64,
}

impl Write for CountingSink {
    fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
        self.bytes += bytes.len() as u64;
        self.writes += 1;
        if let Some(first) = bytes.first() {
            self.checksum = self.checksum.rotate_left(5) ^ u64::from(*first) ^ bytes.len() as u64;
        }
        Ok(bytes.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

fn percentile(values: &[u64], percentile: usize) -> u64 {
    if values.is_empty() {
        return 0;
    }
    let mut sorted = values.to_vec();
    sorted.sort_unstable();
    let rank = (sorted.len() * percentile).div_ceil(100);
    let index = rank.saturating_sub(1).min(sorted.len() - 1);
    sorted[index]
}

fn evaluate_performance_gate(options: &Options, reports: &[Report]) -> Result<(), Vec<String>> {
    let mut failures = Vec::new();
    if cfg!(debug_assertions) {
        failures.push("gates must run from a release build".to_string());
    }
    if options.suite != Suite::Full {
        failures.push("gates require --suite full".to_string());
    }
    if options.scenario.is_some() {
        failures.push("gates require the complete scenario set".to_string());
    }

    let find = |name: &str| reports.iter().find(|report| report.scenario == name);
    let required = [
        "parser-codex",
        "parser-claude",
        "idle-input-render",
        "damage-proportional",
        "history-resize-100k",
        "split-storm",
        "detach-backlog",
        "multiple-clients",
        "key-unbound",
        "key-prefix-binding",
        "command-queue",
        "command-text",
    ];
    for name in required {
        if find(name).is_none() {
            failures.push(format!("required scenario {name} did not run"));
        }
    }
    if !failures.is_empty() {
        return Err(failures);
    }

    for name in ["parser-codex", "parser-claude"] {
        let report = find(name).expect("required report");
        let throughput = report.input_bytes as f64 / report.elapsed.as_secs_f64();
        if throughput < 40_000_000.0 {
            failures.push(format!(
                "{name} throughput {:.1} MB/s is below 40 MB/s",
                throughput / 1_000_000.0
            ));
        }
    }

    for (name, minimum, unit) in [
        ("key-unbound", 15_000_000.0, "routes/second"),
        ("key-prefix-binding", 5_000_000.0, "pairs/second"),
        ("command-queue", 2_000_000.0, "commands/second"),
        ("command-text", 200_000.0, "lists/second"),
    ] {
        let report = find(name).expect("required report");
        let throughput = report.operations as f64 / report.elapsed.as_secs_f64();
        if throughput < minimum {
            failures.push(format!(
                "{name} throughput {throughput:.0} {unit} is below {minimum:.0}"
            ));
        }
        if report.raw.violations != 0 {
            failures.push(format!(
                "{name} recorded {} semantic violations",
                report.raw.violations
            ));
        }
    }
    for name in ["key-unbound", "key-prefix-binding"] {
        let report = find(name).expect("required report");
        if report.allocations.allocations != 0 {
            failures.push(format!(
                "{name} performed {} measured allocations",
                report.allocations.allocations
            ));
        }
    }

    let idle = find("idle-input-render").expect("required report");
    let idle_p95 = percentile(&idle.raw.latencies_ns, 95);
    if idle_p95 >= 16_666_667 {
        failures.push(format!(
            "idle input-to-render p95 {:.3} ms exceeds one 60 Hz frame",
            idle_p95 as f64 / 1_000_000.0
        ));
    }

    let resize = find("history-resize-100k").expect("required report");
    let resize_p95 = percentile(&resize.raw.latencies_ns, 95);
    if resize_p95 >= 5_000_000 {
        failures.push(format!(
            "100k-line resize p95 {:.3} ms exceeds the 5 ms history-independent budget",
            resize_p95 as f64 / 1_000_000.0
        ));
    }

    let split = find("split-storm").expect("required report");
    if split.raw.violations != 0 {
        failures.push(format!(
            "split storm emitted {} whole-display clear frames",
            split.raw.violations
        ));
    }

    let damage = find("damage-proportional").expect("required report");
    if damage.raw.violations != 0 {
        failures.push("ordinary damage emitted a whole-display clear".to_string());
    }
    if damage.raw.ipc_bytes.saturating_mul(4) >= damage.raw.reference_bytes {
        failures.push(format!(
            "one-cell damage emitted {} bytes versus {} for a full scene",
            damage.raw.ipc_bytes, damage.raw.reference_bytes
        ));
    }

    let detached = find("detach-backlog").expect("required report");
    if detached.raw.final_queue_depth != 0 {
        failures.push(format!(
            "detached output left {} chunks unprocessed",
            detached.raw.final_queue_depth
        ));
    }

    const MAX_BOUNDED_LIVE_BYTES: usize = 256 * 1024 * 1024;
    for name in ["detach-backlog", "multiple-clients"] {
        let report = find(name).expect("required report");
        if report.allocations.peak_live_bytes > MAX_BOUNDED_LIVE_BYTES {
            failures.push(format!(
                "{name} peak live memory {:.1} MiB exceeds 256 MiB",
                report.allocations.peak_live_bytes as f64 / (1024.0 * 1024.0)
            ));
        }
    }

    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures)
    }
}

fn print_human(reports: &[Report]) {
    println!(
        "{:<22} {:<7} {:>10} {:>10} {:>10} {:>10} {:>12} {:>12} {:>11} {:>10}",
        "scenario",
        "profile",
        "total ms",
        "p50 us",
        "p95 us",
        "MiB/s",
        "ops/s",
        "alloc MiB",
        "peak MiB",
        "queue"
    );
    for report in reports {
        let seconds = report.elapsed.as_secs_f64();
        let mib_per_second = if seconds == 0.0 {
            0.0
        } else {
            report.input_bytes as f64 / (1024.0 * 1024.0) / seconds
        };
        let operations_per_second = if seconds == 0.0 {
            0.0
        } else {
            report.operations as f64 / seconds
        };
        println!(
            "{:<22} {:<7} {:>10.3} {:>10.3} {:>10.3} {:>10.2} {:>12.0} {:>12.2} {:>11.2} {:>10}",
            report.scenario,
            report.profile,
            report.elapsed.as_secs_f64() * 1000.0,
            percentile(&report.raw.latencies_ns, 50) as f64 / 1000.0,
            percentile(&report.raw.latencies_ns, 95) as f64 / 1000.0,
            mib_per_second,
            operations_per_second,
            report.allocations.allocated_bytes as f64 / (1024.0 * 1024.0),
            report.allocations.peak_live_bytes as f64 / (1024.0 * 1024.0),
            report.raw.max_queue_depth,
        );
    }
    println!("suite={} (use --json for all counters)", reports[0].suite);
}

fn print_json(report: &Report) {
    let seconds = report.elapsed.as_secs_f64();
    let bytes_per_second = if seconds == 0.0 {
        0.0
    } else {
        report.input_bytes as f64 / seconds
    };
    let operations_per_second = if seconds == 0.0 {
        0.0
    } else {
        report.operations as f64 / seconds
    };
    println!(
        concat!(
            "{{\"scenario\":\"{}\",\"suite\":\"{}\",\"profile\":\"{}\",",
            "\"elapsed_ns\":{},\"input_bytes\":{},\"operations\":{},",
            "\"bytes_per_second\":{:.3},\"operations_per_second\":{:.3},",
            "\"p50_ns\":{},\"p95_ns\":{},\"p99_ns\":{},",
            "\"allocations\":{},\"allocated_bytes\":{},\"peak_live_bytes\":{},",
            "\"ipc_bytes\":{},\"reference_bytes\":{},\"max_queue_depth\":{},",
            "\"final_queue_depth\":{},\"violations\":{},\"client_write_ns\":{},",
            "\"checksum\":{}}}"
        ),
        report.scenario,
        report.suite,
        report.profile,
        nanos(report.elapsed),
        report.input_bytes,
        report.operations,
        bytes_per_second,
        operations_per_second,
        percentile(&report.raw.latencies_ns, 50),
        percentile(&report.raw.latencies_ns, 95),
        percentile(&report.raw.latencies_ns, 99),
        report.allocations.allocations,
        report.allocations.allocated_bytes,
        report.allocations.peak_live_bytes,
        report.raw.ipc_bytes,
        report.raw.reference_bytes,
        report.raw.max_queue_depth,
        report.raw.final_queue_depth,
        report.raw.violations,
        nanos(report.raw.client_write),
        report.raw.checksum,
    );
}

#[cfg(test)]
mod tests {
    use super::{deterministic_bytes, tui_frame, tui_frames, Suite, TuiFlavor, SCENARIOS};
    use crate::legacy_terminal::LegacyTerminalEngine;
    use wmux_core::{Screen, TerminalEngine};

    #[test]
    fn required_workloads_are_registered() {
        for required in [
            "parser-codex",
            "parser-claude",
            "large-paste",
            "history-resize-100k",
            "split-storm",
            "detach-backlog",
            "multiple-clients",
        ] {
            assert!(SCENARIOS.contains(&required));
        }
        assert_eq!(Suite::Full.config().history_lines, 100_000);
        assert_eq!(Suite::Full.config().history_resize_iterations, 200);
        assert_eq!(Suite::Smoke.config().history_resize_iterations, 20);
    }

    #[test]
    fn phase_4_hot_path_workloads_are_required() {
        for required in [
            "key-unbound",
            "key-prefix-binding",
            "command-queue",
            "command-text",
        ] {
            assert!(SCENARIOS.contains(&required), "missing {required}");
        }
    }

    #[test]
    fn tui_fixtures_are_deterministic_and_parseable() {
        let first = tui_frame(TuiFlavor::Codex, 7, 80, 24);
        let second = tui_frame(TuiFlavor::Codex, 7, 80, 24);
        assert_eq!(first, second);
        assert_ne!(first, tui_frame(TuiFlavor::Claude, 7, 80, 24));

        let mut engine = TerminalEngine::new();
        let mut screen = Screen::new(80, 24);
        engine.feed(&mut screen, &first);
        assert_eq!(screen.cursor().0, 22);
    }

    #[test]
    fn batched_parser_preserves_every_replay_fixture_grid() {
        for flavor in [TuiFlavor::Codex, TuiFlavor::Claude] {
            let frames = tui_frames(flavor, Suite::Smoke.config().tui_frames, 80, 24);
            let mut legacy = LegacyTerminalEngine::new();
            let mut batched = TerminalEngine::new();
            let mut legacy_screen = Screen::new(80, 24);
            let mut batched_screen = Screen::new(80, 24);

            for frame in frames {
                legacy.feed(&mut legacy_screen, &frame);
                batched.feed(&mut batched_screen, &frame);
                assert_eq!(legacy_screen.cursor(), batched_screen.cursor());
                assert_eq!(
                    legacy_screen.bracketed_paste(),
                    batched_screen.bracketed_paste()
                );
                assert_eq!(
                    legacy_screen.cursor_visible(),
                    batched_screen.cursor_visible()
                );
                for row in 0..legacy_screen.rows() {
                    assert_eq!(
                        legacy_screen.render_line_cells(row),
                        batched_screen.render_line_cells(row),
                        "fixture {flavor:?} diverged at row {row}"
                    );
                }
            }
        }
    }

    #[test]
    fn paste_fixture_has_exact_requested_size() {
        assert_eq!(deterministic_bytes(1_000).len(), 1_000);
    }
}
