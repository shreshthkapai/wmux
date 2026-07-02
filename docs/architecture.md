# wmux Architecture

This document is the architecture contract for wmux. It describes the shape the
project is moving toward and the invariants new code must preserve.

## Core Object Model

```text
Daemon
+-- Sessions
|   +-- Windows
|   |   +-- Layout tree
|   |   +-- Panes
|   |       +-- Platform PTY process
|   |       +-- Screen grid
|   |       +-- Alternate screen
|   |       +-- Scrollback ring
|   +-- Session options
+-- Clients
|   +-- Attached session/window/pane
|   +-- Terminal size
|   +-- Terminal capabilities
|   +-- Current mode
|   +-- Render state
+-- Command engine
+-- IPC server
+-- Event loop
+-- Renderer
+-- Logger/diagnostics
+-- Config
```

The daemon owns all persistent runtime objects. Clients render frames, collect
input, and send typed messages. A closed terminal must not imply a killed shell.

## Platform Boundary

wmux is Windows-first, but the core architecture is not Windows-only. Core code
models terminal multiplexing concepts; platform code owns operating-system
mechanics.

Core owns:

```text
sessions/windows/panes as logical objects
layout tree and geometry
command engine and target resolver
input modes, copy mode, paste buffer
scrollback and terminal grid
VT parser and renderer
status line, config/options, diagnostics models
```

The platform layer owns:

```text
PTY creation and process spawning
PTY resize and process cleanup
pipe/file-descriptor servicing
terminal mode setup/restoration
IPC transport
clipboard integration
console-control or signal behavior
```

The current production backend is Windows and uses:

```text
ConPTY
CreateProcess
ResizePseudoConsole
Windows HANDLE/HPCON values
anonymous/named pipes
Job Objects
Windows clipboard APIs
Windows console mode APIs
```

Those details belong under `src/platform` or behind platform-facing interfaces
in `include/wmux/platform`. Core headers and sources must not include
`windows.h` or mention `HANDLE`, `HPCON`, `CreateProcess`, `ResizePseudoConsole`,
Job Object APIs, Windows clipboard APIs, or named-pipe APIs.

Current platform-facing API surface:

```text
include/wmux/platform/services.hpp          PlatformServices backend boundary
include/wmux/platform/pty_process.hpp       PTY/process facade
include/wmux/platform/ipc_transport.hpp     command/attach IPC transport entry points
include/wmux/platform/terminal_control.hpp  local terminal reset/attach mode helpers
include/wmux/platform/clipboard.hpp         clipboard facade
include/wmux/platform/platform_info.hpp     host OS, shell, console, resource probes
include/wmux/platform/pipe_handle.hpp       opaque transport handle identity
```

Core and daemon code should prefer `platform_services()` instead of directly
calling platform free functions. The service surface is deliberately small:

```text
PtyBackend           resolve shell, spawn pane PTY process, report PTY availability
ClipboardBackend     write text and name the clipboard backend
TerminalModeBackend  terminal mode probes, attach/reset sequences, reset-terminal
IpcTransportBackend  command/attach endpoints, request send, daemon startup
PlatformInfoBackend  environment, cwd, OS version, resource snapshots
```

The old top-level `wmux/pty_process.hpp`, `wmux/ipc_transport.hpp`, and
`wmux/terminal_control.hpp` compatibility shims have been removed. New code must
include `wmux/platform/...` headers or use `wmux/platform/services.hpp`.

The boundary is guarded by `tests/platform_boundary_test.cpp`, which scans
`src/` and `include/` and fails if raw Win32 API names or `windows.h` includes
appear outside platform-owned paths.

The build also has explicit library layers:

```text
wmux_core              platform-neutral commands, config, input, layout, VT/grid, IPC framing
wmux_platform_windows  Windows platform backend services and low-level OS integration
wmux_daemon            daemon state, command execution, shell/runtime orchestration, rendering
wmux_app_support       diagnostics and app-facing helpers that consume platform services
wmux_windows_runtime   Windows attach client/server and daemon server wiring
wmux                   CLI executable entry point
```

New source files should be added to the narrowest correct target. Do not add
Windows-specific code to `wmux_core` or duplicate implementation sources in
the executable/test targets.

Correct boundary:

```text
layout engine computes pane body sizes
daemon/runtime asks PtyProcess to resize each pane
platform backend maps that to ResizePseudoConsole on Windows
renderer redraws using terminal capabilities
```

Do not add fake cross-platform abstractions ahead of need. Add a boundary when
Windows details are about to leak into core logic.

## Session / Window / Pane Model

Internal identity is always stable-ID based:

```text
SessionId
WindowId
PaneId
ClientId
NodeId
RequestId
```

Indexes are display metadata only. They are not internal identity and must not
be used for ownership, runtime lookup, or process cleanup.

The current core model is:

```text
Session
+-- id: SessionId
+-- name
+-- windows: ordered WindowId list
+-- active_window_id: WindowId
+-- options
+-- created_at

Window
+-- id: WindowId
+-- session_id: SessionId
+-- index: display index
+-- name
+-- panes: PaneId -> Pane
+-- pane_order: ordered PaneId list
+-- active_pane_id: PaneId
+-- layout_root: NodeId
+-- layout: LayoutArena
+-- options
+-- created_at

Pane
+-- id: PaneId
+-- window_id: WindowId
+-- mode_state
+-- last_exit_code
+-- created_at
```

`PaneNode` is a summary/snapshot type only. It is used by rendering helpers,
tests, and external summaries, but it is not the authoritative layout owner.
The source of truth is:

```text
LayoutArena
+-- nodes: NodeId -> LayoutNode
+-- pane_leaf_index: PaneId -> NodeId
+-- root_id: NodeId
+-- next_node_id
```

Layout nodes store parent links, so local-first operations such as spread /
equalize can walk from the active pane leaf toward the root without relying on
recursive value-tree paths.

Process/runtime state lives with the daemon runtime:

```text
SessionRuntime
+-- WindowRuntime
    +-- PaneRuntime
        +-- PtyProcess
```

Attached clients carry explicit client state:

```text
Client
+-- id: ClientId
+-- attached_session: SessionId?
+-- active_window: WindowId?
+-- active_pane: PaneId?
+-- terminal_caps
+-- mode
+-- size
+-- render_state
```

The active-object rules are:

```text
Each client has at most one attached session.
Each session has one active window.
Each window has one active pane.
Killing a pane chooses a replacement pane before deleting the old pane.
Killing a window chooses a replacement window before returning to callers.
Operations must never leave active IDs dangling.
User-visible active transitions should be logged and reflected in status output.
```

## Runtime Rule

Only the daemon event loop mutates session, window, pane, layout, process, and
client state. Everything else enqueues typed events.

Allowed event producers:

```text
PTY readers    -> PaneOutput events
IPC handlers   -> ClientCommand / AttachInput events
Resize watcher -> ClientResize events
Timers         -> Timer events
Client reader  -> ClientInput events
```

Forbidden:

```text
PTY reader directly changing active pane
IPC handler directly killing panes
Renderer directly mutating layout
Input decoder directly mutating sessions/windows
Any random thread/task touching daemon-owned state
```

The current codebase already has a daemon event-loop wrapper, but some legacy
paths still rely on the daemon state mutex. New work should move mutation toward
the event loop instead of adding more direct shared-state access.

## Daemon Event Model

Daemon-owned state is mutated by typed events. The C++ model is:

```text
DaemonEvent
+-- AttachStart(IpcRequest)
+-- ClientConnected(ClientId)
+-- ClientDisconnected(ClientId)
+-- ClientInput(ClientId, SessionId, bytes)
+-- DecodedKey(ClientId, key)
+-- MouseEvent(ClientId, event)
+-- MouseFocus(ClientId, focus)
+-- AttachCommand(ClientId, SessionId, command)
+-- CommandModeCommand(ClientId, SessionId, command)
+-- Paste(ClientId, SessionId)
+-- PasteBufferSet(text)
+-- IpcCommand(ClientId?, RequestId, IpcRequest)
+-- PaneOutput(PaneId, bytes)
+-- PaneExited(PaneId, status)
+-- ClientResize(ClientId, cols, rows)
+-- Timer(TimerEvent)
+-- Shutdown
```

The event loop executes one event at a time and installs a debug mutation guard
while each event is running. Code that mutates daemon-owned state should assert
that the guard is active.

Current implementation note:

```text
Command IPC is routed through DaemonEvent::IpcCommand.
Attach start/end, resize, command frames, command-mode dispatch, mouse
focus/drag, paste, paste-buffer updates, and normal input routing are typed
daemon events.
PTY output polling and render batching remain a separate performance-sensitive
pipeline; do not force every PTY byte through the daemon event queue without a
batching/backpressure design.
```

Ordering rules are intentionally defensive:

```text
PaneOutput may arrive after PaneExited.
ClientInput may arrive after ClientDisconnected.
Resize may arrive while layout work is pending.
Timer events may fire after object deletion.
```

Handlers must treat stale IDs as normal:

```text
if target pane/client/window does not exist:
  log at debug level
  drop event safely
```

## IPC Protocol

wmux uses separate Windows named-pipe endpoints for command/control IPC and
interactive attach traffic.

Command/control IPC uses a strict binary frame around the JSON payload:

```text
magic:      "WMUX"
version:    uint16 little-endian
kind:       Control | AttachInput | AttachOutput | Event | Error
request_id: uint64 little-endian
length:     uint32 little-endian
payload:    bounded bytes
```

Rules:

```text
Reject bad magic.
Reject unsupported protocol versions.
Reject unknown frame kinds.
Reject oversized payloads before allocation.
Reject truncated payloads cleanly.
Return responses with the same request_id.
Log request_id for command/control IPC.
```

Windows pipe names are user-specific. The server also applies a same-user pipe
security descriptor where Windows accepts it; if ACL setup fails, startup
continues with the process token default DACL and logs the degradation.

Implementation note:

```text
Command IPC uses short-lived WMUX Control/Error frames.
The attach endpoint is separate and starts with a WMUX Control AttachStart frame.
Client-to-daemon attach traffic uses WMUX AttachInput frames carrying bounded
typed attach payloads. Daemon-to-client rendered terminal traffic uses WMUX
AttachOutput frames. Attach errors use WMUX Error frames.
```

## Command Engine And Target Resolution

All user actions should converge on runtime commands:

```text
raw input bytes
-> key decoder
-> mode handler
-> key binding table
-> RuntimeCommand
-> TargetResolver
-> command executor
-> state mutation
-> status message / log / redraw request
```

The C++ command model lives in `wmux/command_engine.hpp`. The daemon-facing
result and resolver live in `daemon_command_engine.hpp`.

Command targets are semantic:

```text
Current
Session(SessionId)
Window(WindowId)
Pane(PaneId)
Client(ClientId)
MousePosition(ClientId, x, y)
Named(string)
```

Resolution returns stable IDs:

```text
ResolvedTarget
+-- session_id
+-- window_id
+-- pane_id?
+-- client_id?
```

Command executors must not manually search by names, display indexes, mouse
coordinates, or cached active IDs. The target resolver owns:

```text
current client -> active session/window/pane
mouse position -> pane under cursor
session/window/pane ID validation
session/window name resolution
dead/stale target checks
ambiguous target errors
```

Current implementation note:

```text
The runtime command and target model exists. Resolver tests cover current,
explicit session/window/pane, named session/window, mouse-position, and stale
targets.

Interactive attach keybind commands and command-mode commands are mapped to
RuntimeCommand and resolved through TargetResolver before mutation.
Short-lived command IPC mutation requests also construct RuntimeCommand values
before dispatch; query/admin requests remain separate because they do not map
cleanly to user action commands.
```

## Why This Rule Exists

A terminal multiplexer has many concurrent pressure points:

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

If these subsystems mutate state independently, crashes and rare layout
corruption become inevitable. The daemon must remain the single serialized
authority.

## Layout Contract

Layout is tree state, not scattered pane rectangles.

```text
LayoutNode
+-- Pane leaf: PaneId
+-- Split group:
    +-- axis: LeftRight or TopBottom
    +-- children: two or more nodes
    +-- weights: one positive finite weight per child
```

Required invariants:

```text
Every pane has exactly one leaf node.
Every leaf has exactly one PaneId.
Every split has at least two children.
weights.size() == children.size().
All weights are positive finite numbers.
No split node directly contains a same-axis split node.
Layout rectangles are derived, not source of truth.
Pane body rectangles exclude borders and the status line.
Minimum pane size is checked before ConPTY resize.
Killing a pane removes its leaf and normalizes the tree.
One-child splits collapse.
Adjacent same-axis splits flatten.
```

Same-axis splits are normalized:

```text
Bad:
LeftRight
+-- Pane A
+-- LeftRight
    +-- Pane B
    +-- Pane C

Good:
LeftRight
+-- Pane A
+-- Pane B
+-- Pane C
```

Opposite-axis nesting is valid:

```text
LeftRight
+-- Pane A
+-- TopBottom
    +-- Pane B
    +-- Pane C
```

## Geometry Pipeline

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

The layout tree stores structure and weights. Computed rectangles are derived
state. ConPTY sizes come from pane body rectangles, not outer split rectangles.

## Spread / Equalize Semantics

The internal command is semantic:

```text
SpreadPanesEvenly
```

It is not modeled as separate horizontal and vertical commands. The active
pane determines the nearest meaningful split group:

```text
1. Start at the active pane leaf.
2. Move to the parent split group.
3. Set that group's child weights to 1.0.
4. Recompute rectangles.
5. If any visual size changed, stop.
6. If nothing changed, climb to the parent split group.
7. Repeat until root.
8. If no group changes, report that panes are already evenly spread.
```

This mirrors tmux-like local-first spread behavior while preserving subtree
shape.

## Terminal Boundaries

wmux does not own terminal emulation. The host terminal owns final rendering.
wmux owns:

```text
ConPTY process lifecycle
terminal grid model
scrollback model
copy/paste model
layout/render frame generation
input decoding and command dispatch
```

Every host terminal assumption must go through the terminal capability layer.
Do not hardcode Windows Terminal, VSCode, WezTerm, Alacritty, or conhost quirks
inside input, mouse, renderer, or clipboard code.

## Terminal Screen Pipeline

ConPTY output must move through explicit layers:

```text
bytes from ConPTY
-> TerminalVtParser
-> TerminalVtOperation values
-> TerminalGrid mutation
-> TerminalScreenSnapshot / TerminalScrollbackSnapshot
-> daemon renderer
-> attach output frame
```

Do not combine byte parsing, grid mutation, and rendering in one function. The
parser owns VT byte framing, UTF-8 decoding, CSI/OSC framing, and safe unknown
sequence handling. The grid owns terminal state:

```text
cursor position
cursor visibility/style
normal and alternate screen buffers
scrollback ring
scroll region
origin and wrap modes
current SGR attributes
terminal title
```

Unknown or oversized terminal sequences are ignored safely and counted/logged
without writing raw control bytes into the visible grid.

## Rendering And Backpressure

Rendering is a coalesced frame pipeline:

```text
PTY output observed
-> dirty pane set updated
-> output redraw throttled to the frame interval
-> renderer builds full or partial frame
-> bounded client output queue
-> dedicated client writer thread
```

The attach input loop does not write rendered output directly. Rendered frames
are queued behind a bounded per-client output queue. The writer thread is the
only code that writes attach output frames to the client pipe, so slow clients
do not block pane-output polling, input handling, layout state mutation, or
frame construction.

Current render dirtiness is pane-level:

```text
dirty full screen
dirty pane body
dirty borders/status/copy overlay through full redraw
```

Pane output uses partial frames when only pane bodies are dirty. Layout changes,
resize, scroll, copy mode, status messages, and command results still force full
frames because borders/status/copy overlays must remain coherent.

Backpressure policy:

```text
PTY bytes are not dropped for slow clients.
Old rendered frames are not blindly dropped if that could lose a dirty pane.
High-frequency pane output is coalesced before rendering.
Client output queues are bounded by bytes and frame count.
A client that exceeds those bounds is treated as slow and detached/closed.
```

Metrics are exposed through `wmux server status`:

```text
pending pane output bytes
pending client output bytes
peak pending bytes
frame counts by full/partial
coalesced output events
frame render time
slow client count
write failures
```

Windows attach-stream writes use overlapped I/O on the server pipe and remain
isolated on a writer thread. This gives the daemon a cancellable output path for
detach, shutdown, pipe failure, and slow-client handling without letting client
pipe writes block the daemon event loop or pane-output polling. The remaining
renderer optimization gap is cell/region diffing; current dirtiness is still
pane-level.

## Unicode Width Policy

Unicode cell width is core terminal-grid behavior and must stay platform-neutral.
The current v1 policy is deliberately conservative:

```text
ASCII printable characters: width 1
CJK/fullwidth ranges: width 2
emoji presentation ranges: width 2 fallback
combining marks: width 0, appended to the previous visible cell
emoji modifiers and tag characters: width 0, appended to the active grapheme
variation selectors: width 0
zero-width joiner/non-joiner: width 0, joins the surrounding grapheme when possible
regional-indicator pairs: grouped as one width-2 terminal grapheme
ambiguous-width characters: width 1
invalid UTF-8: replacement fallback, width 1
```

Grid rules:

```text
A wide glyph occupies a leading cell and a continuation cell.
Continuation cells are explicit metadata, not inferred from empty strings.
Cursor movement should avoid landing on continuation cells.
Erasing or overwriting either half of a wide glyph clears the whole glyph.
Copy extraction emits logical glyph text once, never continuation-cell artifacts.
Selection rendering expands over the full wide glyph where possible.
ZWJ emoji sequences, emoji modifier sequences, and regional-indicator flag pairs
are stored as one logical terminal grapheme where the stream provides enough
context to join them.
```

Known limitation:

```text
The current policy is UAX #29-inspired but does not ship the full Unicode
property database. It covers the terminal-critical cases above defensively.
Exact future Unicode-version parity should replace the helper internals without
changing layout, copy, or render callers.
```

## Copy, Scrollback, Paste, And Clipboard

Each pane owns bounded terminal history:

```text
TerminalGrid
-> normal screen
-> alternate screen
-> bounded scrollback ring
```

Scrollback never grows without a configured cap. Normal full-screen scrolling
pushes lines into the scrollback ring. Alternate-screen applications do not
pollute normal scrollback; copy mode over alternate screen sees only the
alternate screen snapshot.

Copy mode operates over a snapshot of:

```text
normal mode: scrollback + visible screen
alternate mode: alternate visible screen only
```

Selection extraction emits logical text, not raw cells. Wrapped lines are joined
without a hard newline where practical; hard line breaks are normalized to CRLF.
Wide cells, continuation cells, combining marks, and reversed/out-of-bounds
selections are clamped defensively before extraction.

The internal wmux paste buffer is authoritative:

```text
PasteBuffer
id
text
created_at
source
original byte count
truncated flag
```

Copy writes the wmux paste buffer first. The Windows clipboard write is
best-effort and runs off the attach hot path. Clipboard failure logs a warning
and reports a status message; it does not invalidate the internal paste buffer.

Paste is bounded and throttled:

```text
Ctrl+b ] -> resolve active pane -> read wmux paste buffer
-> normalize line endings
-> wrap in bracketed paste markers only if the pane enabled DECSET 2004
-> write to ConPTY from a tracked background worker
```

Paste workers are joined during attach shutdown and reaped while attached, so
the worker list cannot grow unbounded in normal use. PTY input serialization
prevents typed input from being interleaved into the middle of a throttled paste.

## ConPTY Lifecycle Contract

Each pane owns one `PtyProcess`. The process wrapper owns:

```text
ConPTY handle
child process handle
primary thread handle
input pipe read/write handles
output pipe read handle
Job Object handle, when Windows allows assignment
reader thread
screen grid and bounded raw-output chunks
```

`PtyProcess::terminate()` is idempotent. The first call performs shutdown; later
calls are no-ops and are logged at debug level. Pane/window/session kill paths
must remove the shell from daemon runtime state while holding the daemon mutex,
then call `terminate()` after releasing that mutex.

The shutdown order is:

```text
stop accepting input
close pane stdin pipe
close ConPTY
terminate Job Object if assigned
cancel and join reader thread
wait bounded time for child exit
fall back to TerminateProcess if needed
close remaining handles
mark reader/output done
```

Job Objects are used for process-tree cleanup when available. Job setup or
assignment failure must not prevent shell startup; it is logged and exposed as
degraded cleanup in `wmux server status`, then direct child-process termination
is used as fallback.

Pipe servicing rules:

```text
PTY readers do not mutate daemon layout/session/window/pane state directly.
PTY input writes do not hold daemon state locks.
Process waits do not hold daemon state locks.
ConPTY resize happens only after final pane body rectangles are known.
```

Resize correctness pipeline:

```text
ClientResize event
-> update attached client size
-> compute active window area
-> subtract status row when reserved
-> recompute layout rectangles from the n-child weighted tree
-> derive pane body rectangles from final pane rectangles
-> skip unchanged pane PTY sizes
-> call ResizePseudoConsole for changed panes outside daemon state locks
-> reset cached PTY size for failed resizes so the next redraw retries
-> clamp scrollback viewport, copy-mode cursor, and selection anchors
-> render dirty frame
```

Resize invariants:

```text
Layout rectangles are derived state and are never the source of truth.
Pane body sizes are clamped to at least 1x1 before ConPTY resize.
ConPTY is not resized mid-layout.
Duplicate resize frames should not redraw or resize unchanged panes.
Copy mode must remain valid, clamp safely, or exit if its pane disappears.
Tiny terminal sizes must not panic or write outside frame bounds.
```

Lifecycle validation must include OS-level process checks, not just daemon
counters. At minimum, pane kill, window kill, session kill, and forced daemon
shutdown must be tested against real descendant processes so a removed runtime
cannot hide an orphaned `cmd.exe`, `powershell.exe`, `pwsh.exe`, or ConPTY
support process.
