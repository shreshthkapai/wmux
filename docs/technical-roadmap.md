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
- Testing: current assert-based C++ unit harness plus PowerShell integration,
  stress, and soak scripts; a future Catch2/GoogleTest migration is optional
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

Command JSON is carried inside the versioned `WMUX` frame so malformed, partial,
oversized, or incompatible requests are rejected before JSON parsing.

### Attach Streaming IPC

Bidirectional event stream for interactive sessions:

- client input events
- mouse events
- terminal resize events
- render frames or diffs
- status updates

This path uses the same `WMUX` frame header with attach-specific frame kinds:
`AttachInput` for client events, `AttachOutput` for rendered terminal frames,
and `Error` for stream failures. It must not depend on per-keystroke JSON.

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
- Command IPC is a short-lived `WMUX` Control/Error frame exchange carrying
  bounded JSON payloads.
- Attach uses a separate Windows named-pipe endpoint from command IPC.
- Attach startup uses a `WMUX` Control frame. Interactive client input,
  detach, resize, mouse, copy/paste, status, and rendered output use framed
  attach traffic, keeping shell input separate from control messages.
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
- `wmux new -s <name>` starts a daemon-owned shell and stores runtime ownership
  by stable session ID.
- Shell resolution order is: configured `default-shell`, `WMUX_DEFAULT_SHELL`
  when no config shell is set, PowerShell 7 (`pwsh`), Windows PowerShell, then
  `cmd.exe`.
- New panes currently inherit the daemon process working directory. Per-pane
  current-directory detection is intentionally documented as a later Windows
  hardening item because it is not reliably available through ConPTY alone.
- `wmux attach -t <name>` uses a long-lived attach named-pipe connection. The
  daemon sends framed rendered output while forwarding framed client input to
  the active pane shell.
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

- Attach input uses typed frames for wmux control commands. Normal shell bytes,
  explicit detach, and window commands are kept separate.
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
- Add `Ctrl+b x` for killing the active pane
- Add `Ctrl+b E` for recursively equalizing pane sizes in the active window
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
- `Ctrl+b x` kills the active pane through the same cleanup path as
  `kill-pane`.
- `Ctrl+b E` preserves the pane tree shape and recursively redistributes split
  ratios by leaf count, matching tmux's equalize-current-window behavior more
  closely than a direction-specific resize.
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
- Production-grade redraw, full-screen TUI behavior, color/style attributes,
  alternate screen, scrollback, and copy mode still require Phase 9C's
  daemon-owned VT/grid model.

Current stability checks:

```powershell
.\scripts\test-pane-focus.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script now validates pane split/focus behavior while receiving bordered
multi-pane redraw frames from the daemon.

### Phase 9A: Live Resize Path

- Detect attached terminal size changes in the client
- Send resize events on the attach stream
- Recompute pane rectangles in the daemon
- Resize visible pane ConPTY dimensions
- Redraw the active window after resize

Current implementation notes:

- The client polls the current console viewport while attached and sends a
  dedicated resize attach frame when the size changes.
- Attach resize messages are framed attach-input events, separate from shell
  input and wmux command frames.
- The daemon keeps the current dimensions per attach connection, recomputes the
  active window's pane layout on resize, calls `ResizePseudoConsole` for each
  visible pane body, and redraws the bordered frame.
- This makes split-pane layout responsive to outer terminal resize, but it is
  still backed by raw output sanitization rather than a real VT screen grid.

### Phase 9B: Rendering Hardening

- Improve nested split edge cases
- Prevent border/output corruption in constrained pane rectangles
- Reduce unnecessary full-frame redraw churn under continuous output
- Add layout geometry tests for overlap, gap, and clamped-ratio behavior

Current implementation notes:

- Pane body rendering is disabled for rectangles that are too small to contain
  both borders and text. Tiny panes now render as border-only instead of letting
  shell output overwrite pane borders.
- Visible pane text extraction now clips while scanning the bounded pane output
  buffer instead of building a full sanitized intermediate string. This keeps
  redraw cost tied to the visible pane dimensions rather than the whole recent
  output buffer.
- Attach frame writes and active-pane/window replay are serialized more tightly
  so command-triggered redraws, resize redraws, and output-triggered redraws do
  not interleave on the same attach pipe.
- Continuous output redraws are coalesced to roughly one frame every 33 ms.
  This keeps high-output panes from spinning the renderer as fast as the daemon
  can loop while preserving responsive live updates.
- Session manager tests now verify nested pane layouts cover the requested
  terminal area without gaps or overlaps, clamp extreme split ratios, and avoid
  invalid rectangles under tiny terminal dimensions.
- This phase hardens the current basic renderer. It is still not the final
  terminal rendering model for full-screen TUIs, alternate screen state,
  scrollback, color/style attributes, or copy mode.

### Phase 9C: Virtual Terminal State

- Parse VT output incrementally
- Maintain pane screen grid
- Track cursor state and text attributes
- Handle common clear, cursor movement, and SGR sequences
- Support alternate screen buffer
- Mark dirty regions for render updates

Current implementation notes:

- `TerminalGrid` is the first daemon-owned virtual terminal model. It keeps a
  bounded screen grid per pane process with cursor position, basic text
  attributes, normal screen state, and alternate screen state.
- The ConPTY reader thread feeds output bytes into the pane grid as bytes
  arrive, so attached redraws no longer rebuild visible pane text by sanitizing
  the whole raw output buffer.
- The current parser handles printable bytes, carriage return, line feed,
  backspace, tab stops, common CSI cursor movement, clear screen/line commands,
  basic SGR attribute state, OSC skipping, reset, and alternate-screen
  switching.
- Pane resize now resizes both ConPTY and the stored terminal grid before the
  next redraw.
- The raw bounded PTY byte buffer still exists for diagnostics and future
  recovery work, but the basic renderer now reads `TerminalScreenSnapshot`
  lines from daemon-owned pane state.
- Unit tests cover newline handling, cursor movement, clear-line behavior,
  bounded screen scrolling, SGR escape removal, alternate-screen switching, and
  resize preservation.
- This is a foundation, not full terminal compatibility yet. Full TUI parity
  still needs UTF-8/wide-glyph handling, richer DEC modes, dirty-region
  tracking, scrollback integration, color rendering, and more complete escape
  coverage.

### Phase 10: Command Mode

- Add `Ctrl+b :`
- Parse command text
- Dispatch commands
- Show command errors in status line

Current Phase 10A implementation notes:

- `Ctrl+b :` enters command prompt mode in the attached client.
- The prompt text is sent as a dedicated attach status frame, so the daemon
  remains the single owner of status-line rendering.
- Command prompt input supports printable ASCII, quoted arguments, backspace,
  `Esc` cancel, and `Enter` submission.
- Status payloads are sanitized on the daemon side before rendering to avoid
  terminal-control injection from malformed attach clients.

Current Phase 10B implementation notes:

- Submitted command text is sent as a distinct attach frame type, separate from
  raw shell input, status redraws, and keybind command frames.
- Command mode dispatch is scoped to the currently attached session. It rejects
  `-t <session>` forms so prompt commands cannot accidentally target another
  live session.
- Supported commands are:
  - `rename-session <new>`
  - `new-window -n <name>`
  - `rename-window <new>`
  - `split-window -h`
  - `split-window -v`
- Invalid command text shows a status-line error and does not disconnect the
  attach client.
- Successful commands show a status-line result, update daemon state, and redraw
  the active window.

Current Phase 10C implementation notes:

- Command mode now supports destructive current-session operations:
  - `kill-pane`
  - `kill-window`
- `kill-pane` removes the active pane, collapses the pane tree around the
  surviving sibling subtree, terminates the removed pane's daemon-owned shell,
  and makes a neighboring pane active.
- `kill-pane` refuses to remove the last pane in a window. It does not
  implicitly kill the active window or session.
- `kill-window` removes the active window, terminates every daemon-owned shell
  in that window, and activates a neighboring window.
- `kill-window` refuses to remove the last window in a session. It does not
  implicitly kill the session.
- Removed shell processes are moved out of daemon state under the state lock and
  terminated after the lock is released, keeping state mutation serialized while
  avoiding process teardown under the global daemon mutex.

Current stability checks:

```powershell
.\scripts\test-command-mode.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script drives command-mode attach frames directly and verifies
invalid-command errors, window creation, window rename, pane split input
routing, pane/window destructive command cleanup, last-pane and last-window
refusal, and attached-session rename against daemon-visible state.

### Phase 11: Scrollback

- Add pane-local scrollback ring
- Track viewport
- Support scrolling
- Keep memory bounded

### Phase 12: Copy Mode and Clipboard

- Add copy mode state
- Freeze viewport
- Support cursor movement and selection
- Highlight selection
- Copy to internal buffer
- Copy to Windows clipboard

### Phase 13: Paste Buffer

- Add internal paste buffer
- Paste into active pane
- Normalize line endings
- Optionally integrate with system clipboard

### Phase 14: Mouse Mode

- Enable terminal mouse reporting
- Parse mouse events
- Map clicks to pane rectangles
- Click to focus pane
- Drag borders to resize panes

Current mouse foundation implementation notes:

- Attached clients enable XTerm button-event mouse reporting with SGR
  coordinates while attached and disable those modes again through an RAII
  guard during normal detach, disconnect, or process exit cleanup.
- Mouse parser coverage exists for SGR press, release, drag, wheel, incomplete,
  and invalid sequences.
- The attach client recognizes and consumes SGR mouse sequences so they do not
  leak into the active shell as raw control bytes.
- Parsed mouse events are sent to the daemon as compact attach frames with
  1-based terminal coordinates, button identity, and press/release/drag action.
  The older mouse-focus frame remains supported as a compatibility path for
  existing direct attach tests.
- The daemon maps left-click coordinates to the active window's current pane
  rectangles and selects the hit pane without routing the click bytes into the
  shell.
- Presses on split borders start a per-attach-client drag target. Drag events
  update the target split ratio in the daemon-owned pane tree, clamp the ratio
  to the same safe bounds used by layout, recompute pane rectangles, resize
  visible pane ConPTYs, and redraw the active window.
- Clicks and drags outside pane rectangles, including the status line, are
  no-ops rather than protocol failures.
- `wmux set -g mouse on` and `wmux set -g mouse off` now update daemon-owned
  runtime state. The setting persists while the daemon process runs and is
  reported in `wmux server status` as `mouse: on` or `mouse: off`.
- Attach startup responses include the current mouse setting. Attached clients
  enable terminal mouse reporting only when `mouse` is on, and leave mouse
  reporting disabled when `mouse` is off.
- Current regression coverage includes parser unit tests and
  `scripts/test-mouse-focus.ps1`, which verifies clicked-pane focus changes
  shell input routing. `scripts/test-mouse-resize.ps1` verifies drag-resize by
  moving a split border and confirming subsequent click-to-focus uses the
  updated pane geometry. `scripts/test-mouse-setting.ps1` verifies command IPC,
  status output, and attach response propagation for the mouse setting.

### Phase 15: Configuration

Initial config path:

```text
%USERPROFILE%\.wmux.conf
```

Initial settings:

```text
set -g prefix C-b
set -g mouse on
set -g default-shell pwsh.exe
set -g status on
set -g escape-time-ms 50
set -g scrollback-max-lines 10000
set -g paste-buffer-max-bytes 1048576
set -g max-sessions 64
set -g max-windows-per-session 64
set -g max-panes-per-window 64
set -g client-output-queue-max-bytes 8388608
set -g client-output-queue-max-frames 8
bind-key z new-window
bind-key Up select-pane-up
unbind-key e
```

The daemon keeps explicit scoped option structures for global, session, window,
pane, and client settings. The current public config syntax remains tmux-style
`set -g` plus tmux-style `bind-key` and `unbind-key` lines. Runtime `set -g`
uses the same validator as config-file loading, and attached clients receive
live updates for prefix, mouse reporting, escape timing, status visibility, and
attach backpressure/render limits. Keybinding overrides are validated at daemon
config load and sent to attach clients through framed settings events. Runtime
`bind-key` and `unbind-key` use the same validator and live settings path, so
attached clients receive updated prefix bindings without reattach. Runtime
option changes are daemon-memory changes; they do not rewrite the config file.

Configured resource limits are enforced for session/window/pane creation,
pane raw-output retention, pane scrollback, paste buffers, attach render frame
size, and attach client output queues. IPC frame hard caps remain compiled
protocol limits because frame validation must happen before daemon config is
available to clients. `log-max-bytes` rotates the active log to a single `.1`
retention file before append once the configured byte cap would be exceeded.

### Phase 16: Hardening for Daily Use

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
