# AGENTS.md

This file is the contributor and coding-agent contract for wmux.

## Product Boundary

wmux is a native Windows terminal multiplexer. It is not a terminal emulator,
IDE, GUI shell, cloud collaboration layer, or WSL wrapper.

The daemon owns persistence:

```text
sessions
windows
layout trees
panes
platform PTY processes
terminal grids
scrollback
attached clients
```

Clients render, collect input, and send typed messages.

## Redraw Work Freeze

Do not continue fixing renderer symptoms with isolated tweaks. TerminalEngineV2
can stay, but further work on the Codex/Claude UI box, scroll jank, resize
refresh, split/close refresh, or attach inconsistency must be done through the
tmux-style client-scene redraw model.

Before changing redraw behavior, verify which invariant the change implements.
If the change cannot be described as one of the invariants below, do not make
it.

Permitted while the redraw model is being rebuilt:

```text
documentation
diagnostics and traces
tests and captured fixtures
small correctness fixes tied to an explicit invariant
```

Forbidden as standalone fixes:

```text
Codex/Claude/application-specific rendering branches
raw PTY replay to repair a client screen
host-terminal scroll operations as a substitute for rendering from grid state
global render-cache assumptions for a newly attached client
advancing render baselines for skipped/failed/coalesced frames
separate clear/body/cursor writes that can visibly present partial frames
```

## Tmux-Style Redraw Invariants

wmux must match tmux's redraw architecture at the invariant level:

```text
The daemon's pane screen/grid is authoritative.
PTY output mutates server-side pane terminal state.
Clients never reconstruct screen contents by replaying raw pane history.
Each client owns a separate physical-terminal baseline.
New attach, resize, layout change, reattach, and window switch render the
current scene from the authoritative model.
Client-scoped caches may only suppress output when that exact client baseline
is known valid.
Cache/baseline updates happen only after the frame write succeeds.
Dropped, skipped, coalesced, or failed writes do not update baseline state.
Styled blanks, cursor state, terminal modes, borders, status, overlays, and
alternate-screen contents are part of the scene.
```

A correct redraw flow is:

```text
daemon/window/pane model
-> current client-visible scene
-> optional diff against that client's committed baseline
-> single ordered frame
-> successful client write
-> commit new baseline
```

An incorrect redraw flow is:

```text
raw output backlog
-> replay/skip/coalesce history
-> guess what the client should see
```

When in doubt, prefer a coherent current-scene materialization over a partial
update. Performance optimizations must preserve this baseline contract.

## Platform Boundary

wmux is Windows-first, not Windows-entangled. Core multiplexer logic must model
terminal multiplexing concepts, not Win32 APIs.

Core logic owns:

```text
sessions/windows/panes as logical objects
layout tree and geometry
command engine and target resolver
input modes, copy mode, paste buffer
terminal grid, VT parser, renderer, status line
config/options/diagnostics models
```

The platform layer owns:

```text
ConPTY and future PTY backends
process spawning, resizing, killing, and cleanup
Windows HANDLE/HPCON/Job Object details
named-pipe or future socket transport
terminal mode setup/restoration
clipboard integration
console-control or signal behavior
```

Core headers and core sources must not directly include `windows.h` or reference
`HANDLE`, `HPCON`, `CreateProcess`, `ResizePseudoConsole`, Job Object APIs,
Windows clipboard APIs, or named-pipe APIs. Keep those details behind
platform services under `src/platform` / `include/wmux/platform`.

New daemon/core code should normally depend on the aggregate service boundary:

```text
wmux/platform/services.hpp
```

The current service interfaces are:

```text
PtyBackend
ClipboardBackend
TerminalModeBackend
IpcTransportBackend
PlatformInfoBackend
```

Concrete low-level platform code may use narrower platform headers:

```text
wmux/platform/pty_process.hpp
wmux/platform/ipc_transport.hpp
wmux/platform/terminal_control.hpp
wmux/platform/clipboard.hpp
wmux/platform/platform_info.hpp
```

Do not reintroduce removed top-level compatibility headers:

```text
wmux/pty_process.hpp
wmux/ipc_transport.hpp
wmux/terminal_control.hpp
wmux/windows_clipboard.hpp
```

The platform boundary is tested by `tests/platform_boundary_test.cpp`. If that
test fails, move OS-specific code behind the platform layer instead of weakening
the test.

The CMake target split is part of the boundary:

```text
wmux_core              platform-neutral core implementation
wmux_platform_windows  Windows backend and OS integration
wmux_daemon            daemon runtime and rendering
wmux_app_support       diagnostics/app helpers using platform services
wmux_windows_runtime   Windows attach/daemon server wiring
wmux                   executable entry point
```

Add new files to the narrowest correct target. Do not paste implementation
sources directly into `wmux` or `wmux_tests` when they already belong to one of
the libraries.

Terminal-engine work is core work. The intended long-term source layout is:

```text
src/platform/
  windows_pty_process.cpp
  unix_pty_process.cpp       future backend
src/core/
  terminal_engine_legacy.cpp
  terminal_engine_v2.cpp
  vt_parser_v2.cpp
  screen_writer_v2.cpp
  grid_core_v2.cpp
src/daemon/
  sessions, windows, panes, layout, render scheduling
src/client/
  attach UI and input
```

Forbidden in terminal engine, parser, grid, and render-core code:

```text
Win32 console hacks
ConPTY assumptions
Windows handles
raw PTY replay as the rendering model
```

Allowed inside the platform-neutral terminal engine:

```text
packed cells
line pools
style interning
VT parser replacement
screen-writer / print-run collection
render-view replacement
queue-friendly ingestion APIs
```

Keep the robust legacy engine available as a fallback while V2 is opt-in and
measured. Do not trade correctness for speed by weakening damage invalidation,
scrollback rules, copy-mode semantics, or platform boundaries.

## Core Model

Use stable IDs internally:

```text
SessionId
WindowId
PaneId
ClientId
NodeId
RequestId
```

Indexes are display metadata only. Do not use indexes for runtime identity,
process ownership, IPC routing, or cleanup.

Active object rules:

```text
Each client has at most one attached session.
Each session has one active window.
Each window has one active pane.
Pane/window kill paths must choose a replacement active object.
No mutation path may leave active IDs dangling.
```

## Non-Negotiable Rule

Only the daemon event loop mutates daemon-owned runtime state. Other threads
must enqueue typed events.

Do not add code where:

```text
PTY reader threads mutate layout/session/window/pane state directly.
IPC handlers mutate state outside the event-loop path.
Renderer code changes layout or active pane state.
Input decoding directly changes sessions/windows.
Mouse parsing directly resizes panes without a daemon command/event.
```

If a shortcut seems easier, stop and route it through the daemon command/event
path.

## Command Rules

User actions should become runtime commands before they mutate daemon-owned
state:

```text
raw input bytes
-> key decoder / mode handler
-> key binding table
-> RuntimeCommand
-> TargetResolver
-> command executor
-> state mutation
-> status/log/redraw
```

Do not let command implementations manually search by names, indexes, mouse
coordinates, or stale active IDs. Route target lookup through the shared target
resolver and return user-visible errors for dead or ambiguous targets.

Typed daemon events currently include:

```text
AttachStart
ClientConnected
ClientDisconnected
ClientInput
DecodedKey
MouseEvent
MouseFocus
AttachCommand
CommandModeCommand
Paste
PasteBufferSet
IpcCommand
PaneOutput
PaneExited
ClientResize
Timer
Shutdown
```

Treat stale event targets as expected race fallout. Log them at debug level and
drop them safely instead of asserting that they cannot happen.

PTY output/render batching is intentionally separate from attach input events.
Do not route every PTY byte through the daemon event queue without an explicit
batching and backpressure design.

## IPC Rules

Command/control IPC must use the versioned `WMUX` frame:

```text
WMUX magic
uint16 protocol version
frame kind
uint64 request id
uint32 payload length
bounded payload
```

Never parse command JSON before the frame has passed validation. Reject bad
magic, unsupported versions, unknown kinds, oversized payloads, and truncated
payloads without panicking. Every command response must use the same request ID
as the request.

Windows pipe endpoints must remain user-specific. Apply same-user pipe ACLs
where practical, and log any security-descriptor degradation.

Attach traffic is separate from command IPC. Attach startup uses a framed
Control request. Client-to-daemon attach messages use framed AttachInput
messages carrying bounded typed attach payloads. Daemon-to-client render data
uses framed AttachOutput messages. Attach failures use framed Error responses.

## Stability Priorities

Performance and stability outrank feature count.

Rules:

```text
RAII for handles, pipes, threads, and terminal modes.
No raw owning pointers.
No unbounded output buffers.
No blocking work in the client input/render loop.
No silent placeholder behavior for tmux semantics.
No host-terminal assumptions outside TerminalCapabilities/TerminalQuirk.
No layout rectangles as source of truth.
No process-kill path without explicit cleanup ownership.
No process waits, pipe writes, or PTY teardown while holding daemon state locks.
```

Host-terminal scroll-region optimization is disabled while the V2 terminal
engine is being built. Child PTY scrolls must mutate the authoritative terminal
grid and force pane/window damage as needed; do not emit host-terminal scroll
commands or move render-cache rows from child scroll events until the engine
and line-view renderer are fast and covered by focused corruption tests.

Pane processes are owned through `PtyProcess`. `PtyProcess::terminate()` is
idempotent and must be called after the shell has been removed from daemon
runtime maps. The Windows backend uses Job Objects when available for
process-tree cleanup; if Job Object assignment is unavailable, log and surface
degraded cleanup instead of failing shell startup.

## Layout Rules

Layout is an n-child weighted tree stored in `LayoutArena`:

```text
Window.layout_root: NodeId
LayoutArena.nodes: NodeId -> LayoutNode
Pane leaf: stable NodeId + PaneId
Split group: stable NodeId + axis + children + weights
```

`PaneNode` is a derived snapshot for summaries, rendering helpers, and tests.
Do not make it the authoritative owner of layout state again.

Maintain these invariants:

```text
Every pane appears exactly once in the tree.
Every split has at least two children.
weights.size() == children.size().
Weights are positive finite values.
Same-axis split children are flattened.
One-child splits collapse after pane removal.
Computed rectangles are derived from tree + terminal size.
PTY resize uses pane body size.
```

## tmux Semantics

When implementing tmux-inspired behavior, check tmux semantics first. If
Windows or ConPTY makes exact behavior impossible, document the limitation and
surface it instead of adding fake behavior.

Known strict bindings:

```text
Ctrl+b c  new window and switch to it
Ctrl+b n  next window
Ctrl+b p  previous window
Ctrl+b x  kill active pane with safety guard
Ctrl+b E  spread/equalize panes using local-first parent walk
```

Lowercase `Ctrl+b e` is intentionally not the spread binding.

## Diagnostics

Bug reports should start with:

```powershell
wmux doctor
wmux doctor --json
wmux server status
```

Logs live under the user-local wmux log directory printed by `wmux doctor`.
