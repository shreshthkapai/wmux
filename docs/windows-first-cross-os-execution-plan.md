# Windows-First Cross-OS tmux-Equivalent Execution Plan

This plan describes how to build a native Windows-first, Rust-based terminal multiplexer with tmux-level semantics, performance, robustness, and later cross-OS support.

The implementation strategy is:

```text
Build Windows first.
Design the core as cross-OS from day one.
Keep all platform APIs behind backend interfaces.
Do not let Windows types leak into core.
Do not build the Unix backend fully until the Windows backend proves the abstraction.
```

The intended final product is a full tmux-equivalent multiplexer:

- persistent server
- disposable clients
- attach/detach
- sessions
- windows
- panes
- split/resize/select layouts
- scrollback
- copy mode
- paste buffers
- command language
- key tables
- status line
- formats
- hooks
- control mode
- robust child process lifecycle
- fast redraw
- native Windows backend
- later Unix backend

## Non-Negotiable Architecture Rules

These rules apply from the first commit.

1. Core code must not import Windows APIs.
2. Core code must not import Unix APIs.
3. Core objects use stable IDs, not raw platform handles.
4. The server is the only authority for sessions, windows, panes, grids, layouts, options, and clients.
5. Clients are disposable. Losing a client must not destroy pane processes unless explicitly requested.
6. A pane is a virtual terminal, not just a child process.
7. Child output is parsed into an internal grid. The attached terminal is only a renderer.
8. Commands mutate state through a serialized server command queue.
9. IPC is versioned from the start.
10. Every platform mechanism has an OS-neutral semantic event in the core.
11. Windows first means Windows backend first, not Windows-only architecture.

## Repository Shape

Use a Rust workspace. The names can change, but the ownership boundaries should not.

```text
crates/
  mux-core/
    src/
      ids.rs
      server_state.rs
      sessions.rs
      windows.rs
      panes.rs
      layouts.rs
      grid.rs
      screen.rs
      vt_parser.rs
      screen_write.rs
      redraw.rs
      keys.rs
      commands/
      options/
      formats/
      status.rs
      paste.rs
      hooks.rs
      modes/
      control.rs

  mux-protocol/
    src/
      lib.rs
      version.rs
      frame.rs
      messages.rs
      codec.rs

  mux-platform/
    src/
      lib.rs
      pty.rs
      process.rs
      ipc.rs
      terminal.rs
      filesystem.rs
      credentials.rs
      clock.rs

  mux-platform-windows/
    src/
      lib.rs
      conpty.rs
      process.rs
      job.rs
      named_pipe.rs
      console.rs
      terminal_features.rs
      overlapped.rs

  mux-platform-unix/
    src/
      lib.rs
      pty.rs
      process.rs
      ipc.rs
      terminal.rs
      signals.rs

  mux-server/
    src/main.rs

  mux-client/
    src/main.rs

  mux-cli/
    src/lib.rs

tests/
  fixtures/
  golden/
  stress/
```

## Core Data Model

Use stable IDs.

```rust
struct SessionId(u32);
struct WindowId(u32);
struct WinlinkId(u32);
struct PaneId(u32);
struct ClientId(u32);
struct JobId(u32);
struct PasteBufferId(u32);
```

The server state should look conceptually like this:

```rust
struct ServerState {
    sessions: Store<SessionId, Session>,
    windows: Store<WindowId, Window>,
    winlinks: Store<WinlinkId, Winlink>,
    panes: Store<PaneId, Pane>,
    clients: Store<ClientId, Client>,
    paste_buffers: PasteBufferStore,
    options: GlobalOptions,
    key_tables: KeyTables,
    command_queues: CommandQueues,
    jobs: Store<JobId, Job>,
}
```

Do not create cyclic Rust ownership graphs. IDs plus stores are simpler, safer, and easier to serialize/debug/test.

## Platform Interfaces

Define traits early, but keep them minimal. Expand only when Windows implementation proves the need.

### PTY/Pane Backend

The core needs this semantic interface:

```rust
trait PtyBackend {
    fn spawn_pane(&mut self, request: SpawnPaneRequest) -> Result<SpawnedPane>;
    fn write_pane_input(&mut self, pane: BackendPaneId, bytes: &[u8]) -> Result<()>;
    fn resize_pane(&mut self, pane: BackendPaneId, size: TerminalSize) -> Result<()>;
    fn close_pane_input(&mut self, pane: BackendPaneId) -> Result<()>;
    fn terminate_pane(&mut self, pane: BackendPaneId, mode: TerminateMode) -> Result<()>;
}
```

The backend emits events:

```rust
enum PtyEvent {
    Output { pane: PaneId, bytes: Bytes },
    Exited { pane: PaneId, status: ExitStatus },
    Closed { pane: PaneId },
    Error { pane: PaneId, error: BackendError },
}
```

Windows implementation:

- `CreatePseudoConsole`
- pipes for ConPTY input/output
- `CreateProcessW` with pseudo console attribute
- process handle wait for exit
- Job Object per pane
- `ResizePseudoConsole`
- overlapped IO or async pipe integration

Unix implementation later:

- `openpty` or equivalent
- fork/exec
- PTY master reads/writes
- ioctl resize
- SIGCHLD handling

### IPC Backend

Core semantic interface:

```rust
trait IpcBackend {
    fn listen(&mut self, endpoint: ServerEndpoint) -> Result<Listener>;
    fn connect(&mut self, endpoint: ServerEndpoint) -> Result<Connection>;
    fn send(&mut self, connection: ConnectionId, message: ProtocolMessage) -> Result<()>;
    fn close(&mut self, connection: ConnectionId) -> Result<()>;
}
```

Events:

```rust
enum IpcEvent {
    ClientConnected { connection: ConnectionId, peer: PeerIdentity },
    Message { connection: ConnectionId, message: ProtocolMessage },
    Disconnected { connection: ConnectionId },
    Error { connection: Option<ConnectionId>, error: BackendError },
}
```

Windows first:

- named pipes
- per-user pipe namespace
- secure DACL
- peer identity using token/SID checks where possible
- framed protocol over byte stream

Unix later:

- AF_UNIX socket
- filesystem permissions
- peer uid/gid

### Terminal Client Backend

Semantic interface:

```rust
trait TerminalClientBackend {
    fn open_current_terminal(&mut self) -> Result<TerminalClientHandle>;
    fn enter_raw_mode(&mut self, handle: TerminalClientHandle) -> Result<()>;
    fn restore_terminal(&mut self, handle: TerminalClientHandle) -> Result<()>;
    fn write_output(&mut self, handle: TerminalClientHandle, bytes: &[u8]) -> Result<()>;
    fn query_size(&mut self, handle: TerminalClientHandle) -> Result<TerminalSize>;
}
```

Events:

```rust
enum TerminalEvent {
    Input { client: ClientId, bytes: Bytes },
    Resized { client: ClientId, size: TerminalSize },
    Closed { client: ClientId },
}
```

Windows first:

- enable virtual terminal processing
- enable virtual terminal input
- save/restore console modes
- handle Windows Terminal/conhost differences
- query terminal size
- produce input bytes compatible with the core key parser

Unix later:

- termios raw mode
- tty fd
- SIGWINCH

## Protocol Requirements

Create `mux-protocol` before the server is useful. Never use ad hoc JSON for hot IPC.

Protocol must include:

- protocol version
- handshake
- client identify messages
- command request
- attach request
- detach request
- resize event
- terminal input
- terminal output
- exit/shutdown
- error response
- file read/write stream later
- control-mode notifications later

Initial message shape:

```rust
enum ProtocolMessage {
    Hello { version: u32, client_pid: u32 },
    HelloOk { version: u32, server_pid: u32 },
    HelloError { server_version: u32, message: String },
    Identify(IdentifyMessage),
    Command(CommandRequest),
    Attach(AttachRequest),
    Detach(DetachRequest),
    Resize { cols: u16, rows: u16, xpixel: u16, ypixel: u16 },
    Input { bytes: Bytes },
    Output { bytes: Bytes },
    Exit { code: i32, message: Option<String> },
    Shutdown,
}
```

Framing:

- magic bytes
- protocol version
- message type
- payload length
- payload bytes
- max frame size

Acceptance criteria:

- protocol rejects unknown major version
- protocol rejects oversized frames
- protocol rejects malformed payloads
- protocol has round-trip tests for every message
- protocol fuzz target exists by Phase 6

## Execution Phases

Each phase should end with a working, testable product state. Do not move to later features until the phase acceptance criteria pass.

## Phase 0: Project Foundation

Goal: create the workspace, coding standards, build/test pipeline, and architectural boundaries.

Build:

- Rust workspace with the crate layout above.
- `mux-core` with ID types and empty state stores.
- `mux-platform` with initial traits.
- `mux-protocol` with version constants and frame skeleton.
- `mux-server` and `mux-client` binaries that compile.
- Logging/tracing setup.
- Error type strategy.
- Basic CI commands:
  - `cargo fmt`
  - `cargo clippy`
  - `cargo test`

Decisions:

- async runtime: choose one runtime and commit.
- serialization: choose a binary encoding or write explicit codec.
- buffer type: choose a bytes buffer type.
- ID store: choose slotmap/generational arena/custom store.

Acceptance criteria:

- clean build on Windows
- all crates compile
- core crate has no Windows imports
- protocol crate has no Windows imports
- platform-windows is the only crate using Windows APIs
- basic unit tests pass

## Phase 1: Minimal Client/Server IPC

Goal: client connects to server and exchanges versioned messages.

Build:

- server starts and listens on a per-user Windows named pipe.
- client locates default endpoint.
- client sends `Hello`.
- server replies `HelloOk`.
- client can send a simple `Command`.
- server returns success/error.
- server handles disconnect.
- client prints server response.

Required commands:

- `server status`
- `server stop`
- `list-clients`

Windows details:

- pipe path must be user-scoped.
- pipe must reject other users by default.
- server startup lock prevents two servers for same endpoint.
- stale endpoint handling exists.

Acceptance criteria:

- `wmux server status` reports no server when none exists.
- `wmux new-session` can start server implicitly later, but for this phase explicit server start is enough.
- multiple clients can connect and disconnect.
- protocol version mismatch produces a useful error.
- named pipe security is tested manually with another user where possible.

## Phase 2: Server Runtime and Command Queue

Goal: introduce tmux-like server state mutation through a serialized command queue.

Build:

- authoritative `ServerState`.
- `Client` object for each IPC connection.
- command queue per client.
- global command scheduler.
- command result reporting.
- target-less commands.
- server shutdown path.
- structured logs around command execution.

Initial commands:

- `list-clients`
- `display-message`
- `kill-server`
- `show-messages`

Acceptance criteria:

- commands execute in deterministic order.
- command errors do not crash server.
- one bad client cannot poison other clients.
- server shuts down cleanly after `kill-server`.
- command parser is separated from command execution.

## Phase 3: Windows ConPTY Pane Prototype

Goal: spawn one shell through ConPTY and move bytes between pane and client.

Build:

- Windows ConPTY wrapper.
- process creation with ConPTY attached.
- pipe read/write loop.
- pane process handle tracking.
- Job Object per pane.
- pane exit event.
- client terminal raw/VT mode.
- attach client input to pane input.
- attach pane output to client output.
- resize ConPTY from client size.

Initial user flow:

```text
wmux new-session
```

Expected:

- starts server if needed
- creates one session
- creates one window
- creates one pane
- spawns default shell
- attaches current terminal
- user can type commands
- output appears
- detach key can return client to shell while pane continues

Windows requirements:

- default shell configurable, initial default can be PowerShell.
- console modes restored on client exit.
- pane process tree killed when pane explicitly killed.
- process not killed on client detach.

Acceptance criteria:

- interactive shell works in Windows Terminal.
- resize updates ConPTY.
- detach leaves pane running.
- reattach reconnects to same pane.
- killing server kills pane job.
- client crash does not kill pane.

Manual verification for this phase:

```powershell
cargo build --workspace
.\target\debug\wmux.exe new-session
```

Inside the attached shell, type a normal command and confirm output appears.
Press `Ctrl-B`, then `d`; the client should detach and return to the original
PowerShell prompt while the pane process remains server-owned. Reattach with:

```powershell
.\target\debug\wmux.exe attach-session
```

Phase 3 is still a byte-passthrough ConPTY prototype. It must reconnect to the
same live pane, but redraw-from-history after reattach is deliberately Phase 4,
where pane output becomes server-owned screen/grid state.

## Phase 4: Internal Screen Grid and VT Parser

Goal: stop treating pane output as direct passthrough. Parse output into internal terminal state.

Build:

- `GridCell`
- `GridLine`
- `Grid`
- `Screen`
- cursor state
- scroll region
- basic attributes
- UTF-8 decoding
- printable character handling
- CR/LF/BS/TAB
- clear screen/line
- cursor movement
- SGR attributes
- alternate screen
- scrollback
- parser state machine

Pipeline:

```text
ConPTY bytes -> vt_parser -> screen_write -> pane screen/grid
```

For this phase, renderer may still do full-screen redraw from grid.

Acceptance criteria:

- simple shell output appears correctly.
- cursor movement works.
- full-screen apps partially work.
- scrollback is stored in server.
- detach/attach redraws from grid state, not buffered terminal output.
- parser has unit tests for common sequences.

## Phase 5: Renderer and Redraw Diffing

Goal: make rendering fast and tmux-like.

Build:

- client-side virtual terminal state.
- full redraw path from grid to terminal.
- dirty line/dirty region tracking.
- minimal cursor movement.
- SGR diffing.
- clear-to-end optimizations.
- batched writes.
- output backpressure.
- slow-client handling.
- redraw flags on client/window/pane.

Renderer pipeline:

```text
pane grid + layout + status + overlays
  -> desired client scene
  -> diff with client render state
  -> VT output bytes
  -> terminal client backend
```

Acceptance criteria:

- no full redraw on every byte.
- large output remains responsive.
- slow client does not block pane parser forever.
- rapid output does not grow buffers unboundedly.
- redraw correctness tests exist for simple scenes.
- measured latency is acceptable under large output.

Current implementation note:

- `mux-core` tracks dirty rows in `Screen`.
- `mux-core::redraw` has per-client `RenderState`, full redraw, and diff redraw paths.
- `mux-server` keeps render state per attached client.
- pane output is parsed once into the server-owned screen, then rendered as dirty-line diffs.
- attach, resize, and backpressure fall back to full redraw from server-owned state.
- adjacent output messages are batched before IPC write.
- client output queues are bounded; stale queued redraws are collapsed into a fresh full redraw for slow clients.
- current cells are character-only, so SGR/color diffing is structurally placed in the renderer path but rich cell attributes remain a later parser/grid fidelity upgrade.

## Phase 6: Sessions, Windows, Panes, and Attach/Detach Semantics

Goal: implement tmux's real object model.

Build:

- `Session`
- `Window`
- `Winlink`
- `Pane`
- session creation/destruction
- window creation/destruction
- pane creation/destruction
- current window
- active pane
- last-window stack
- last-pane stack
- attach/detach clients
- multiple clients attached to same session
- client size negotiation
- window size recalculation

Commands:

- `new-session`
- `attach-session`
- `detach-client`
- `list-sessions`
- `list-windows`
- `list-panes`
- `new-window`
- `kill-window`
- `kill-pane`
- `select-window`
- `select-pane`
- `rename-session`
- `rename-window`

Acceptance criteria:

- multiple sessions work.
- multiple clients can attach to same session.
- detach does not kill panes.
- kill-pane kills only that pane's job.
- kill-window kills all pane jobs in that window.
- server exits when configured to exit-empty.
- state transitions are covered by unit tests.

## Phase 7: Layout Engine and Pane Splitting

Goal: implement tmux-like pane layouts.

Build:

- layout cell tree
- left-right split
- top-bottom split
- pane geometry calculation
- pane borders
- active pane border style
- resize-pane
- zoom-pane
- even-horizontal
- even-vertical
- tiled
- main-horizontal
- main-vertical
- custom layout dump/parse later in phase
- pane swap
- pane break/join basics

Commands:

- `split-window`
- `resize-pane`
- `select-layout`
- `next-layout`
- `previous-layout`
- `swap-pane`
- `break-pane`
- `join-pane`
- `rotate-window`
- `display-panes`
- `resize-window`

Acceptance criteria:

- layout is deterministic.
- rapid terminal resize keeps valid pane sizes.
- no pane receives zero/invalid size except defined minimum behavior.
- split/resize behavior matches tmux semantics as closely as possible.
- golden layout tests exist.

## Phase 8: Key Tables, Prefix, and Input Routing

Goal: make interaction tmux-like instead of raw shell passthrough.

Build:

- key code model
- terminal input key parser
- prefix key
- root table
- prefix table
- copy-mode table placeholders
- repeat-time
- command bindings
- send-prefix
- mouse event decoding baseline
- bracketed paste routing
- pane input routing

Commands:

- `bind-key`
- `unbind-key`
- `list-keys`
- `send-keys`
- `send-prefix`
- `switch-client`
- `refresh-client`

Acceptance criteria:

- default prefix works.
- common tmux bindings work.
- keys can trigger command lists.
- unbound keys go to active pane.
- repeatable bindings work.
- paste is not misinterpreted as commands.

## Phase 9: Command Language and Target Resolution

Goal: support tmux command syntax deeply enough for configs and automation.

Build:

- command lexer/parser
- quoting and escaping
- semicolon command chains
- command aliases
- argument parser
- source/target flags
- target resolution subsystem
- client/session/window/pane target syntax
- error reporting
- command queue continuations

Commands to harden:

- all earlier commands through parser
- `if-shell`
- `run-shell`
- `wait-for`
- `command-prompt` placeholder if prompt not ready
- `source-file`

Acceptance criteria:

- command strings and CLI argv use same execution path.
- target resolution is centralized.
- common tmux target forms work.
- parser has golden tests.
- invalid syntax gives useful errors.

## Phase 10: Options, Formats, and Status Line

Goal: implement tmux's configuration and display customization backbone.

Build:

- option table
- server/session/window/pane option scopes
- global/local option inheritance
- user options
- style values
- colour parser
- format tree
- format expansion
- basic modifiers
- status-left/status-right
- window list
- status interval
- status messages
- prompt line reservation

Commands:

- `set-option`
- `show-options`
- `set-window-option` compatibility alias
- `display-message`
- `set-environment`
- `show-environment`

Acceptance criteria:

- options affect existing and new objects correctly.
- status line updates without excessive redraw.
- basic tmux status configs work.
- formats can access client/session/window/pane values.
- option inheritance tests exist.

## Phase 11: Copy Mode and Scrollback UX

Goal: implement one of tmux's most important interactive features.

Build:

- mode stack per pane
- copy-mode screen
- scroll position
- cursor movement
- vi/emacs style movement
- search
- selection
- rectangle selection if desired in this phase
- copy to paste buffer
- copy-pipe later in phase
- mouse wheel scroll in copy mode
- line numbers option later

Commands:

- `copy-mode`
- `send-keys -X ...`
- `capture-pane`
- `copy-pipe`

Acceptance criteria:

- user can enter copy mode, scroll, search, select, copy, paste.
- scrollback remains server-owned.
- copy mode survives client redraw/resize.
- mode key tables work.
- copy-mode behavior has regression tests.

## Phase 12: Paste Buffers and Clipboard Integration

Goal: implement tmux paste buffers and Windows clipboard integration.

Build:

- paste buffer store
- named buffers
- automatic buffers
- buffer ordering
- paste into pane
- load/save buffers
- Windows clipboard adapter
- OSC 52 policy if supporting terminal clipboard

Commands:

- `set-buffer`
- `load-buffer`
- `save-buffer`
- `paste-buffer`
- `list-buffers`
- `delete-buffer`
- `choose-buffer` later if mode tree exists

Acceptance criteria:

- paste buffers behave independently from OS clipboard.
- paste preserves bytes correctly.
- large paste is throttled safely.
- clipboard integration is configurable and safe.

## Phase 13: Config Files, Hooks, and Notifications

Goal: make startup and automation tmux-like.

Build:

- config file discovery
- source-file execution
- hooks as option-backed command lists
- notifications for session/window/pane/client events
- after-hooks
- startup config loading before first session command where appropriate
- error reporting for config load failures

Commands:

- `source-file`
- `set-hook`
- `show-hooks`

Acceptance criteria:

- config can define options and key bindings.
- config errors are visible.
- hooks run in correct order.
- hooks cannot recursively crash server.

## Phase 14: Jobs, Shell Commands, and Async Command Continuations

Goal: support commands that run external commands without blocking the server.

Build:

- job manager
- Windows process job wrapper for non-pane commands
- stdout/stderr capture
- command queue continuation
- shell adapter layer
- timeout/cancel behavior
- format jobs for status line where needed

Commands:

- `run-shell`
- `if-shell`
- `display-popup` later if popup support exists

Acceptance criteria:

- external commands do not block pane IO.
- command results resume the right command queue.
- job output is bounded.
- job process cleanup is reliable.

## Phase 15: Control Mode

Goal: support machine clients and automation integrations.

Build:

- control client type
- structured command replies
- notifications
- pane output subscriptions
- pause-after
- no-output
- subscription filters
- begin/end/error records

Commands/features:

- `-C` control mode equivalent
- control client attach
- pane output forwarding
- control notifications for session/window/pane/client changes

Acceptance criteria:

- external program can drive the server without terminal UI.
- control clients do not corrupt terminal clients.
- pane output subscription is backpressured.
- command responses are deterministic and parseable.

## Phase 16: Advanced Window Modes and UI

Goal: implement tmux's richer internal UI surfaces.

Build:

- mode tree framework
- choose-tree
- choose-client
- buffer picker
- customize mode
- menus
- popups
- command prompt
- confirm-before
- display-menu
- display-panes overlay polish

Commands:

- `choose-tree`
- `choose-client`
- `choose-buffer`
- `customize-mode`
- `display-menu`
- `popup`
- `confirm-before`
- `command-prompt`

Acceptance criteria:

- modes render through same screen/redraw architecture.
- overlays resize correctly.
- menus/popup input does not leak to panes.
- command prompt runs command strings correctly.

## Phase 17: Windows Hardening and Performance Pass

Goal: make the Windows product smooth, fast, and robust before adding Unix.

Hardening:

- ConPTY EOF/exit race tests.
- pipe closure tests.
- rapid resize tests.
- many-pane tests.
- many-client tests.
- slow-client tests.
- server restart/stale pipe tests.
- crash client during output.
- kill pane while output is active.
- kill server while clients attached.
- terminal mode restoration tests/manual verification.
- job object cleanup verification.

Performance:

- profile large output.
- profile full-screen apps.
- profile many panes.
- reduce allocations in parser.
- reduce allocations in renderer.
- tune buffer sizes.
- batch writes.
- avoid per-byte async overhead.
- add counters for redraw bytes, discarded bytes, parse bytes, frame latency.

Acceptance criteria:

- shell feels responsive under large output.
- full-screen terminal apps are usable.
- detach/attach is instant or near-instant.
- no unbounded memory growth under stress.
- process cleanup is reliable.
- server remains stable after repeated stress runs.

## Phase 18: Compatibility Matrix and Behavior Audit

Goal: define exactly what Windows compatibility means.

Test with:

- Windows Terminal
- conhost
- VS Code integrated terminal
- PowerShell 7
- Windows PowerShell
- cmd.exe
- WSL shell if supported
- Git Bash/MSYS2 if supported
- common TUIs:
  - vim/neovim
  - less
  - git interactive commands
  - npm/cargo output
  - PowerShell prompts
  - rich/colored output tools

Create a matrix:

```text
terminal x shell x scenario x result x known differences
```

Acceptance criteria:

- known differences are explicit.
- bugs are separated from unavoidable OS differences.
- defaults are chosen for best Windows Terminal behavior.

## Phase 19: Unix Backend

Goal: add cross-OS support without disturbing the proven core.

Build:

- Unix IPC backend with AF_UNIX sockets.
- Unix terminal backend with termios.
- Unix PTY backend.
- fork/exec process backend.
- signal handling.
- SIGCHLD process exit integration.
- SIGWINCH resize integration.
- peer uid/gid identity.
- Unix default socket path behavior.

Do not rewrite core.

Acceptance criteria:

- core crate remains mostly unchanged.
- Unix backend passes same semantic tests.
- Windows backend still passes.
- platform-specific tests are separate.
- shared command/session/layout/grid/parser tests run on both.

## Phase 20: Cross-OS Consistency Pass

Goal: make Windows and Unix behavior consistent at the semantic layer.

Audit:

- command behavior
- target resolution
- layout output
- options inheritance
- status formats
- key bindings
- copy mode
- paste buffers
- attach/detach
- session/window/pane lifecycle
- control mode

Create:

- shared golden tests
- platform-specific expected-difference tests
- documented compatibility notes

Acceptance criteria:

- semantic tests pass on Windows and Unix.
- platform differences are isolated to backend tests or documented behavior.
- no platform types leak into core.

## Phase 21: Release-Quality Polish

Goal: make the project usable as a serious terminal multiplexer.

Build:

- installer/package story
- default config
- man/help output
- command reference
- config reference
- troubleshooting guide
- logging flags
- crash diagnostics
- upgrade protocol behavior
- server version mismatch handling
- migration behavior for config changes

Acceptance criteria:

- users can install and start without reading source.
- logs can diagnose backend problems.
- protocol mismatch errors are understandable.
- common tmux users can map habits to this mux.

## Quality Gates

Do not call the project tmux-equivalent until all gates pass.

### Functional Gate

- create/attach/detach sessions
- split panes
- resize panes
- run multiple shells
- scrollback and copy mode
- status line
- key bindings
- config file
- command language
- paste buffers
- control mode

### Performance Gate

- large output remains responsive
- many panes remain usable
- redraw is diffed and batched
- memory stays bounded by configured history/buffers
- slow clients do not stall server

### Robustness Gate

- client crash does not kill server
- server cleans pane process trees on explicit kill
- terminal modes restored on client exit
- malformed IPC does not crash server
- malformed terminal bytes do not crash parser
- resize storms do not corrupt layout
- detach/attach loops do not leak resources

### Cross-OS Gate

- same core tests pass on Windows and Unix
- platform differences documented
- backend code isolated
- command/session/window/pane semantics match

## Definition of Done

The project is complete when:

- Windows implementation is native and stable.
- Unix implementation exists behind the same backend traits.
- Core is OS-neutral.
- tmux semantic model is implemented.
- known OS-level differences are documented.
- performance is comparable to tmux for realistic workloads.
- stress tests pass.
- parser fuzzing has run without crashes.
- server/client lifecycle is robust.
- common terminal applications work smoothly.

## Build Order Summary

Follow this order:

1. Foundation and crate boundaries.
2. Versioned IPC.
3. Server runtime and command queue.
4. Windows ConPTY shell prototype.
5. Internal screen/grid/parser.
6. Renderer and redraw diffing.
7. Sessions/windows/panes object model.
8. Layouts and splits.
9. Key tables and prefix.
10. Command parser and target resolution.
11. Options/formats/status.
12. Copy mode.
13. Paste buffers/clipboard.
14. Config/hooks/notifications.
15. Jobs and async shell commands.
16. Control mode.
17. Advanced modes/UI.
18. Windows hardening/performance.
19. Unix backend.
20. Cross-OS consistency.
21. Release polish.

If this order is followed, every phase builds on a stable previous layer and avoids the main failure mode: building a terminal UI before building the persistent virtual terminal server underneath it.
