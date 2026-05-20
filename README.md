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

The current skeleton builds a small `wmux` executable with the Phase 5
daemon-owned ConPTY shell and raw interactive attach path:

```bash
cmake -S . -B build
cmake --build build
./build/wmux --help
./build/wmux version
./build/wmux server status
./build/wmux new -s finance
./build/wmux ls
./build/wmux attach -t finance
./build/wmux rename-session -t finance trading
./build/wmux kill-session -t trading
./build/wmux server stop
./build/wmux server stop --force
ctest --test-dir build --output-on-failure
```

On Windows, CMake will produce `wmux.exe`.

The daemon uses separate Windows named-pipe endpoints for command
request/response traffic and long-lived attach streaming. It keeps session
state, ConPTY handles, shell processes, and a bounded recent output buffer in
the daemon process. `wmux new -s <name>` starts a daemon-owned
`powershell.exe -NoLogo -NoProfile` shell, and `wmux attach -t <name>` opens a
streaming attach connection to it.

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

Interactive attach is intentionally still raw output passthrough. It is good
enough to prove ConPTY process ownership and first shell IO. Client input uses
small length-prefixed attach frames so `Ctrl+b d` is an explicit detach event
instead of an accidental pipe close. The client also sends its initial terminal
size on attach so the daemon can resize ConPTY before replaying output, but
live resize events, the richer terminal model, and pane rendering come in later
phases.

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
