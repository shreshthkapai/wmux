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
- Attach now has a separate streaming pipe path on Windows.
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
  reader thread, and bounded recent output chunks.
- `wmux new -s <name>` starts `powershell.exe -NoLogo -NoProfile` under the
  daemon and stores it by session name.
- `wmux attach -t <name>` uses a long-lived named-pipe connection. The daemon
  replays recent buffered output and then streams live ConPTY output while
  forwarding client input to the shell.
- The attach stream is still raw byte passthrough. A real Windows terminal
  emulator is expected to handle ConPTY terminal negotiation for this phase.
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
- `Ctrl+b d` currently detaches from the streaming attach connection.
- Phase 5 should harden this path with better terminal-size propagation,
  explicit detach status, redirected-stdin behavior, and crash cleanup tests.

This proves ConPTY, IPC, daemon lifetime, input routing, detach, and reattach
before pane rendering becomes complex.

### Phase 6: True Detach and Reattach

- Keep shell lifetime owned by daemon
- Continue reading output while detached
- Store bounded recent output
- Reconnect client to existing session
- Replay visible state on attach
- Resume live streaming

### Phase 7: Virtual Terminal State

- Parse VT output incrementally
- Maintain pane screen grid
- Track cursor state and text attributes
- Handle common clear, cursor movement, and SGR sequences
- Support alternate screen buffer
- Mark dirty regions for render updates

### Phase 8: Render Daemon-Owned Grid

- Client renders daemon-provided pane state
- Avoid direct raw passthrough for normal attached rendering
- Add frame coalescing
- Clip output to pane rectangle

### Phase 9: Windows

- Add window model
- Track active window
- Create, switch, and rename windows
- Render active window only

### Phase 10: Pane Splitting and Layout Rendering

- Add pane layout tree
- Split horizontally and vertically
- Compute pane rectangles
- Draw borders and active pane highlight
- Route input only to focused pane
- Resize ConPTY dimensions when pane rectangles change
- Handle terminal resize events

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
