# wmux

`wmux` is a planned native Windows terminal multiplexer inspired by the workflow
model of tmux.

The goal is to provide persistent, pane-based, session-oriented terminal
workflows directly on Windows without depending on WSL. It is intended for
developers who keep long-running shells, servers, logs, build jobs, agents, and
project workspaces alive across terminal restarts.

## Product Shape

`wmux` is:

- a terminal-native multiplexer
- a persistent process orchestrator
- a session, window, and pane manager
- keyboard-first, with first-class mouse and copy-mode support
- designed for native Windows terminal workflows

`wmux` is not:

- a terminal emulator
- an IDE
- a GUI desktop app
- a replacement for Windows Terminal, Alacritty, WezTerm, or VSCode terminal
- a cloud collaboration platform
- a full tmux clone in the initial release

Existing terminal emulators remain responsible for the outer terminal window.
`wmux` provides the orchestration layer inside them.

## Initial Scope

The first usable version is intentionally narrow:

1. Sessions, windows, and panes
2. Detach and reattach persistence
3. Mouse support
4. Copy mode and Windows clipboard integration

The most important early validation is:

```text
1. Start wmux
2. Create a session
3. Run a long process
4. Detach
5. Close the terminal
6. Reopen the terminal
7. Reattach
8. Confirm the process is still running
```

## Engineering Priority

Performance and stability are hard product requirements.

```text
stability and speed >>> everything else
```

The interactive path must not randomly block, lag, leak handles, grow memory
without bounds, or crash under normal development workloads.

See:

- [Product Overview](docs/product-overview.md)
- [Technical Roadmap](docs/technical-roadmap.md)
- [Engineering Principles](docs/engineering-principles.md)

## Build And Validate

The current skeleton builds a small `wmux` executable with the Phase 9C
daemon-owned ConPTY shell, raw detach/reattach path, window commands, and
basic pane rendering plus live attach resize and rendering hardening:

```bash
cmake -S . -B build
cmake --build build
./build/wmux --help
./build/wmux version
./build/wmux server status
./build/wmux new -s finance
./build/wmux ls
./build/wmux attach -t finance
./build/wmux new-window -n logs
./build/wmux list-windows
./build/wmux rename-window agents
./build/wmux split-window -h
./build/wmux split-window -v
./build/wmux rename-session -t finance trading
./build/wmux kill-session -t trading
./build/wmux server stop
./build/wmux server stop --force
ctest --test-dir build --output-on-failure
```

On Windows, CMake will produce `wmux.exe`.

The daemon uses separate Windows named-pipe endpoints for command
request/response traffic and long-lived attach streaming. It keeps session
state, window state, ConPTY handles, shell processes, and a bounded recent
output buffer in the daemon process. `wmux new -s <name>` starts a daemon-owned
`powershell.exe -NoLogo -NoProfile` shell for the session's initial window, and
`wmux attach -t <name>` opens a streaming attach connection to the session's
active window.

Window commands currently operate on the only live session when exactly one
session exists. They also accept `-t <session>` for explicit daemon commands:

```bash
./build/wmux new-window -t finance -n logs
./build/wmux list-windows -t finance
./build/wmux rename-window -t finance agents
./build/wmux split-window -t finance -h
./build/wmux split-window -t finance -v
```

`split-window` updates daemon-owned pane state for the active window, spawns a
new ConPTY shell for the created pane, and marks the new pane active.
Interactive attach also supports `Ctrl+b %`, `Ctrl+b "`, and `Ctrl+b` arrow
keys. Input is routed only to the active pane. Attached windows now render all
visible panes with ASCII borders, an active-pane highlight, clipped pane text,
and a status line. The current renderer avoids drawing pane text over
tiny-pane borders, coalesces continuous output redraws, and has layout tests
for nested geometry, clamped split ratios, and tiny terminal dimensions. Pane
text now comes from a daemon-owned virtual terminal grid that is updated by the
PTY reader thread, not from ad hoc raw-byte sanitization during every redraw.

`wmux server stop` refuses to stop while live sessions exist. Use
`wmux server stop --force` only when you explicitly want the daemon to terminate
active session runtimes.

For the current Windows shell-lifetime stability check, run:

```powershell
.\scripts\test-kill-session-cleanup.ps1 -Wmux .\build-vs\Debug\wmux.exe -Iterations 20
```

The script creates and kills uniquely named sessions, then verifies that no new
daemon-owned `powershell.exe`, `pwsh.exe`, or `cmd.exe` processes remain. It
does not stop the daemon or touch sessions it did not create.

For the detach/reattach persistence check, run:

```powershell
.\scripts\test-detach-reattach.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script requires an empty daemon, restarts it with a deterministic
`cmd.exe /D /Q` test shell, starts a long-running loop through the attach pipe,
simulates terminal closure by dropping the pipe, reattaches, verifies newer
output was captured while detached, then verifies an explicit detach still
leaves the session attachable.

Interactive attach now uses daemon-rendered basic pane frames rather than direct
active-shell passthrough. Client input uses small length-prefixed attach frames
so `Ctrl+b d` is an explicit detach event instead of an accidental pipe close.
`Ctrl+b c` creates a new window, while `Ctrl+b n` and `Ctrl+b p` switch the
attached session between independent window shells. The client also sends its
initial terminal size on attach and sends resize frames while attached, so the
daemon can recompute pane rectangles, resize each visible pane's ConPTY, and
redraw the active window. The VT/grid foundation currently handles printable
text, cursor movement, common clear commands, SGR attribute state, and alternate
screen switching. It is still early and does not yet provide full TUI parity,
dirty-region frame diffs, scrollback, or copy-mode selection.

For the current interactive window switching check, run:

```powershell
.\scripts\test-window-switching.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script requires an empty daemon, restarts it with a deterministic
`cmd.exe /D /Q` test shell, drives the attach pipe directly, creates a window
with the same command frame used by `Ctrl+b c`, switches with the same command
frames used by `Ctrl+b p` and `Ctrl+b n`, and verifies each window keeps
independent shell state.

For the current interactive pane focus check, run:

```powershell
.\scripts\test-pane-focus.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script requires an empty daemon, restarts it with a deterministic
`cmd.exe /D /Q` test shell, drives the attach pipe directly, splits panes with
the same command frames used by `Ctrl+b %` and `Ctrl+b "`, switches focus with
the same command frames used by `Ctrl+b` arrow keys, and verifies each pane
keeps independent shell state while the attach stream renders a bordered pane
layout.

For development from WSL or Linux, the same IPC abstraction uses a Unix-domain
socket fallback so daemon lifecycle behavior can be validated before ConPTY
work begins. The Linux fallback is for development only; Windows named pipes
remain the product target.

The build prefers installed `CLI11` and `spdlog` CMake packages. If they are not
installed, configure with dependency fetching enabled:

```bash
cmake -S . -B build -DWMUX_FETCH_DEPS=ON
```

This workspace currently falls back to a tiny CLI11 compatibility header because
CLI11 is not installed locally. The fallback is intentionally narrow; install the
real dependency or enable `WMUX_FETCH_DEPS` before expanding CLI behavior beyond
the current command contract.
