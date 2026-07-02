# wmux Robustness Roadmap v2

**Goal:** build `wmux` as a tmux-grade Windows terminal multiplexer, not a toy pane splitter.

This roadmap assumes `wmux` is a long-running daemon that owns sessions, windows, panes, layout state, ConPTY processes, input modes, terminal capabilities, IPC, rendering, diagnostics, and lifecycle cleanup.

Core mindset:

> `wmux` is not a renderer with panes.  
> `wmux` is a daemon-owned state machine with terminal clients, sessions, windows, panes, a layout tree, a command engine, ConPTY processes, terminal capability profiles, and a renderer.

---

## 0. Architecture Contract

Write this into `docs/architecture.md` and `AGENTS.md`.

### Core Object Model

```text
Daemon
├── Sessions
│   ├── Windows
│   │   ├── Layout tree
│   │   └── Panes
│   │       ├── ConPTY handle
│   │       ├── Child process
│   │       ├── Screen grid
│   │       ├── Alternate screen
│   │       └── Scrollback ring
│   └── Session options
├── Clients
│   ├── Attached session/window/pane
│   ├── Terminal size
│   ├── Terminal capabilities
│   ├── Current mode
│   └── Render state
├── Command engine
├── IPC server
├── Event loop
├── Renderer
├── Logger/diagnostics
└── Config
```

### Non-Negotiable Runtime Rule

```text
Only the daemon event loop mutates session/window/pane/layout/process/client state.
Everything else enqueues typed events.
```

Allowed event producers:

```text
PTY readers      -> PaneOutput events
IPC handlers     -> ClientCommand / AttachInput events
Resize watcher   -> ClientResize events
Timers           -> Timer events
Client reader    -> ClientInput events
```

Forbidden:

```text
PTY reader directly changing active pane
IPC handler directly killing panes
Renderer directly mutating layout
Input decoder directly mutating sessions/windows
Any random thread/task touching daemon-owned state
```

### Why this matters

A multiplexer fails when many subsystems fight over state:

```text
input path
process output
resize handling
client attach/detach
rendering
copy mode
mouse mode
process lifecycle
```

The daemon must be the single serialized authority.

---

## 1. Runtime Contract and Support Matrix

Create:

```text
docs/runtime-contract.md
```

### 1.1 Host Terminal Support Tiers

```text
Tier 1:
- Windows Terminal

Tier 2:
- VSCode integrated terminal

Tier 3:
- WezTerm
- Alacritty
- classic conhost, if practical
```

Do not pretend all terminals behave the same. Build a capability/quirk layer from the start.

### 1.2 Shell Support Tiers

```text
Tier 1:
- PowerShell 7 / pwsh
- Windows PowerShell
- cmd.exe

Tier 2:
- wsl.exe
- Git Bash
- NuShell

Tier 3:
- arbitrary custom shell command
```

### 1.3 Document Known Unsupported Cases

Document:

```text
unsupported host terminals
unsupported shells
known mouse limitations
known clipboard limitations
known Unicode limitations
known TUI limitations
admin/elevated shell caveats
remote shell caveats
nested terminal caveats
```

### 1.4 Implement `wmux doctor`

Command:

```bash
wmux doctor
```

Print:

```text
wmux version
Windows version/build
host terminal guess
shell guess
ConPTY availability
VT input mode
VT output mode
truecolor support guess
mouse support guess
bracketed paste support guess
clipboard backend
config path
log path
IPC path
daemon status
active sessions/windows/panes
```

Also add:

```bash
wmux doctor --json
```

This should become the first thing users paste into bug reports.

---

## 2. Terminal Capability and Quirk Layer

tmux has terminal features, terminal overrides, terminfo integration, and per-terminal feature detection. `wmux` needs a Windows-oriented equivalent.

### 2.1 Terminal Capability Model

```rust
struct TerminalCapabilities {
    host: TerminalHost,

    supports_truecolor: bool,
    supports_256_color: bool,
    supports_sgr_mouse: bool,
    supports_mouse_drag: bool,
    supports_mouse_wheel: bool,
    supports_bracketed_paste: bool,
    supports_focus_events: bool,
    supports_cursor_style: bool,
    supports_alt_screen: bool,
    supports_extended_keys: bool,
    supports_osc52_clipboard: bool,
    supports_synchronized_output: bool,

    quirks: Vec<TerminalQuirk>,
}
```

```rust
enum TerminalHost {
    WindowsTerminal,
    VSCode,
    WezTerm,
    Alacritty,
    Conhost,
    Unknown,
}
```

```rust
enum TerminalQuirk {
    BrokenAltKeySequences,
    CtrlBreakSpecialHandling,
    MouseWheelEncodingDiffers,
    NoOsc52Clipboard,
    VscodeKeyTranslation,
    LegacyConhostMode,
    CursorStyleUnsupported,
    UnknownEscapeSequences,
}
```

### 2.2 Config Overrides

Allow users to force/override capability detection:

```toml
[terminal]
host = "windows-terminal"
truecolor = true
mouse = true
bracketed_paste = true
extended_keys = false
osc52_clipboard = false

[terminal.quirks]
vscode_key_translation = true
legacy_conhost_mode = false
```

### 2.3 Capability Consumers

Subsystems must not hardcode terminal assumptions.

```text
Input decoder checks extended-key support.
Mouse mode checks mouse protocol support.
Renderer checks truecolor/cursor/alt-screen support.
Clipboard checks Windows clipboard vs OSC52 support.
Doctor prints detected capabilities and overrides.
```

### 2.4 Terminal Compatibility Tests

Create tests/manual scripts for:

```text
Windows Terminal + pwsh
Windows Terminal + Windows PowerShell
Windows Terminal + cmd
VSCode integrated terminal + pwsh
WezTerm + pwsh
Alacritty + pwsh
```

For each, verify:

```text
Ctrl+b prefix
arrow keys
modified arrows
Alt keys
function keys
mouse click
mouse drag
mouse wheel
resize events
copy/paste
truecolor
alternate screen app
terminal cleanup after crash
```

---

## 3. Layout Tree and Geometry Invariants

This is central. Do not treat layout as scattered pane rectangles.

### 3.1 Use N-Child Weighted Split Groups

```rust
enum LayoutNode {
    Pane {
        pane_id: PaneId,
    },

    Split {
        axis: SplitAxis,
        children: Vec<NodeId>,
        weights: Vec<f32>,
    },
}
```

```rust
enum SplitAxis {
    LeftRight, // children arranged left-to-right; widths divided
    TopBottom, // children arranged top-to-bottom; heights divided
}
```

### 3.2 Required Invariants

Add these as code comments, debug assertions, and tests.

```text
Every pane has exactly one leaf node.
Every leaf has exactly one PaneId.
Every PaneId points back to its layout leaf.
Every split has at least two children.
weights.len() == children.len().
All weights are positive finite numbers.
No split node directly contains a same-axis split node.
Layout rects are derived, not source of truth.
Pane body rects exclude borders/status line.
Minimum pane size is enforced before ConPTY resize.
Killing a pane removes its leaf and normalizes the tree.
If a split has one child after removal, collapse it.
If adjacent same-axis splits appear, flatten them.
```

### 3.3 Normalize Same-Axis Splits

Bad:

```text
LeftRight
├── Pane A
└── LeftRight
    ├── Pane B
    └── Pane C
```

Normalize to:

```text
LeftRight
├── Pane A
├── Pane B
└── Pane C
```

Opposite-axis nesting is valid:

```text
LeftRight
├── Pane A
└── TopBottom
    ├── Pane B
    └── Pane C
```

### 3.4 Split Insertion Rules

When splitting an active pane:

#### Case A: Parent has same axis

Before:

```text
LeftRight
├── A weight 1
└── B weight 1
```

Split `B` left-right:

```text
LeftRight
├── A weight 1
├── B weight 0.5
└── C weight 0.5
```

The old pane's weight is divided between old and new panes.

#### Case B: Parent has different axis

Before:

```text
LeftRight
├── A
└── B
```

Split `B` top-bottom:

```text
LeftRight
├── A
└── TopBottom
    ├── B weight 1
    └── C weight 1
```

### 3.5 Equalize / Spread Panes

Internal command:

```rust
Command::SpreadPanesEvenly
```

Do not model this as:

```rust
EqualizeHorizontal
EqualizeVertical
```

The command should be semantic:

```text
Spread/equalize the nearest meaningful split group around the active pane.
```

Algorithm:

```text
1. Start at active pane leaf.
2. Move to parent split group.
3. Try setting all child weights in that group to 1.0.
4. Recompute rects.
5. If any visual size changed, stop.
6. If no visual size changed, climb to parent split group.
7. Repeat until root.
8. If no group changes, show "Panes already evenly spread".
```

This mirrors tmux-like behavior where equalize/spread starts local and climbs outward if the local group is already equal.

### 3.6 Geometry Computation Pipeline

```text
client terminal size
-> window area
-> subtract status line
-> root layout rect
-> recursively compute child rects from weights
-> compute pane body rects
-> compute border rects
-> resize ConPTY panes
-> render borders/status/output
```

Important:

```text
The layout tree stores structure and weights.
Computed rectangles are derived state.
ConPTY sizes come from pane body rectangles, not outer split rectangles.
```

### 3.7 Minimum Pane Sizes

Define:

```rust
struct PaneSizeLimits {
    min_cols: u16,
    min_rows: u16,
}
```

Suggested default:

```text
min_cols = 10 or 20
min_rows = 3 or 5
```

For TUI stability, optionally define stricter limits:

```text
min_tui_cols = 20 or 30
min_tui_rows = 5 or 8
```

When a split/equalize/resize would violate minimums:

```text
refuse the operation, or
clamp sizes, or
mark panes as hidden/too-small later
```

For v1, refusal with status message is safer.

### 3.8 Layout Tests

Add tests for:

```text
split same axis flattens
split opposite axis nests
kill pane collapses one-child split
kill pane preserves opposite-axis nesting
equalize local group
equalize climbs when local group already equal
rapid split/kill/equalize preserves invariants
computed rects fit inside terminal
sum of child rects + borders equals parent rect
minimum size enforcement
```

Add property tests if possible:

```text
random split/kill/equalize/resize sequence
assert layout invariants after every step
```

---

## 4. Session / Window / Pane Model

### 4.1 Data Types

```rust
struct Session {
    id: SessionId,
    name: String,
    windows: Vec<WindowId>,
    active_window: WindowId,
    options: SessionOptions,
    created_at: Instant,
}
```

```rust
struct Window {
    id: WindowId,
    session_id: SessionId,
    index: usize,
    name: String,
    panes: HashMap<PaneId, Pane>,
    active_pane: PaneId,
    layout_root: NodeId,
    layout_arena: LayoutArena,
    options: WindowOptions,
}
```

```rust
struct Pane {
    id: PaneId,
    window_id: WindowId,
    process: PaneProcess,
    screen: ScreenGrid,
    scrollback: ScrollbackRing,
    alt_screen: Option<ScreenGrid>,
    mode_state: PaneModeState,
    last_exit: Option<ExitStatus>,
}
```

```rust
struct Client {
    id: ClientId,
    attached_session: Option<SessionId>,
    active_window: Option<WindowId>,
    active_pane: Option<PaneId>,
    terminal_caps: TerminalCapabilities,
    mode: ClientMode,
    size: TerminalSize,
    render_state: RenderState,
}
```

### 4.2 Stable IDs

Use stable IDs internally:

```text
SessionId
WindowId
PaneId
ClientId
NodeId
RequestId
```

Do not use indexes internally except for display and user-facing targeting.

### 4.3 Active Object Rules

Track:

```text
active session per client
active window per session
active pane per window
```

When a pane/window is killed:

```text
choose next sensible active pane/window
never leave active IDs dangling
log the transition
show status message if user-visible
```

---

## 5. Daemon Event Model Hardening

### 5.1 Event Enum

```rust
enum DaemonEvent {
    ClientConnected(ClientId),
    ClientDisconnected(ClientId),

    ClientInput {
        client_id: ClientId,
        bytes: Vec<u8>,
    },

    DecodedKey {
        client_id: ClientId,
        key: KeyEvent,
    },

    MouseEvent {
        client_id: ClientId,
        event: MouseEvent,
    },

    IpcCommand {
        client_id: Option<ClientId>,
        request_id: RequestId,
        command: Command,
    },

    PaneOutput {
        pane_id: PaneId,
        bytes: Bytes,
    },

    PaneExited {
        pane_id: PaneId,
        status: ExitStatus,
    },

    ClientResize {
        client_id: ClientId,
        cols: u16,
        rows: u16,
    },

    Timer(TimerEvent),
    Shutdown,
}
```

### 5.2 Event Loop

```rust
while let Some(event) = event_rx.recv().await {
    daemon.handle_event(event);
}
```

### 5.3 State Mutation Guard

In debug builds:

```text
assert daemon state mutation only happens through daemon event loop
log mutation source
panic in tests on wrong-thread mutation
```

### 5.4 Event Ordering Rules

Define ordering guarantees:

```text
PaneOutput may arrive after PaneExited.
ClientInput may arrive after ClientDisconnected.
Resize may arrive while layout operation is pending.
Timer events may fire after object deletion.
```

Handlers must treat stale IDs safely:

```text
if target pane/client/window does not exist:
    log debug
    drop event safely
```

---

## 6. Command Engine and Command Queue

All user actions should become commands.

### 6.1 Command Enum

```rust
enum Command {
    NewSession { name: Option<String> },
    KillSession { target: Target },

    NewWindow { target: Target, shell: Option<String> },
    NextWindow { target: Target },
    PreviousWindow { target: Target },
    KillWindow { target: Target },

    SplitPane { target: Target, axis: SplitAxis },
    KillPane { target: Target, confirm: bool },
    SelectPane { target: Target },
    ResizePane { target: Target, direction: ResizeDirection, amount: u16 },
    SpreadPanesEvenly { target: Target },

    EnterCopyMode { target: Target },
    ExitCopyMode { target: Target },
    CopySelection { target: Target },
    PasteBuffer { target: Target },

    SetOption { scope: OptionScope, key: String, value: String },
    DisplayMessage { target: Target, message: String },

    DetachClient { target: Target },
    AttachSession { target: Target },
}
```

### 6.2 Command Result

```rust
struct CommandResult {
    status: CommandStatus,
    message: Option<String>,
    redraw: RedrawRequest,
    events: Vec<DaemonEvent>,
}
```

```rust
enum CommandStatus {
    Success,
    UserError,
    InternalError,
    NoOp,
}
```

### 6.3 Command Pipeline

```text
raw input bytes
-> key decoder
-> mode handler
-> key binding table
-> Command
-> target resolver
-> command executor
-> state mutation
-> status message/log/redraw
```

### 6.4 Command Logging

Every command log should include:

```text
request id
client id
session id
window id
pane id
command name
result
status message
elapsed time
```

---

## 7. Target Resolution

### 7.1 Target Types

```rust
enum Target {
    Current,
    Session(SessionId),
    Window(WindowId),
    Pane(PaneId),
    Client(ClientId),
    MousePosition { client_id: ClientId, x: u16, y: u16 },
    Named(String),
}
```

### 7.2 Resolved Target

```rust
struct ResolvedTarget {
    session_id: SessionId,
    window_id: WindowId,
    pane_id: Option<PaneId>,
    client_id: Option<ClientId>,
}
```

### 7.3 Rules

All commands use target resolver.

Do not let commands manually search by indexes or names.

Resolver responsibilities:

```text
current client -> active session/window/pane
mouse position -> pane under cursor
window index/name -> WindowId
pane index/id -> PaneId
dead/stale target -> user-visible error
```

---

## 8. ConPTY Lifecycle Hardening

Windows-specific core.

### 8.1 PaneProcess Model

```rust
struct PaneProcess {
    pseudo_console: PseudoConsoleHandle,
    process_handle: ProcessHandle,
    thread_handle: ThreadHandle,
    input_pipe_write: PipeHandle,
    output_pipe_read: PipeHandle,
    process_id: u32,
    created_at: Instant,
    killed: bool,
}
```

### 8.2 Handle Audit

Audit all handles:

```text
pseudo console handle
child process handle
child thread handle
input pipe read/write
output pipe read/write
job object handle, if used
client pipe handles
IPC handles
```

Every handle must have:

```text
single owner
clear close point
RAII wrapper
debug logging on close
```

### 8.3 Idempotent Kill

Pane/window/session kill must be idempotent.

Calling kill twice should not:

```text
panic
double-close handles
leave orphan shell
corrupt layout
send duplicate exit events that break state
```

Pseudo-contract:

```rust
fn kill_pane(pane_id: PaneId) -> Result<KillResult> {
    if pane already killed:
        return Ok(AlreadyKilled)

    mark pane killed
    stop accepting input
    close stdin pipe
    terminate process tree if needed
    close ConPTY
    wait bounded time
    force terminate if needed
    emit PaneExited or cleanup event
}
```

### 8.4 Process Tree Cleanup

Where possible, use Windows Job Objects to group child processes.

Goals:

```text
killing a pane kills its shell and descendants
killing a session kills all panes
daemon exit does not leave pwsh/cmd/conhost/wmux child processes
```

Test for orphans:

```powershell
Get-Process powershell,pwsh,cmd,conhost,wmux -ErrorAction SilentlyContinue
```

### 8.5 Pipe Servicing Rules

Do not block daemon event loop on ConPTY pipe reads/writes.

Rules:

```text
PTY output reader runs separately and enqueues PaneOutput.
PTY input writer must not hold daemon state locks.
Never wait on child process while holding daemon lock.
Never write to pipes while holding layout/session locks.
Close pipes in correct order.
```

### 8.6 ConPTY Resize

Only resize after final pane body rect is known.

Pipeline:

```text
layout change
-> recompute all pane body sizes
-> call ResizePseudoConsole for affected panes
-> mark panes dirty
-> redraw
```

Never do:

```text
resize ConPTY mid-layout
resize before border/status subtraction
resize while holding render/client pipe lock
```

---

## 9. IPC Protocol and Security

### 9.1 Separate Control IPC from Attach Stream

Do not mix command/control messages with raw terminal stream chaos.

Use separate channels or framed stream types:

```text
control IPC:
    new session
    list sessions
    attach
    detach
    kill
    status

attach stream:
    client input
    rendered output
    resize
    mode/status events
```

### 9.2 Framed Protocol

JSON is acceptable initially if it is inside a strict frame.

Frame:

```text
magic: WMUX
version: u16
kind: Control | AttachInput | AttachOutput | Event | Error
request_id: u64
length: u32
payload: json/msgpack/bincode
```

Requirements:

```text
reject oversized frames
reject malformed frames
reject partial frames cleanly
reject unknown message kinds
reject unsupported protocol versions
never panic on invalid input
```

### 9.3 Request IDs

Every command request has request ID.

Responses include same request ID.

Logs include request ID.

### 9.4 IPC Security

On Windows, named pipe/socket path must be user-specific.

Add:

```text
user-specific pipe name/path
ACL/security descriptor where possible
reject cross-user clients unless explicitly allowed
protocol version negotiation
client/daemon compatibility check
clear error for incompatible client version
```

Reason: IPC can control shells. Treat it as sensitive local control plane.

### 9.5 IPC Fuzz Tests

Fuzz:

```text
random bytes
wrong magic
wrong version
huge length
partial frame
truncated JSON
unknown command
invalid target
invalid UTF-8
nested huge payload
```

Expected:

```text
daemon survives
connection rejected or error returned
no memory blow-up
no panic
no shell command execution
```

---

## 10. Terminal Input Hardening

### 10.1 Layering

Do not let raw bytes directly become commands.

```text
raw bytes
-> terminal input decoder
-> KeyEvent / MouseEvent / PasteEvent
-> mode handler
-> binding table
-> Command
```

### 10.2 KeyEvent Model

```rust
struct KeyEvent {
    key: Key,
    modifiers: Modifiers,
    raw: Option<Vec<u8>>, // debug only, privacy-safe
}
```

```rust
enum Key {
    Char(char),
    Enter,
    Escape,
    Backspace,
    Tab,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    Function(u8),
    CtrlBreak,
    Unknown,
}
```

### 10.3 Prefix State Machine

```text
Normal
-> Ctrl+b
-> PrefixPending
-> next key decides command
-> return Normal
```

Implement:

```text
Ctrl+b c -> new window
Ctrl+b n -> next window
Ctrl+b p -> previous window
Ctrl+b x -> kill pane
Ctrl+b E -> spread panes evenly
Ctrl+b [ -> copy mode
Ctrl+b ] -> paste buffer
```

### 10.4 Escape-Time Handling

Alt keys and Escape-prefixed sequences are tricky.

Add configurable:

```toml
[input]
escape_time_ms = 50
```

Rules:

```text
Escape alone exits copy mode / sends Escape.
Escape followed quickly by sequence may be Alt/function/arrow sequence.
Slow sequence should not break normal Escape.
```

### 10.5 Input Debug Mode

Command:

```bash
wmux debug-keys
```

or runtime option:

```text
Ctrl+b ? maybe later
```

Logs decoded keys without leaking normal typed text.

Safe logging:

```text
DecodedKey { key: Ctrl+b, modifiers: CTRL }
DecodedKey { key: Char('c'), printable: true }
MouseEvent { kind: Press, x: 10, y: 4 }
PasteEvent { bytes_len: 438 }
```

Avoid logging:

```text
actual pasted password/text
full command line typed by user
```

### 10.6 Input Tests

Test:

```text
Ctrl+b prefix
Ctrl+b c/n/p/x/E/[ /]
arrows
modified arrows
Alt keys
function keys
Ctrl+C
Ctrl+Break
bracketed paste
mouse click
mouse drag
mouse wheel
slow escape sequence
invalid escape sequence
```

---

## 11. Mode System

Copy mode and prefix mode should not be ad-hoc flags scattered around.

### 11.1 Client Mode

```rust
enum ClientMode {
    Normal,
    PrefixPending { started_at: Instant },
    CopyMode(CopyModeState),
    CommandPrompt(CommandPromptState), // later
    MouseDragResize(MouseDragState),
}
```

### 11.2 Mode Rules

```text
Input decoder only decodes input.
Mode handler decides meaning.
Mode transition is explicit and logged.
Copy mode input never goes to shell.
Normal mode input goes to active pane unless it becomes command.
Prefix mode consumes one following key.
Mouse drag mode captures mouse until release/cancel.
```

### 11.3 Mode Tests

```text
Ctrl+b enters PrefixPending
unknown prefix key shows status and returns Normal
Ctrl+b [ enters CopyMode
q exits CopyMode
Escape exits CopyMode
copy mode navigation does not send keys to shell
detach/crash restores client terminal modes
```

---

## 12. Terminal Screen / Grid / VT Parser Hardening

This is a major subsystem.

### 12.1 Layering

Separate:

```text
bytes from ConPTY
-> VT parser
-> terminal operations/events
-> screen grid mutation
-> renderer
```

Do not combine parser, grid, and renderer in one giant function.

### 12.2 VT Parser Coverage

Implement/test:

```text
cursor movement
cursor position
erase in display
erase in line
insert/delete chars
insert/delete lines
scroll regions
SGR styles
256 color
truecolor
bold/dim/italic/underline/reverse
alternate screen
save/restore cursor
tabs
wrap mode
origin mode if needed
hide/show cursor
set cursor style if needed
OSC title sequences
unknown sequence handling
```

### 12.3 Screen Grid Model

```rust
struct ScreenGrid {
    cols: u16,
    rows: u16,
    cursor: Cursor,
    cells: Vec<Cell>,
    scroll_region: Option<ScrollRegion>,
    modes: ScreenModes,
}
```

```rust
struct Cell {
    grapheme: String,
    width: CellWidth,
    style: Style,
}
```

### 12.4 Golden Tests

Create golden sequence tests for:

```text
PowerShell prompt
cmd prompt
git status/log/diff
Python REPL
less/pager behavior
vim/nvim where possible
clear screen
alternate screen enter/exit
colored output
progress bars
long wrapped lines
```

Expected output should be normalized into a testable grid snapshot.

### 12.5 Unknown VT Sequences

Unknown sequences must not corrupt screen.

Policy:

```text
ignore safely
log debug with sequence class/length
never panic
never write raw control bytes into visible grid
```

---

## 13. Unicode and Wide Cell Correctness

This is a rabbit hole but essential for robustness.

### 13.1 Handle

```text
ASCII
CJK wide characters
emoji fallback
combining marks
zero-width characters
ambiguous-width characters
regional indicators
variation selectors
```

### 13.2 Width Policy

Choose a width library/policy and document it.

Tests:

```text
ASCII only
CJK only
emoji
combining accents
mixed ASCII + CJK
mixed ASCII + emoji
selection across wide cells
cursor movement over wide cells
copy extraction of wide text
```

### 13.3 Grid Rules

```text
Wide char occupies leading cell + continuation cell.
Cursor cannot land on continuation cell.
Erase clears both parts of wide char.
Selection highlighting covers full wide char.
Copy extraction emits logical text, not cell artifacts.
```

---

## 14. Rendering and Backpressure

### 14.1 Rendering Pipeline

```text
daemon state change
-> mark dirty window/pane/region
-> coalesce redraw request
-> renderer builds frame/diff
-> client writer sends bytes
```

### 14.2 Avoid Full Redraws Where Possible

Start simple, but design for:

```text
dirty panes
dirty borders
dirty status line
dirty copy-mode overlay
dirty full screen
```

### 14.3 Coalescing

Do not render on every byte of output.

Use:

```text
max frame rate, e.g. 30-60 FPS
immediate redraw for user input if needed
coalesce high-output pane redraws
```

### 14.4 Backpressure Policy

Important rules:

```text
Never hold daemon state lock while writing to client pipe.
Never let slow client stall pane output.
Bound client output queues.
If client is too slow, detach/degrade that client.
Do not drop PTY bytes unless an explicit degraded mode is implemented.
Prefer dropping/coalescing render frames, not shell output.
```

### 14.5 Metrics

Track:

```text
pending pane output bytes
pending client output bytes
dirty region count
render frame duration
frames per second
dropped/coalesced frames
slow client count
```

---

## 15. Resize Correctness

### 15.1 Resize Pipeline

```text
ClientResize event
-> update client size
-> compute window area
-> recompute layout rects
-> clamp minimum pane sizes
-> compute pane body rects
-> ResizePseudoConsole for affected panes
-> clamp copy-mode viewport/cursor/selection
-> mark dirty
-> redraw
```

### 15.2 Resize Stress Tests

```text
rapid terminal resize
resize while high-output command runs
resize while copy mode active
resize during mouse drag
resize during split/kill/equalize
resize during attach/detach
resize to tiny terminal
resize back to large terminal
```

### 15.3 Correctness Checks

```text
borders do not overwrite pane output
pane output does not overwrite status line
computed pane body size matches ConPTY size
copy-mode cursor remains valid
selection remains valid or is clamped
no panic on tiny size
```

---

## 16. Copy / Scrollback / Paste / Clipboard

### 16.1 Scrollback Ring

Bounded ring:

```rust
struct ScrollbackRing {
    max_lines: usize,
    lines: VecDeque<GridLine>,
}
```

Rules:

```text
never unbounded
memory usage predictable
alternate screen does not pollute normal scrollback unless intentionally configured
clear screen behavior documented
```

### 16.2 Copy Mode

Required behavior:

```text
Ctrl+b [ enters copy mode
q exits
Escape exits
arrows navigate
PageUp/PageDown work
Home/End work
Space starts selection
movement extends selection
reversed selections work
selection across scrollback + live grid works
highlight accurate
```

### 16.3 Copy Extraction

Handle:

```text
wrapped lines
empty lines
line boundaries
wide characters
combining characters
selection ending mid-wide-char
alternate-screen behavior
```

Output rules:

```text
logical text, not raw cell dump
normalized newlines
preserve intentional line breaks
handle wrapped lines like tmux-style copy where practical
```

### 16.4 Paste Buffer

Internal wmux paste buffer is authoritative.

```rust
struct PasteBuffer {
    id: BufferId,
    text: String,
    created_at: Instant,
    source: BufferSource,
}
```

Rules:

```text
Copy always writes wmux buffer first.
Windows clipboard write is best-effort/async.
Clipboard failure shows status but does not break copy.
Buffer size is bounded.
Large paste is bounded/throttled.
```

### 16.5 Paste

```text
Ctrl+b ] pastes wmux buffer
multiline paste works
bracketed paste support if enabled
large paste throttled
paste never blocks render/input loop
```

### 16.6 Clipboard Integration

```text
copy writes to Windows clipboard reliably
clipboard writes happen off critical path
failure shows status
external paste into other apps works
large copied text bounded
```

---

## 17. Mouse Mode

### 17.1 Capabilities

Mouse mode depends on terminal profile.

```text
SGR mouse support
drag support
wheel support
button reporting
modifier reporting if available
```

### 17.2 Features

```text
click-to-focus panes
drag borders to resize
wheel scrollback/copy mode
status/debug signal when mouse events received
```

### 17.3 Mouse Routing

```text
raw mouse sequence
-> MouseEvent
-> mode handler
-> target resolver from x/y
-> Command
```

Mouse actions should not directly mutate layout.

### 17.4 Tests

```text
click active pane
click border
drag border
wheel in normal mode
wheel in copy mode
mouse disabled
host terminal without mouse support
```

---

## 18. Status Line and UX Feedback

### 18.1 Status Model

```rust
struct StatusState {
    permanent_left: StatusSegment,
    permanent_right: StatusSegment,
    temporary_message: Option<TempMessage>,
}
```

### 18.2 Required Feedback

Show status for:

```text
new window
window changed
pane killed
pane kill refused because last pane
equalize success/no-op
copy mode entered/exited
copy success/failure
paste success/failure
clipboard failure
mouse enabled/disabled
unknown keybind
command error
shell spawn failure
```

### 18.3 Active Context Visibility

Status should show:

```text
session name
window index/name
active window
active pane
mode: prefix/copy/mouse
temporary messages
```

### 18.4 Message Clearing

Temporary messages:

```text
clear after timeout
clear on next command if appropriate
do not persist forever
do not flicker under high output
```

---

## 19. Client Failure and Terminal Cleanup

### 19.1 Terminal Mode RAII

Track modes enabled by wmux:

```text
raw mode
mouse reporting
bracketed paste
focus events
cursor visibility
cursor style
alternate screen
title changes if any
```

On detach/crash/reset:

```text
restore what wmux changed
emit conservative reset sequence
show cursor
disable mouse
disable bracketed paste
leave alternate screen if used
restore sane line mode where possible
```

### 19.2 `wmux reset-terminal`

Command:

```bash
wmux reset-terminal
```

Should emit recovery sequences:

```text
show cursor
disable mouse reporting
disable bracketed paste
disable focus events
reset styles
exit alternate screen
clear stuck modes where possible
```

### 19.3 Failure Cases

Handle:

```text
intentional detach
client crash
pipe failure
Ctrl+C
Ctrl+Break
terminal close
daemon crash
daemon restart
network/remote terminal weirdness later
```

### 19.4 Tests

Manual/crash-path tests:

```text
kill client process mid-session
close terminal tab
Ctrl+C during attach
Ctrl+Break during attach
daemon exits while attached
client exits while shell running
terminal state restored after each
```

---

## 20. Shell Spawn and Environment Contract

### 20.1 Default Shell Resolution

Define order:

```text
configured shell
PowerShell 7 / pwsh if found
Windows PowerShell
cmd.exe
```

### 20.2 Spawn Parameters

Log safely:

```text
shell path
args
cwd
environment source
process id
ConPTY size
```

Do not log secrets.

### 20.3 CWD Rules

Define:

```text
new pane inherits current pane cwd if detectable, or
new pane inherits session cwd, or
new pane uses daemon cwd/config default
```

On Windows, shell cwd detection may be imperfect. Document behavior.

### 20.4 Spawn Failure

If shell spawn fails:

```text
show status message
log detailed error
do not corrupt layout
remove failed pane cleanly or show dead pane explicitly
```

---

## 21. Options and Config System

tmux has extensive options/scopes. wmux can start smaller but should respect the architecture.

### 21.1 Option Scopes

```text
global options
session options
window options
pane options
client options
```

### 21.2 Config File

Example:

```toml
[prefix]
key = "C-b"

[status]
enabled = true
position = "bottom"

[terminal]
truecolor = true
mouse = true

[scrollback]
max_lines = 10000

[keys]
"prefix c" = "new-window"
"prefix n" = "next-window"
"prefix p" = "previous-window"
"prefix x" = "kill-pane"
"prefix E" = "spread-panes-evenly"
```

### 21.3 Config Validation

```text
unknown keys error clearly
invalid keybinds error clearly
invalid shell path error clearly
config path printed by doctor
```

---

## 22. Observability and Diagnostics

This should be first-class.

### 22.1 Structured Logs

Log as structured text/JSONL.

Fields:

```text
timestamp
level
event_type
request_id
client_id
session_id
window_id
pane_id
message
metadata
```

### 22.2 `wmux server status`

Command:

```bash
wmux server status
```

Shows:

```text
daemon uptime
sessions/windows/panes
clients
process ids
ConPTY sizes
memory estimate
handle count
event queue depth
render queue depth
recent errors
```

### 22.3 `wmux dump-state`

Command:

```bash
wmux dump-state
```

Outputs:

```text
sessions
windows
panes
clients
layout tree
active ids
process ids
terminal capabilities
modes
recent events
```

### 22.4 `wmux dump-layout`

Very important.

Example output:

```text
Window 1 active_pane=3 size=100x30
TopBottom weights=[1,1]
├── LeftRight weights=[1,1]
│   ├── Pane 1 rect=(0,0 49x13)
│   └── Pane 2 rect=(50,0 50x13)
└── Pane 3 rect=(0,14 100x14) active
```

This will make layout bugs debuggable.

### 22.5 Recent Event Ring

Keep bounded recent events:

```text
last 1000 commands
last 1000 key events, privacy-safe
last 1000 process lifecycle events
last 1000 resize/layout events
last 1000 errors
```

Expose through:

```bash
wmux dump-events
```

---

## 23. Performance and Resource Limits

### 23.1 Limits

Define:

```text
max sessions
max windows per session
max panes per window
max scrollback lines
max paste buffer size
max IPC frame size
max client output queue
max pane pending bytes
max log file size
```

### 23.2 High Output

Test commands:

```powershell
1..100000 | ForEach-Object { Write-Output $_ }
```

```cmd
for /L %i in (1,1,100000) do @echo %i
```

Python spammer:

```python
for i in range(1000000):
    print(i)
```

Expect:

```text
input remains responsive
memory bounded
render coalesces
daemon does not freeze
client can detach
pane can be killed
```

### 23.3 Locking Rules

```text
No locks held while writing to terminal/client pipes.
No locks held while writing to ConPTY.
No locks held while waiting on child process.
No locks held while doing clipboard IO.
No locks held during long file/log writes.
```

---

## 24. Attach / Detach Reliability

### 24.1 Attach Semantics

```text
client connects
detect capabilities
enter terminal modes
attach to session
render full frame
start sending input/output
```

### 24.2 Detach Semantics

```text
client detaches intentionally
restore terminal state
client removed
session keeps running
processes keep running
```

### 24.3 Crash/Disconnect Semantics

```text
client pipe dies
mark client disconnected
restore if possible
do not kill session
do not kill panes
session remains attachable
```

### 24.4 Reattach

On reattach:

```text
restore active session/window/pane
recompute layout for new client size
render full frame
ConPTY panes survive
long-running commands survive
```

### 24.5 Tests

```text
attach/detach 100 times
detach during high output
detach during copy mode
detach during resize
detach during shell spawn
reattach after long-running command
multiple clients attach to same session later
```

---

## 25. Window and Pane Semantics

### 25.1 Window Commands

Required:

```text
Ctrl+b c -> create window and switch to it
Ctrl+b n -> next window
Ctrl+b p -> previous window
status line shows active window
window shell state remains independent
```

### 25.2 Pane Kill

Required:

```text
Ctrl+b x -> kill active pane
if last pane in multi-window session, kill active window
if last pane of last window, refuse or require confirmation
cleanup child process reliably
status message every time
```

### 25.3 Pane Selection

Required:

```text
click-to-focus later
keyboard select pane later
active pane visually obvious
active pane stable across redraw/resize
```

---

## 26. Reference tmux Semantics

Create:

```text
docs/reference-tmux.md
```

Track which tmux behaviors wmux copies.

Sections:

```text
prefix behavior
new window
next/previous window
pane split
pane kill
spread/equalize panes
copy mode entry/exit
copy selection
paste buffer
mouse click focus
mouse drag resize
status line
detach/reattach
```

For each:

```text
tmux behavior
wmux intended behavior
intentional differences
test coverage
```

This prevents Codex from guessing semantics.

---

## 27. Testing Strategy

### 27.1 Unit Tests

```text
layout tree
target resolver
command parser/engine
key decoder
mode transitions
VT parser
screen grid mutation
Unicode width
copy selection extraction
IPC framing
config parsing
```

### 27.2 Integration Tests

```text
spawn shell in ConPTY
send input
read output
split panes
resize panes
kill panes
create windows
switch windows
attach/detach
copy/paste
```

### 27.3 Golden Tests

```text
VT sequence -> expected screen grid
layout tree -> expected rects
input bytes -> expected key events
command sequence -> expected state snapshot
```

### 27.4 Property Tests

Useful for layout:

```text
random split/kill/equalize/resize
assert tree invariants
assert rects valid
assert no duplicate PaneId
assert no orphan layout nodes
assert active pane valid
```

---

## 28. Stress Suite

Create:

```text
scripts/stress/
```

Scripts:

```text
create/kill 100 sessions
create/kill 1000 panes
split/kill panes repeatedly
attach/detach loops
high-output panes
resize storms
copy/paste loops
malformed IPC
daemon restart
shell spawn failure
mouse event flood
Unicode output flood
```

Expected:

```text
no orphan processes
no unbounded memory growth
no handle leaks
daemon remains responsive
terminal recovers
logs explain failures
```

---

## 29. Soak Suite

Create:

```text
scripts/soak/
```

Run:

```text
1 hour
4 hours
overnight
```

Track:

```text
memory
CPU
handle count
process count
event queue depth
render latency
client output queue depth
dropped/coalesced frames
errors/warnings
orphan processes
```

Pass criteria:

```text
memory bounded
CPU reasonable when idle
handle count stable
no orphan shells
no stuck clients
no unbounded logs
```

---

## 30. Release Gate

Do not call it broadly usable until:

```text
No orphan powershell.exe/pwsh.exe/cmd.exe/conhost.exe after stress.
Memory bounded under high output.
Handle count stable under create/kill loops.
Terminal always restores after detach/crash/reset.
Attach/detach works repeatedly.
Panes/windows survive long workloads.
Resize storms do not crash/freeze.
Basic TUI apps remain usable after split/resize/attach/detach.
Known limitations documented.
wmux doctor works.
wmux dump-state works.
wmux reset-terminal works.
```

### Measurable TUI Gate

Test these manually or semi-automated:

```text
PowerShell prompt
cmd prompt
git log / pager
Python REPL
vim or nvim if installed
Codex-like TUI if available
```

Expected:

```text
display remains usable
no border corruption
status line intact
resize does not permanently corrupt output
detach/reattach restores usable screen
```

---

## 31. Suggested Implementation Order

Do not implement everything randomly. Build in dependency order.

### Phase 1 — Core Data Model

```text
1. Architecture contract
2. Session/window/pane IDs
3. N-child weighted layout tree
4. Layout invariants
5. Rect computation
6. split/kill/equalize layout tests
```

### Phase 2 — Event and Command Core

```text
1. Daemon event loop
2. Command enum
3. Target resolver
4. Keybind -> command pipeline
5. Status messages
6. Logging with request IDs
```

### Phase 3 — ConPTY and Process Lifecycle

```text
1. Pane spawn
2. Output reader events
3. Input writer path
4. ResizePseudoConsole after layout
5. Idempotent kill
6. Process-tree cleanup
7. Orphan process tests
```

### Phase 4 — Runtime Usability

```text
1. Ctrl+b c/n/p/x/E
2. Active window/pane status
3. Attach/detach basics
4. Terminal cleanup RAII
5. wmux reset-terminal
6. wmux doctor
```

### Phase 5 — Terminal Compatibility Layer

```text
1. TerminalCapabilities
2. Windows Terminal profile
3. VSCode profile
4. Config overrides
5. mouse/bracketed-paste/cursor capability checks
```

### Phase 6 — VT/Grid/Rendering Hardening

```text
1. Separate parser/grid/renderer layers
2. Golden VT tests
3. dirty redraws
4. render coalescing
5. backpressure limits
6. resize storm tests
```

### Phase 7 — Copy/Paste/Mouse

```text
1. mode system
2. copy mode
3. selection
4. copy extraction
5. paste buffer
6. Windows clipboard async integration
7. mouse click/drag/wheel
```

### Phase 8 — Stress, Soak, Release

```text
1. stress scripts
2. soak scripts
3. dump-state
4. dump-layout
5. performance metrics
6. release gate
```

---

## 32. AGENTS.md / Codex Rules

Put this in `AGENTS.md` or equivalent:

```text
wmux is a daemon-owned state machine.

Rules:
1. Do not mutate session/window/pane/layout/client state outside the daemon event loop.
2. Do not add direct state changes in PTY readers, IPC handlers, renderers, or input decoders.
3. All user actions must become Command enum values.
4. All commands must resolve targets through the target resolver.
5. Layout tree must preserve invariants:
   - N-child weighted split groups
   - no same-axis nested split groups
   - every pane has one leaf
   - every split has 2+ children
   - weights match children
6. Computed rectangles are derived state, not source of truth.
7. Resize ConPTY only after final pane body size is known.
8. Never hold daemon locks while writing to client pipes, ConPTY pipes, clipboard, or logs.
9. All process/handle cleanup must be idempotent.
10. Every user-visible failure should produce a status message and a useful log entry.
11. Add tests for layout, command, IPC, input, and lifecycle changes.
12. Do not paper over terminal bugs with host-specific hacks unless routed through TerminalCapabilities/TerminalQuirk.
```

---

## 33. Reference Sources

Useful references for implementation research:

```text
tmux man page:
https://man7.org/linux/man-pages/man1/tmux.1.html

tmux layout source:
https://github.com/tmux/tmux/blob/master/layout.c

tmux key bindings:
https://github.com/tmux/tmux/blob/master/key-bindings.c

tmux command select-layout:
https://github.com/tmux/tmux/blob/master/cmd-select-layout.c

tmux terminal features:
https://github.com/tmux/tmux/blob/master/tty-features.c

tmux terminal key handling:
https://github.com/tmux/tmux/blob/master/tty-keys.c

Microsoft ConPTY session docs:
https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session

Microsoft ResizePseudoConsole:
https://learn.microsoft.com/en-us/windows/console/resizepseudoconsole
```

---

## Final Summary

The most important robustness pillars are:

```text
1. Daemon event loop is the only state mutator.
2. Layout tree has strong invariants.
3. Commands are explicit and target-resolved.
4. Terminal compatibility is capability-based.
5. ConPTY lifecycle is RAII/idempotent.
6. Parser/grid/renderer are separate layers.
7. Client IO and pane IO never block daemon state.
8. Diagnostics are first-class.
9. Stress/soak tests define release readiness.
```

If these are respected, `wmux` can grow toward tmux-grade robustness instead of becoming a pile of terminal-specific patches.
