# Technical Roadmap

## Architecture

`wmux` should use a client/server architecture.

```text
Terminal Emulator
    |
wmux client
    |
Windows named pipe IPC
    |
wmux daemon
    |
Session manager
    |
Sessions
    |
Windows
    |
Panes
    |
ConPTY-backed shell processes
```

The daemon owns:

- sessions
- windows
- panes
- shell processes
- ConPTY handles
- pane buffers
- virtual terminal state
- scrollback

The client owns:

- terminal input capture
- rendering daemon-provided frames
- forwarding key, mouse, resize, and command events
- restoring local terminal state on exit

The client should not directly interpret child shell output. The daemon should
read pane output, update pane terminal state, and send renderable updates to
attached clients.

## Technology Choices

- Language: C++20
- Build: CMake
- CLI parsing: CLI11
- Logging: spdlog
- Testing: Catch2 or GoogleTest
- IPC: Windows named pipes
- PTY: ConPTY
- Terminal control: Win32 Console APIs and VT escape sequences

## Core Data Model

```text
Session
- id
- name
- windows
- active_window_id
- created_at

Window
- id
- name
- pane_tree
- active_pane_id

Pane
- id
- process_id
- ConPTY handle
- input pipe
- output pipe
- terminal grid
- scrollback ring
- dimensions
```

Pane layouts should use a tree:

```text
PaneNode
- Leaf(PaneId)
- Split {
    direction
    ratio
    first
    second
  }
```

## Important Internal Reality

`wmux` is not a terminal emulator as a product, but it does need a minimal
virtual terminal model internally.

Pane output contains VT escape sequences that assume ownership of a full
terminal surface. To support split panes, redraw, resize, scrollback, copy mode,
and reattach, the daemon needs to parse pane output into a bounded screen model.

At minimum, each pane should eventually track:

- normal screen grid
- alternate screen grid
- cursor position
- text attributes
- dirty regions
- scrollback ring
- terminal modes needed by common shells and TUIs

## IPC Design

Use two logical IPC paths:

### Command IPC

Request/response messages for administrative commands:

- create session
- list sessions
- rename session
- kill session
- create/list/rename windows
- server status
- server stop

JSON is acceptable here because command traffic is low volume.

### Attach Streaming IPC

Bidirectional event stream for interactive sessions:

- client input events
- mouse events
- terminal resize events
- render frames or diffs
- status updates

This path should use length-prefixed framed messages. It should not depend on
per-keystroke JSON in the final design.

## Development Phases

### Phase 0: Repository Skeleton

- Create CMake project
- Build `wmux.exe`
- Add CLI parser
- Add logging
- Add `--help` and `version`

### Phase 1: Command Surface

Implement command parsing with placeholder behavior:

```text
wmux
wmux new -s <name>
wmux ls
wmux attach -t <name>
wmux rename-session -t <old> <new>
wmux kill-session -t <name>
wmux server status
wmux server stop
wmux server stop --force
```

### Phase 2: Daemon Skeleton

- Add internal daemon mode
- Auto-start daemon when needed
- Add single-instance behavior
- Add named pipe command IPC
- Implement server status and stop

Current implementation notes:

- `wmux --daemon` runs the background command daemon.
- Native Windows builds use a named pipe endpoint.
- WSL/Linux development builds use a Unix-domain socket fallback.
- Command IPC is line-delimited JSON for low-volume request/response commands.
- Attach now uses a separate Windows named-pipe endpoint from command IPC.
- The initial attach stream keeps JSON only for attach startup. Client input and
  detach lifecycle are length-prefixed frames, which keeps shell input separate
  from control messages.
- Existing Phase 1 commands round-trip through the daemon and still return
  placeholder responses until Phase 3 session state exists.

### Phase 3: Session Manager

- Create sessions
- List sessions
- Rename sessions
- Kill sessions
- Prevent duplicate names
- Keep sessions alive while daemon runs

Current implementation notes:

- Session state is daemon-owned and in-memory.
- Session identity is a stable numeric ID. Session names are mutable metadata and
  command lookup keys, not runtime ownership keys.
- `wmux new -s <name>`, `wmux ls`, `wmux rename-session -t <old> <new>`,
  and `wmux kill-session -t <name>` now mutate daemon state.
- Duplicate names are rejected by the daemon, not only by client-side parsing.
- Session state intentionally lasts only for the daemon lifetime until process
  recovery is introduced. ConPTY-backed shell persistence now exists while the
  daemon is running.

### Phase 4: First ConPTY Shell

- Add `PtyProcess` abstraction
- Create input and output pipes
- Use `CreatePseudoConsole`
- Spawn `powershell.exe` initially
- Read output asynchronously
- Send input to shell

Current implementation notes:

- `PtyProcess` owns the ConPTY handle, input/output pipes, child process handle,
  Windows job object, reader thread, and bounded recent output chunks.
- Shell processes are assigned to a kill-on-close job object so session teardown
  and daemon process exit clean up the daemon-owned shell process tree where
  Windows permits it.
- `wmux new -s <name>` starts `powershell.exe -NoLogo -NoProfile` under the
  daemon and stores runtime ownership by stable session ID.
- Windows development builds also honor `WMUX_DEFAULT_SHELL` as a temporary
  test hook. It is not a replacement for the later configuration system.
- `wmux attach -t <name>` uses a long-lived attach named-pipe connection. The
  daemon replays recent buffered output and then streams live ConPTY output
  while forwarding framed client input to the shell.
- Attach output is still raw byte passthrough. A real Windows terminal emulator
  is expected to handle ConPTY terminal negotiation for this phase.
- Raw PTY byte chunks are only a Phase 4 replay mechanism. Scrollback, copy
  mode, pane rendering, and correct reattach must be built on daemon-owned VT
  state and bounded scrollback rings, not on raw byte replay.
- This phase proves daemon-owned shell lifetime and first attach output. It is
  not the final pane renderer or terminal parser.

### Phase 5: Raw Interactive Attach

- Put client terminal into raw mode
- Forward keyboard input to active pane
- Stream raw pane output back to client
- Detect prefix key
- Implement `Ctrl+b d` detach
- Restore terminal state on client exit

Current overlap from Phase 4:

- The client already switches the local console into a raw-ish VT input/output
  mode and restores it on exit.
- `Ctrl+b d` sends an explicit detach frame before the client leaves the
  streaming attach connection.
- Phase 5 now sends the initial client terminal size during attach and asks the
  daemon to resize the session ConPTY before replaying output.
- Phase 5 should still harden this path with live resize events,
  redirected-stdin behavior, and attach/crash cleanup tests.
- The daemon tracks active attach clients by client ID and classifies connection
  endings as explicit detach, disconnect, protocol error, shell close, or output
  close while unregistering the client.
- Attach connections still run on worker threads for the prototype. They are no
  longer invisible to daemon state, but a later event-loop design should replace
  this with fully joinable/owned client runtimes.

Current lifecycle guard:

- `wmux server stop` refuses to stop while live sessions exist.
- `wmux server stop --force` explicitly terminates live session runtimes before
  stopping the daemon.
- Session kill and forced server stop ask active attach connections to
  disconnect so blocked pipe IO can unwind.

This proves ConPTY, IPC, daemon lifetime, input routing, detach, and reattach
before pane rendering becomes complex.

### Phase 6: True Detach and Reattach

- Keep shell lifetime owned by daemon
- Continue reading output while detached
- Store bounded recent output
- Reconnect client to existing session
- Replay visible state on attach
- Resume live streaming
- Add repeated create/attach/detach/kill testing and verify shell cleanup.

Current implementation notes:

- Session shells are daemon-owned and continue running after the attach client
  sends detach or the attach pipe is closed by terminal exit.
- The ConPTY reader thread stays alive while detached and appends output to a
  bounded recent-output buffer.
- Reattach opens a new attach pipe, gets the existing session runtime by stable
  session ID, replays buffered output, then resumes live streaming.
- This is still raw byte replay. The later virtual terminal state phase is
  required before copy mode, scrollback, alternate screen replay, and panes can
  be considered correct.

Current stability checks:

```powershell
.\scripts\test-kill-session-cleanup.ps1 -Wmux .\build-vs\Debug\wmux.exe -Iterations 20
.\scripts\test-detach-reattach.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The cleanup script validates repeated session create/kill cleanup and checks
that no new daemon-owned shell processes remain after each kill. It leaves
pre-existing sessions alone. The detach/reattach script requires an empty
daemon because it restarts wmux with a deterministic `cmd.exe /D /Q` test shell;
then it drives the attach pipe directly, simulates terminal closure, verifies
output continues while detached, and verifies explicit detach leaves the session
attachable.

### Phase 7A: Window Model and Commands

- Add stable window IDs
- Make sessions own ordered windows
- Track `active_window_id`
- Create one ConPTY-backed shell per window
- Add command IPC for creating, listing, and renaming windows

Current implementation notes:

- Every new session starts with one initial window named `0`.
- Window runtime ownership is keyed by stable `SessionId` and `WindowId`; window
  names are metadata and command lookup text, not process ownership keys.
- `wmux new-window -n <name>`, `wmux list-windows`, and
  `wmux rename-window <new>` operate on the only live session when exactly one
  session exists.
- The same commands accept `-t <session>` for explicit daemon targeting:
  `wmux new-window -t finance -n logs`, `wmux list-windows -t finance`, and
  `wmux rename-window -t finance agents`.
- `wmux attach -t <session>` attaches to that session's active window.
- Phase 8C replaces raw active-window passthrough with basic daemon-rendered
  pane frames. Copy mode and correct redraw for complex terminal apps still
  require daemon-owned VT state.

### Phase 7B: Interactive Window Switching

- Add `Ctrl+b c` for new window
- Add `Ctrl+b n` and `Ctrl+b p` for next/previous window
- Add command-mode equivalents once command mode exists
- Ensure attached clients switch to the newly active window cleanly

Current implementation notes:

- Attach input now has a third framed message type for wmux control commands.
  Normal shell bytes, explicit detach, and window commands are kept separate.
- `Ctrl+b c` creates a new daemon-owned window shell in the attached session and
  makes it active.
- `Ctrl+b n` and `Ctrl+b p` switch the session's active window and replay the
  newly active shell's bounded output buffer to the attached client.
- The attach thread performs initial and switch replay before returning to input
  polling. This avoids blocking output behind an idle synchronous pipe read.
- Server-side attach input reads poll for complete framed messages before
  calling `ReadFile`, which keeps live output responsive while the client is
  idle.
- Phase 8C replaces raw byte replay in the attached view with basic
  daemon-rendered pane frames. Correct redraw across full-screen TUIs,
  alternate screen state, scrollback, and copy mode still require the
  daemon-owned VT model in Phase 9.

Current stability checks:

```powershell
.\scripts\test-window-switching.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script verifies interactive create, previous-window, and next-window
control frames against real ConPTY shells and confirms each window keeps
independent shell state.

### Phase 8A: Pane Model and Split Commands

- Add stable pane IDs
- Add pane summaries to windows
- Add a pane layout tree
- Track `active_pane_id` per window
- Add command IPC for `split-window -h` and `split-window -v`
- Spawn one daemon-owned ConPTY shell for each new pane

Current implementation notes:

- Every new session starts with one window and one pane.
- Every new window starts with one pane.
- `split-window [-t <session>] -h` and `split-window [-t <session>] -v`
  split the active pane in the active window and make the newly created pane
  active.
- Pane runtime ownership is keyed by stable `SessionId`, `WindowId`, and
  `PaneId`; pane layout position is not process identity.
- The pane tree records nested horizontal and vertical splits with a 50/50
  initial ratio. Phase 8C uses this tree for basic rectangle computation and
  bordered rendering.
- Command-IPC splits update the daemon model. Existing attach clients are still
  not notified unless the split is initiated through that attach connection.

### Phase 8B: Interactive Pane Splitting and Focus

- Add `Ctrl+b %` and `Ctrl+b "` for splitting
- Add `Ctrl+b` arrow keys for pane focus
- Route input only to the focused pane
- Ensure each pane keeps independent shell state

Current implementation notes:

- Attach command frames now include pane commands for horizontal split, vertical
  split, and directional focus.
- `Ctrl+b %` creates a new daemon-owned pane shell by splitting the active pane
  horizontally, then replays the new active pane to the attached client.
- `Ctrl+b "` creates a new daemon-owned pane shell by splitting the active pane
  vertically, then replays the new active pane to the attached client.
- `Ctrl+b` arrow keys select the nearest pane in that direction based on the
  pane tree's current virtual rectangles. This is the same neighbor model the
  later renderer can use, but it does not draw borders yet.
- Attach streaming tracks active window and active pane IDs so output from an
  old pane is ignored after focus changes.
- Input is resolved against the daemon's current active pane on every input
  frame, so shell input follows focus.
- Phase 8C replaces raw active-pane passthrough with basic multi-pane redraws.
  Phase 8B itself proved process ownership and input routing, not final
  split-pane rendering.

Current stability checks:

```powershell
.\scripts\test-pane-focus.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script verifies interactive horizontal split, vertical split, left/right/up
focus, and independent shell state for each pane against real ConPTY shells.

### Phase 8C: Layout Rendering

- Compute pane rectangles
- Draw borders and active pane highlight
- Clip pane output to pane rectangles
- Resize ConPTY dimensions when pane rectangles change
- Handle terminal resize events

Current implementation notes:

- `compute_pane_layout_rects` converts the daemon-owned pane tree into integer
  rectangles for the current attached terminal size.
- Attach output now redraws the active window as a full frame containing every
  visible pane, ASCII borders, active-pane highlighting, clipped text regions,
  and a status line.
- Pane ConPTY dimensions are resized to the pane body before replay/redraw.
- The renderer strips common VT control sequences and renders plain clipped
  text from the bounded pane output buffer. This keeps the layout stable enough
  for split-pane validation, but it is intentionally not the final terminal
  renderer.
- Live terminal resize events are still pending. The initial attach size is
  used for the current frame.
- Production-grade redraw, full-screen TUI behavior, color/style attributes,
  alternate screen, scrollback, and copy mode still require Phase 9's
  daemon-owned VT/grid model.

Current stability checks:

```powershell
.\scripts\test-pane-focus.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script now validates pane split/focus behavior while receiving bordered
multi-pane redraw frames from the daemon.

### Phase 9: Virtual Terminal State

- Parse VT output incrementally
- Maintain pane screen grid
- Track cursor state and text attributes
- Handle common clear, cursor movement, and SGR sequences
- Support alternate screen buffer
- Mark dirty regions for render updates

### Phase 10: Render Daemon-Owned Grid

- Client renders daemon-provided pane state
- Avoid direct raw passthrough for normal attached rendering
- Add frame coalescing
- Clip output to pane rectangle

### Phase 11: Command Mode

- Add `Ctrl+b :`
- Parse command text
- Dispatch commands
- Show command errors in status line

### Phase 12: Scrollback

- Add pane-local scrollback ring
- Track viewport
- Support scrolling
- Keep memory bounded

### Phase 13: Copy Mode and Clipboard

- Add copy mode state
- Freeze viewport
- Support cursor movement and selection
- Highlight selection
- Copy to internal buffer
- Copy to Windows clipboard

### Phase 14: Paste Buffer

- Add internal paste buffer
- Paste into active pane
- Normalize line endings
- Optionally integrate with system clipboard

### Phase 15: Mouse Mode

- Enable terminal mouse reporting
- Parse mouse events
- Map clicks to pane rectangles
- Click to focus pane
- Drag borders to resize panes

### Phase 16: Configuration

Initial config path:

```text
%USERPROFILE%\.wmux.conf
```

Initial settings:

```text
set -g prefix C-b
set -g mouse on
set -g default-shell powershell.exe
set -g status on
```

### Phase 17: Hardening for Daily Use

- Add handle leak tests
- Add attach/detach loop tests
- Add rapid resize tests
- Add high-output stress tests
- Add many-pane stress tests
- Add long-running workload validation
- Use the tool for real development sessions

Hardening is listed as a phase, but stability checks should be added throughout
the project rather than deferred until the end.

## Early Success Gate

The first major success checkpoint is:

```text
wmux new -s dev
run a long process
detach
close the terminal
open a new terminal
wmux attach -t dev
confirm the long process is still running
```

The next success checkpoint is the same flow with split panes, bounded memory,
and no visible input/render lag under heavy output.
