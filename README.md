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
- [Testing Strategy](docs/testing-strategy.md)
- [Stability Testing](docs/stability-testing.md)
- [Release Gate](docs/release-gate.md)

## Installation

wmux is currently built from source. It targets native Windows APIs including
ConPTY and named pipes, so the runtime and integration tests should be run from
Windows, not WSL.

### Prerequisites

- Windows 10 version 1809 or newer, or Windows 11
- Git
- CMake 3.24 or newer
- Visual Studio 2022 Build Tools with the C++ workload
- vcpkg, or CMake dependency fetching enabled

Install the core toolchain with `winget`:

```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

After installing the Visual Studio Build Tools, open a new PowerShell window so
the updated environment is visible to CMake.

### Dependencies

The recommended dependency manager is vcpkg:

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
C:\dev\vcpkg\bootstrap-vcpkg.bat
C:\dev\vcpkg\vcpkg.exe install cli11 spdlog --triplet x64-windows
```

If you keep vcpkg somewhere else, replace `C:\dev\vcpkg` in the configure
command below with your local path.

### Build From Source

Clone the repository:

```powershell
git clone https://github.com/shreshthkapai/wmux.git
cd wmux
```

Configure and build with vcpkg:

```powershell
cmake -S . -B build-vs -DCMAKE_TOOLCHAIN_FILE=C:\dev\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build-vs --config Debug
```

Run the unit tests:

```powershell
ctest --test-dir build-vs -C Debug --output-on-failure
```

If vcpkg is not available, CMake can fetch the current third-party dependencies
during configure:

```powershell
cmake -S . -B build-vs -DWMUX_FETCH_DEPS=ON
cmake --build build-vs --config Debug
```

### First Run

Start a session. In an interactive PowerShell window this creates the session
and immediately attaches to it:

```powershell
.\build-vs\Debug\wmux.exe new -s main
```

Detach from the session with:

```text
Ctrl+b d
```

Reattach later:

```powershell
.\build-vs\Debug\wmux.exe attach -t main
```

List and stop sessions:

```powershell
.\build-vs\Debug\wmux.exe ls
.\build-vs\Debug\wmux.exe kill-session -t main
```

Stop the daemon after all sessions are closed:

```powershell
.\build-vs\Debug\wmux.exe server stop
```

`wmux server stop` refuses to terminate live sessions. Use `--force` only when
you intentionally want to terminate active panes and their shell processes:

```powershell
.\build-vs\Debug\wmux.exe server stop --force
```

### Key Bindings

```text
Ctrl+b d       detach
Ctrl+b c       new window
Ctrl+b n       next window
Ctrl+b p       previous window
Ctrl+b 0..9    select window by index
Ctrl+b %       split horizontal
Ctrl+b "       split vertical
Ctrl+b arrows  switch pane
Ctrl+b x       kill pane after y/n confirmation
Ctrl+b C-arrows resize pane by 1 cell
Ctrl+b M-arrows resize pane by 5 cells
Ctrl+b E       equalize panes
Ctrl+b [       copy mode
Ctrl+b ]       paste buffer
Ctrl+b :       command mode
```

### Configuration

wmux reads `%USERPROFILE%\.wmux.conf` on daemon startup. The file uses
tmux-style global settings:

```text
set -g prefix C-b
set -g mouse on
set -g default-shell pwsh.exe
set -g status on
# wmux-owned UI highlights use blue by default. Change this to red, a 0-255
# terminal color index, or a truecolor value like "#1e90ff".
set -g accent blue
set -g ui-inherit-terminal-theme on
set -g ui-tmux-style off
set -g border-style smooth
set -g escape-time-ms 50
set -g scrollback-max-lines 10000
set -g paste-buffer-max-bytes 1048576
set -g max-sessions 64
set -g max-windows-per-session 64
set -g max-panes-per-window 64
set -g client-output-queue-max-bytes 8388608
set -g client-output-queue-max-frames 8
bind-key z new-window
bind-key C kill-pane
bind-key Up select-pane-up
unbind-key e
```

If `default-shell` is omitted, wmux resolves the shell in this order:
PowerShell 7 (`pwsh`), Windows PowerShell, then `cmd.exe`. `WMUX_DEFAULT_SHELL`
is treated as a temporary environment override only when no config shell is set.
Explicit `default-shell` paths are validated when the config is parsed; plain
commands such as `pwsh.exe -NoLogo` are resolved through the normal Windows
process search path when panes are spawned.

Resource limits are part of the config contract. The daemon enforces configured
limits for session/window/pane creation, pane raw-output retention, scrollback,
paste buffers, attach render frame size, and attach client output queues. IPC
frame hard caps remain compiled protocol limits because clients must validate
frames before daemon config is known. `log-max-bytes` rotates the active
client/daemon log to `.1` before appending once the configured byte cap would be
exceeded.

Runtime `wmux set -g ...` uses the same validation path as config loading.
Already-attached clients receive live updates for prefix, mouse reporting,
escape timing, status visibility, UI accent/theme settings, and attach
backpressure/render limits.

The renderer keeps shell/application colors from the attached terminal by
default. wmux only styles the multiplexer-owned UI: pane borders, the active
pane highlight, copy-mode selection, and the status line. `accent` accepts
common color names (`blue`, `red`, `green`, etc.), 0-255 terminal color indexes,
and `#RRGGBB` truecolor values. When `ui-tmux-style` is on, wmux uses tmux-like
green/black UI styling instead of the configured accent. It is off by default so
normal terminal themes remain inherited. `border-style smooth` draws tmux-like
single-line pane borders; use `border-style ascii` only as a fallback for
terminals or fonts without box-drawing support.

Prefix key bindings can be customized with tmux-style `bind-key` lines. The key
is the key pressed after the wmux prefix. Supported named keys include `Up`,
`Down`, `Left`, `Right`, `PageUp`, `PageDown`, `Space`, `Tab`, `Enter`,
`Escape`, `Backspace`, and `C-<key>` control-key notation. Supported actions
are the current attach commands:

```text
bind-key z new-window
bind-key C kill-pane
bind-key "prefix x" split-horizontal
bind-key Up select-pane-up
bind-key E equalize-panes
bind-key d detach
bind-key '[' copy-mode
bind-key ']' paste-buffer
bind-key ':' command-prompt
unbind-key e
```

Custom key bindings are validated when the daemon reads the config and are sent
to attach clients through the framed settings path. Runtime commands use the
same validation and update already-attached clients:

```powershell
.\build-vs\Debug\wmux.exe bind-key z new-window
.\build-vs\Debug\wmux.exe bind-key E select-layout -E
.\build-vs\Debug\wmux.exe unbind-key e
```

The same commands also work from command mode, for example
`Ctrl+b : bind-key z new-window`. Runtime option changes update daemon memory;
they do not rewrite `%USERPROFILE%\.wmux.conf`, so daemon restart reloads the
file-backed configuration.

Terminal capability detection can also be overridden when a host terminal has a
known quirk or wmux guesses incorrectly:

```text
set -g terminal-host windows-terminal
set -g terminal-truecolor on
set -g terminal-mouse on
set -g terminal-bracketed-paste on
set -g terminal-extended-keys on
set -g terminal-osc52-clipboard off
set -g terminal-quirk-no-osc52-clipboard on
```

Run `wmux doctor` to see both detected and effective terminal capabilities.
Run `wmux debug-keys` from an interactive Windows terminal to inspect decoded
key, mouse, and paste events without printing pasted text.

### Validation

Run the quick release gate before testing locally:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe -Quick
```

Run the full release gate before calling a build stable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

### Troubleshooting

If CMake cannot find a compiler, install the Visual Studio Build Tools C++
workload and open a fresh PowerShell window.

If CMake cannot find vcpkg packages, check that
`CMAKE_TOOLCHAIN_FILE` points to your actual vcpkg checkout.

If a rebuild fails because `wmux.exe` is still running, stop the daemon:

```powershell
.\build-vs\Debug\wmux.exe server stop --force
```

## Build And Validate

The current skeleton builds a small `wmux` executable with a daemon-owned
ConPTY shell model, explicit detach/reattach, window and pane commands, basic
pane rendering, live attach resize, rendering hardening, command prompt
dispatch, mouse input, click-to-focus, and an explicit
daemon-owned mouse setting:

```bash
cmake -S . -B build
cmake --build build
./build/wmux --help
./build/wmux version
./build/wmux doctor
./build/wmux doctor --json
./build/wmux server status
./build/wmux new -s finance
./build/wmux ls
./build/wmux attach -t finance
./build/wmux new-window
./build/wmux new-window -n logs
./build/wmux list-windows
./build/wmux select-window -t 1
./build/wmux next-window
./build/wmux previous-window
./build/wmux rename-window agents
./build/wmux split-window -h
./build/wmux split-window -v
./build/wmux resize-pane -L
./build/wmux select-layout -E
./build/wmux set -g mouse on
./build/wmux set -g status off
./build/wmux set -g scrollback-max-lines 10000
./build/wmux rename-session -t finance trading
./build/wmux kill-session -t trading
./build/wmux server stop
./build/wmux server stop --force
ctest --test-dir build --output-on-failure
```

On Windows, CMake will produce `wmux.exe`.

The daemon uses separate Windows named-pipe endpoints for command
request/response traffic and long-lived attach streaming. Both endpoints use
the versioned `WMUX` frame header with request IDs and bounded payloads:
command IPC uses Control/Error frames, attach input uses AttachInput frames,
and rendered attach output uses AttachOutput frames. The daemon keeps session
state, window state, ConPTY handles, shell processes, and bounded screen and
scrollback state in the daemon process. `wmux new -s <name>` starts a
daemon-owned shell for the session's initial window using the configured shell
or the default Windows shell resolution order, and `wmux attach -t <name>` opens
a streaming attach connection to the session's active window.

Window commands currently operate on the only live session when exactly one
session exists. They also accept `-t <session>` for explicit daemon commands:

```bash
./build/wmux new-window -t finance -n logs
./build/wmux list-windows -t finance
./build/wmux select-window -t finance:1
./build/wmux next-window -t finance
./build/wmux previous-window -t finance
./build/wmux rename-window -t finance agents
./build/wmux split-window -t finance -h
./build/wmux split-window -t finance -v
./build/wmux resize-pane -t finance:1 -L
./build/wmux select-layout -t finance:1 -E
./build/wmux kill-pane -t finance:1
./build/wmux kill-window -t finance:1
```

`split-window` updates daemon-owned pane state for the active window, spawns a
new ConPTY shell for the created pane, and marks the new pane active.
Interactive attach also supports `Ctrl+b %`, `Ctrl+b "`, `Ctrl+b x`,
`Ctrl+b E`, `Ctrl+b` arrow keys, `Ctrl+b C-arrow` resize-by-one, and
`Ctrl+b M-arrow` resize-by-five. Input is routed only to the active pane.
Attached windows render visible panes with shared tmux-like single-cell
separators, active-pane highlighting, clipped pane text, and a bottom status
line shaped like tmux's default session/window/status layout. The status line
keeps a generous gap before the right-side segment so active window/pane text
does not collide with clock/title text. The renderer avoids drawing pane text
over tiny-pane borders, coalesces continuous output redraws, and has layout
tests for nested geometry, clamped split ratios, and tiny terminal dimensions.
Interactive splits refuse to shrink the active pane below a small TUI-safe
floor; this avoids creating pane sizes that commonly destabilize full-screen
terminal applications. Pane text now comes from a daemon-owned virtual terminal
grid that is updated by the PTY reader thread, not from ad hoc raw-byte
sanitization during every redraw.

`wmux server stop` refuses to stop while live sessions exist. Use
`wmux server stop --force` only when you explicitly want the daemon to terminate
active session runtimes.

Mouse mode is enabled by default for alpha testing and can be changed through
daemon runtime state:

```bash
./build/wmux set -g mouse on
./build/wmux set -g mouse off
```

When mouse mode is on, attached clients enable SGR mouse reporting while the
attach session is active and disable it again on exit. The client parses SGR
mouse sequences, consumes mouse traffic, and sends compact mouse event frames
to the daemon. When mouse mode is off, attached clients do not enable terminal
mouse reporting and do not treat mouse sequences as wmux control traffic. The
daemon maps left-click coordinates to the active window's pane rectangles for
focus. Drag/release sequences are parsed and ignored for layout by design:
mouse mode does not resize panes. Clicks and drags outside panes/status are
no-ops.

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
active-shell passthrough. Client input uses framed AttachInput messages, so
`Ctrl+b d` is an explicit detach event instead of an accidental pipe close.
`Ctrl+b c` creates a new window, while `Ctrl+b n` and `Ctrl+b p` switch the
attached session between independent window shells. The client also sends its
initial terminal size on attach and sends resize frames while attached, so the
daemon can recompute pane rectangles, resize each visible pane's ConPTY, and
redraw the active window. The VT/grid foundation currently handles printable
text, cursor movement, common clear commands, SGR attribute state, and alternate
screen switching. It is still early and does not yet provide full TUI parity,
dirty-region frame diffs, scrollback, or copy-mode selection.

`Ctrl+b :` now enters command prompt mode in the attached client. The prompt is
rendered through the daemon-owned status line and supports basic ASCII text
entry, quoted arguments, backspace, `Esc` cancel, and `Enter` submission.
Command mode dispatches the same tmux-style command names for implemented wmux
features:

```text
rename-session <new>
new-window [-n <name>]
select-window -t <window>
next-window
previous-window
rename-window <new>
split-window -h
split-window -v
resize-pane -L
resize-pane -R
resize-pane -U
resize-pane -D
select-layout -E
kill-pane
kill-window
bind-key <key> <action>
unbind-key <key>
```

Errors and successful command results are shown in the status line. Destructive
command-mode operations are scoped to the attached session. `Ctrl+b x` prompts
for confirmation before executing `kill-pane`; textual `kill-pane` commands
execute directly like tmux commands.

For the current command-mode dispatch check, run:

```powershell
.\scripts\test-command-mode.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script requires an empty daemon, restarts it with a deterministic
`cmd.exe /D /Q` test shell, drives the attach pipe directly with command-mode
frames, verifies invalid-command errors, creates and renames a window, splits a
pane, kills a pane, kills a window, verifies last-pane and last-window refusal,
renames the session, and checks daemon-visible state after each command.

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

For the current mouse click-to-focus check, run:

```powershell
.\scripts\test-mouse-focus.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script requires an empty daemon, restarts it with a deterministic
`cmd.exe /D /Q` test shell, drives the attach pipe directly with mouse-focus
frames, and verifies shell input routes to the pane selected by the click.

For the current daemon mouse setting check, run:

```powershell
.\scripts\test-mouse-setting.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The script requires an empty daemon, verifies the default `mouse: off` status,
sets mouse on and off through command IPC, and confirms attach startup responses
include the expected `mouse_enabled` value.

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
