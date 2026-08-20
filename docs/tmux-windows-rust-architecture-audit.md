# tmux Architecture Audit and Windows/Rust Equivalence Plan

This document explains what tmux is doing internally, what architectural ideas should be copied for a Windows-first implementation, and which Unix mechanisms cannot be reproduced literally on native Windows. Where exact Unix behavior is impossible, the document gives the Windows-native equivalent that should be used to preserve tmux-level behavior, performance, reliability, and consistency.

The goal is not to make a program that merely resembles tmux. The goal is a native Windows terminal multiplexer with tmux's model: persistent server, detachable clients, sessions, windows, panes, layouts, command language, key tables, copy mode, scrollback, status line, and control protocol.

## Executive Summary

tmux is a persistent terminal virtualization server.

The user-facing `tmux` binary is both client launcher and server launcher. Most invocations start as a short-lived client. That client connects to a long-running server over local IPC. If no server exists and the command allows server startup, the client starts the server. The server owns all durable state: sessions, windows, panes, child processes, scrollback, options, layouts, paste buffers, jobs, and attached clients.

The most important design decisions are:

- Keep all persistent state in one authoritative server.
- Make clients disposable views onto server state.
- Treat each pane as a virtual terminal, not just a subprocess.
- Parse child process output into an internal screen grid.
- Render from internal grid state to attached client terminals.
- Keep command execution serialized through a server-side command queue.
- Use narrow platform backends for process, PTY, IPC, terminal size, signals/events, and credentials.

For Windows, do not build a POSIX emulation clone. Build the same core model with a native Windows backend:

- Unix PTY becomes ConPTY.
- `fork/exec` becomes `CreateProcessW` attached to ConPTY.
- Unix signals become Windows process/job/control-event equivalents.
- Unix sockets/imsg become named pipes or Windows AF_UNIX plus a versioned protocol.
- `waitpid` becomes process-handle wait/completion.
- Unix process groups become Job Objects and process groups where possible.
- termios/raw tty becomes Windows console input/output mode plus VT mode.
- terminfo becomes a terminal capability layer targeting modern VT terminals first.

Rust can implement this. Rust is not the blocker. The hard parts are terminal emulation fidelity, ConPTY quirks, async IO correctness, process tree cleanup, redraw performance, and compatibility testing.

## What tmux Is Doing

### Process Model

tmux has two process roles:

- Client process: starts from the user's terminal, parses command-line flags, connects to the server, identifies itself, sends commands, and if attached, passes terminal input/output between the user terminal and the server.
- Server process: persistent daemon that owns sessions, windows, panes, child processes, layout state, grid state, options, hooks, paste buffers, jobs, and all attached clients.

The server starts only when required. A command such as `tmux new-session` can start the server. A command such as `tmux attach` may fail if no server exists, depending on flags.

The client/server split is the reason detach/attach works. Detaching destroys only the client view. Pane processes remain children managed by the server.

### Event Model

tmux is event-driven. It uses libevent around:

- client socket reads/writes
- server socket accepts
- pane PTY reads/writes
- timers
- signals
- command queue progress
- status redraw intervals
- resize timers
- child process exits
- jobs started by tmux commands

The server is largely single-threaded and deterministic. This matters. tmux avoids broad shared-state concurrency by funneling state mutation through the event loop and command queue.

For a Rust rewrite, this maps well to:

- one server runtime
- async IO for clients and panes
- explicit state owner task or actor
- serialized command execution
- carefully bounded background workers only for slow external jobs or filesystem operations

### IPC Model

tmux clients talk to the server with a versioned local protocol. Upstream tmux uses OpenBSD `imsg` over Unix sockets. The protocol includes:

- protocol version
- identify messages
- command messages
- detach/exit/shutdown messages
- resize messages
- shell/exec messages
- file read/write messages

Important properties:

- The protocol is versioned.
- The client identifies terminal name, tty name, cwd, environment, flags, pid, features, stdin/stdout fds, and terminfo data.
- The server decides whether the client becomes attached, stays control-mode, executes a command, exits, or starts a shell.
- File transfer between client and server is modeled explicitly through protocol messages.

For Windows, keep the same idea, not the same Unix transport:

- Use a versioned binary or framed protocol.
- Use named pipes or Windows AF_UNIX.
- Authenticate using Windows user/session security, pipe security descriptors, process token checks, or explicit peer identity exchange.
- Keep protocol payloads OS-neutral where possible.
- Keep handle passing out of the core protocol unless a Windows backend specifically needs it.

### Server State Model

The server owns the object graph:

```text
server
  clients
  sessions
    winlinks
      windows
        panes
          process/backend
          input parser
          screen/grid/history
          modes
          layout cell
  paste buffers
  options
  key tables
  command queues
  jobs
  hooks/notifications
```

This state graph is tmux's real product.

### Session

A session is a named workspace.

It contains:

- session id
- name
- current working directory
- creation/activity/attach times
- current window link
- last-window stack
- tree of window links
- status position and status line count
- session options
- environment
- attached client count
- references

Sessions do not directly own windows in a simple list. They own `winlink` objects that point to windows. This allows a window to be linked into multiple sessions.

Behavior to preserve:

- session creation/destruction
- attach/detach
- rename
- switch current window
- last-window behavior
- session groups
- window linking/unlinking
- per-session options/environment
- activity timestamps and alerts

### Window

A window is a container for panes.

It contains:

- window id
- name
- active pane
- list of panes
- last-pane stack
- z-index list
- layout tree
- saved layout tree for zoom
- size and pending size
- activity/creation time
- alert flags
- options
- references
- links back to all winlinks that reference it

Behavior to preserve:

- multiple panes per window
- active pane
- pane history stack
- linked windows across sessions
- automatic/manual window naming
- alert propagation
- layout save/restore
- zoom
- resize policy
- floating panes if implementing current tmux features

### Winlink

A winlink is the per-session reference to a window.

It contains:

- index inside a session
- owning session
- referenced window
- alert flags
- visited flag

This is why one real window can appear in different sessions at different indexes.

Behavior to preserve:

- base-index
- renumbering
- session-local window index
- linked windows
- per-session alert state for a shared window

### Pane

A pane is the heaviest runtime object.

It is not merely a subprocess. It contains:

- pane id
- owning window
- pane options
- layout cell
- saved layout cell
- size and position
- flags
- shell/argv/cwd
- child process identity
- PTY/ConPTY handle or fd
- async IO event/buffer
- terminal input parser
- current screen
- base screen
- status screen
- scrollback grid
- modes stack
- copy/search state
- prompt state
- pipe-pane state
- resize queue
- cached styles/colours
- visible range data
- scrollbar state
- process exit status/dead time

This is the core design point: a pane owns a virtual terminal. Child output is parsed into internal terminal state. The attached client terminal is only a renderer, not the source of truth.

Behavior to preserve:

- pane creation/destruction
- split layout placement
- process spawn
- process output parsing
- process input writing
- pane resize
- scrollback
- alternate screen
- copy mode
- search
- paste
- pipe-pane
- remain-on-exit
- respawn-pane
- capture-pane
- synchronized panes

### Screen and Grid

tmux keeps a virtual screen for each pane.

The screen contains:

- title/path metadata
- grid pointer
- cursor position
- cursor style/colour
- scroll region
- terminal modes
- saved cursor and saved grid for alternate screen
- tab stops
- selection
- images if enabled
- hyperlinks
- write list
- progress bar

The grid contains:

- width/height
- history size and limit
- scrollback metadata
- array of grid lines

Each grid line stores compact cell entries and extended entries. Simple cells are stored compactly. Cells needing richer Unicode, colours, attributes, or hyperlinks use extended storage. This compact representation matters for scrollback memory and redraw speed.

Behavior to preserve:

- Unicode cell width
- combining characters
- wide characters
- attributes
- 256-colour and RGB colour
- default colours
- hyperlinks
- alternate screen
- scrollback history
- wrapped lines
- cleared/dead lines
- efficient cell storage

### Terminal Input Parser

tmux parses byte streams emitted by child programs. It implements a VT-style state machine:

- ground state
- escape sequences
- CSI
- OSC
- DCS
- character sets
- attributes
- cursor movement
- erase/insert/delete
- scrolling
- mouse-related sequences
- bracketed paste
- clipboard queries/replies
- terminal feature negotiation

Parser output mutates the pane screen/grid through screen-write operations.

This parser is security-sensitive and correctness-sensitive. Child processes can emit arbitrary bytes. A rewrite should fuzz this parser heavily.

### Screen Write Layer

The parser does not directly write to clients. It calls screen-write functions that mutate internal screen state and mark regions dirty.

Responsibilities:

- draw characters
- move cursor
- set/reset attributes
- clear screen/line
- insert/delete characters/lines
- scroll regions
- handle alternate screen
- handle synchronized updates
- maintain dirty state for redraw

This separation is important:

```text
child output bytes
  -> input parser
  -> screen-write operations
  -> pane screen/grid
  -> redraw builder
  -> client tty renderer
```

### Layout Engine

tmux uses a tree of layout cells.

Cell types:

- left-right split node
- top-bottom split node
- leaf pane cell

Each cell stores:

- type
- flags
- parent
- size
- offset
- saved size/offset
- pane pointer for leaves
- child cells for nodes

Behavior to preserve:

- split-window horizontal/vertical
- resize-pane
- select-layout
- even-horizontal
- even-vertical
- main-horizontal
- main-vertical
- tiled
- custom layout strings
- zoom
- pane movement/swap/join/break
- border calculations
- floating panes if supported

The layout tree must be deterministic. Given the same commands and terminal size, it should produce the same pane geometry.

### Redraw Engine

tmux renders from internal state to each attached client terminal.

The redraw path:

1. Determine client session/current window.
2. Recalculate window size if needed.
3. Build a scene from panes, borders, status line, overlays, popups, menus, messages.
4. Compute visible ranges and obscured regions.
5. Compare desired output with client tty state.
6. Emit minimal VT sequences and characters.

Important features:

- Dirty flags avoid unnecessary redraw.
- Redraw is per client because each client may have different size/features.
- tmux tracks client tty cursor, attributes, modes, colours, and scroll region to emit less output.
- Slow clients get backpressure and may have output discarded/rebuilt.

For performance, this area matters more than language choice.

### Client TTY Layer

An attached client has a `tty` object.

It stores:

- client pointer
- terminal size
- pixel cell size if known
- cursor state
- output area state
- current tty mode
- foreground/background
- scroll region
- input buffer
- output buffer
- timers
- termios state on Unix
- terminal capability data
- mouse state
- feature flags

Responsibilities:

- read user terminal input
- parse keys/mouse
- write rendered output
- track terminal capabilities
- handle raw mode
- handle resize
- handle output buffering/backpressure

For Windows:

- use console input/output handles for local terminal clients when appropriate
- enable VT input/output modes
- read input as bytes/events depending on terminal mode
- output VT sequences to Windows Terminal/conhost
- prefer modern VT mode over legacy console APIs

### Key Handling

tmux converts terminal input into internal key codes.

It supports:

- prefix key
- root table
- prefix table
- copy-mode tables
- custom key tables
- repeatable bindings
- mouse events
- extended keys
- bracketed paste

Key bindings map keys to command lists.

Behavior to preserve:

- prefix behavior
- table switching
- repeat-time
- send-prefix
- bind-key/unbind-key
- copy-mode key tables
- mouse key formats
- user-defined bindings

### Command System

Commands are table-driven.

Each command defines:

- canonical name
- alias
- argument parser
- usage string
- source target
- destination target
- flags
- execution callback

Commands are parsed into command lists and executed through command queues.

Important command subsystems:

- parser for tmux command language
- args parser
- target resolution
- command queue
- hooks
- after-hooks
- error reporting
- format expansion
- asynchronous continuations for commands that wait

Behavior to preserve:

- command names/aliases
- command chaining
- quoting/escaping
- target syntax
- formats
- hooks
- wait-for
- if-shell/run-shell async behavior
- source-file
- command-prompt

### Target Resolution

tmux commands often need to resolve a target:

- session
- window
- pane
- client
- mouse location
- current/last/next/previous
- exact id
- index
- name

Target resolution is complex because commands may run from:

- attached client
- detached client
- control client
- config file
- hook
- command prompt
- mouse binding
- no client context

Preserve this as a dedicated subsystem. Do not scatter target lookup across command implementations.

### Options

tmux has hierarchical options:

- server options
- session options
- window options
- pane options
- user options
- hooks as options

Options have types:

- string
- number
- key
- colour
- flag
- choice
- style
- command
- arrays

Behavior to preserve:

- global options
- local overrides
- inheritance
- set-option/show-options
- set-window-option compatibility
- user `@option`
- option expansion in formats/status
- hooks

### Formats

tmux has a powerful format expansion system used by:

- status line
- display-message
- list commands
- hooks
- command options
- window names
- conditionals

Formats include:

- variables
- modifiers
- comparisons
- substitutions
- time formatting
- pane/window/session/client defaults
- jobs for shell output in formats

For exact tmux-like behavior, this is a major subsystem, not a helper function.

### Status Line

The status line is rendered as a screen-like object.

It supports:

- left/right status
- window list
- styles
- intervals
- messages
- prompts
- command prompt
- search prompt
- mouse ranges
- multiple status lines

The status line is per client because size, session, prompt, and message state differ per client.

### Modes

Panes can enter modes. A mode overlays or replaces normal pane behavior.

Important modes:

- copy mode
- view mode
- tree mode
- choose/buffer/client/customize modes
- clock mode

Mode objects define callbacks for:

- init/free
- resize
- key handling
- command handling
- drawing
- format additions
- screen selection

Copy mode is large because it implements:

- scrolling
- cursor movement
- search
- selection
- copy pipe
- vi/emacs key behavior
- rectangle selection
- line numbers
- mouse selection

### Paste Buffers

tmux stores paste buffers in the server.

Buffers have:

- name
- data
- size
- creation time
- automatic/manual marker
- order

Behavior to preserve:

- set-buffer
- load-buffer
- save-buffer
- paste-buffer
- choose-buffer
- delete-buffer
- automatic buffer naming/order
- clipboard integration where available

### Jobs

tmux can run external jobs for commands and formats:

- `run-shell`
- `if-shell`
- status/format jobs
- hooks

Jobs are asynchronous and integrated with the command queue/event loop.

For Windows:

- use `CreateProcessW`
- collect stdout/stderr via pipes
- integrate with async runtime
- handle quoting/shell invocation explicitly for PowerShell/cmd/default shell

### Control Mode

Control mode lets external programs drive tmux over a machine-readable protocol.

It is not the same as an attached terminal client. It receives structured notifications and can subscribe to pane output.

Behavior to preserve:

- `%begin/%end/%error`
- pane output notifications
- session/window/pane change notifications
- subscriptions
- pause-after behavior
- no-output behavior
- client flags

If you want compatibility with tmux integrations, control mode needs careful implementation.

### Notifications and Hooks

tmux emits notifications for server state changes and runs hooks.

Examples:

- session created/closed/renamed
- window linked/unlinked/renamed/layout changed
- pane mode changed
- paste buffer changed/deleted
- client attached/detached/session changed

Hooks are part of the command system and options system.

### File Transfer

tmux supports commands that read/write files from the client side or server side. Because the server and client may have different environments and permissions, tmux models file read/write over protocol messages.

For Windows, preserve this concept:

- avoid assuming server cwd equals client cwd
- make file operations explicit
- stream large files
- report errors through protocol

### Environment Handling

tmux maintains environment sets:

- global environment
- session environment
- client-provided environment during identify
- child process environment for panes/jobs

When spawning panes, tmux constructs the child environment from session/global/client data, sets tmux-specific variables, and sets shell/default-command behavior.

For Windows:

- environment is UTF-16 for `CreateProcessW`
- variable names are case-insensitive by convention
- PATH/PATHEXT semantics differ
- shell choice matters

Still keep an OS-neutral environment model internally, with backend conversion at spawn time.

## Windows-First Architecture

### Recommended Rust Module Shape

```text
crates/
  mux-core/
    server_state
    sessions
    windows
    panes
    layouts
    screen
    grid
    vt_parser
    redraw
    commands
    options
    formats
    key_tables
    paste
    control_mode
    hooks

  mux-protocol/
    framed_ipc
    messages
    versioning
    serialization

  mux-platform/
    traits
      PtyBackend
      ProcessBackend
      IpcBackend
      TerminalClientBackend
      Clock/EventBackend
      CredentialBackend

  mux-platform-windows/
    conpty
    create_process
    job_object
    named_pipe
    console_modes
    windows_terminal_features
    overlapped_io

  mux-platform-unix/
    pty
    fork_exec
    signals
    unix_socket
    termios

  mux-server/
    daemon/server runtime

  mux-client/
    cli client

  mux-cli/
    argument parsing
```

The core must never directly import Windows APIs. The Windows backend can be first and best, but the core interfaces should be OS-neutral.

### Core Backend Interfaces

#### Pane PTY Backend

Required operations:

- spawn pane process
- read output bytes
- write input bytes
- resize terminal
- close input
- terminate process
- query exit status
- identify process
- clean process tree

Unix implementation:

- PTY master/slave
- fork/exec
- termios
- ioctl resize
- signals/process groups

Windows implementation:

- ConPTY
- pipes
- `CreateProcessW`
- `ResizePseudoConsole`
- process handle wait
- Job Object cleanup

#### IPC Backend

Required operations:

- connect client to server
- create server listener
- accept clients
- send framed message
- receive framed message
- authenticate peer
- handle protocol version mismatch

Unix implementation:

- AF_UNIX socket
- filesystem permissions
- peer uid/gid
- optional fd passing

Windows implementation:

- named pipe or Windows AF_UNIX
- security descriptor
- process token/user SID/session id
- explicit handle inheritance only inside backend

#### Terminal Client Backend

Required operations:

- enter raw/alternate application mode as needed
- read key/input bytes
- write VT output
- query size
- receive resize events
- restore terminal on exit
- enable mouse/bracketed paste/focus modes

Unix implementation:

- termios raw mode
- tty fd
- SIGWINCH
- terminfo

Windows implementation:

- console input/output modes
- ENABLE_VIRTUAL_TERMINAL_PROCESSING
- ENABLE_VIRTUAL_TERMINAL_INPUT
- window-size polling or console events
- Windows Terminal/conhost behavior detection

## What Cannot Be Done Literally in Rust on Native Windows

These limitations are not Rust limitations. Rust can call the Windows API. The limitations come from native Windows not being Unix.

The correct approach is to preserve tmux behavior through native equivalents.

### Unix PTY

Literal tmux mechanism:

- PTY master/slave
- child process connected to slave
- server reads/writes master
- terminal size via ioctl
- Unix tty semantics

Cannot be done literally on native Windows because Windows does not have Unix PTYs.

Windows equivalent:

- ConPTY via `CreatePseudoConsole`
- server owns ConPTY and pipe handles
- child process spawned with ConPTY attached through startup attributes
- resize through `ResizePseudoConsole`
- read/write through pipe async IO

Implementation requirement:

- Define `PtyBackend`.
- Treat ConPTY as the pane terminal backend.
- Hide ConPTY handles from core state.
- Normalize resize, EOF, close, and exit events into core pane events.

Expected fidelity:

- High for modern terminal applications.
- Not byte-identical to Unix PTY behavior.
- Good enough for exact tmux-like pane UX if tested carefully.

### fork/exec

Literal tmux mechanism:

- fork server process
- in child, set environment/cwd/termios
- exec shell or command

Cannot be done literally on native Windows because Windows does not have `fork`.

Windows equivalent:

- `CreateProcessW`
- explicit environment block
- explicit cwd
- startup attribute list for ConPTY
- handle inheritance list

Implementation requirement:

- Build command line quoting deliberately.
- Store original argv in pane state.
- Provide shell-specific launch strategies:
  - PowerShell
  - cmd.exe
  - direct executable
  - WSL bridge if intentionally supported

Expected fidelity:

- Exact at mux command layer.
- Child process launch semantics differ by shell.

### Unix Signals

Literal tmux mechanism:

- SIGCHLD for child exit
- SIGWINCH for terminal resize
- SIGTERM/SIGHUP/SIGKILL for termination
- SIGTSTP/SIGCONT for suspend/resume
- foreground process group signals

Cannot be done literally on native Windows because Windows has a different process and console control model.

Windows equivalent:

- process handle waits for exit
- console resize events or polling
- `TerminateProcess` and Job Object termination
- `GenerateConsoleCtrlEvent` where process groups/console attachment allow it
- explicit mux-level suspend/detach behavior instead of Unix job-control identity

Implementation requirement:

- Define semantic events:
  - pane exited
  - client resized
  - terminate pane
  - interrupt pane
  - detach client
  - server shutdown
- Map those events to platform APIs.

Expected fidelity:

- `detach`, `kill-pane`, `kill-window`, `resize`, `exit` can be reliable.
- Unix job-control edge cases cannot be identical.

### Process Groups and Foreground TTY

Literal tmux mechanism:

- process groups
- sessions
- controlling terminal
- foreground/background job control

Cannot be done literally on native Windows.

Windows equivalent:

- Job Objects for process tree ownership
- `CREATE_NEW_PROCESS_GROUP` where useful
- process handles for lifecycle
- console control events where possible

Implementation requirement:

- Every pane process tree belongs to a Job Object.
- Killing a pane kills the job.
- Track process root and descendants by job, not by Unix process group.

Expected fidelity:

- Better cleanup reliability on Windows than ad hoc process killing.
- Not identical to Unix shell job control.

### termios Raw Mode

Literal tmux mechanism:

- `tcgetattr`
- `tcsetattr`
- raw/cbreak tty modes
- restore terminal on exit

Cannot be done literally on native Windows.

Windows equivalent:

- `GetConsoleMode`
- `SetConsoleMode`
- enable/disable line input, echo, processed input
- enable VT input/output modes
- restore saved console modes on exit

Implementation requirement:

- Terminal client backend owns console mode transitions.
- Always restore modes on clean exit.
- Add crash recovery best effort where possible.

Expected fidelity:

- High in Windows Terminal and modern conhost.

### terminfo/ncurses Capabilities

Literal tmux mechanism:

- terminfo database
- terminal capabilities by `$TERM`
- feature overrides

Cannot be done literally in standard native Windows because Windows terminal capability discovery is not terminfo-based.

Windows equivalent:

- capability profile layer
- detect Windows Terminal/conhost/VS Code terminal/other terminal where possible
- assume modern VT baseline when VT mode is enabled
- allow user overrides

Implementation requirement:

- Internal renderer targets capability flags, not raw OS assumptions.
- Provide defaults for:
  - Windows Terminal
  - conhost
  - mintty/MSYS terminal
  - VS Code terminal
  - fallback dumb terminal

Expected fidelity:

- High for modern VT terminals.
- Exact terminfo behavior is not meaningful on native Windows.

### Unix Socket Peer Credentials and File Permissions

Literal tmux mechanism:

- AF_UNIX socket path
- filesystem permissions
- peer uid/gid
- fd passing

Cannot be done exactly on all native Windows versions/transports.

Windows equivalent:

- named pipe with security descriptor
- user SID/session identity checks
- optional Windows AF_UNIX where supported
- explicit protocol-level identity
- no core dependency on fd passing

Implementation requirement:

- Server endpoint must be per-user by default.
- Prevent cross-user attach unless explicitly allowed.
- Model permissions in mux terms, not Unix mode bits.

Expected fidelity:

- Security can be robust.
- Unix permission semantics are not identical.

### File Descriptor Passing

Literal tmux mechanism:

- pass fds over Unix socket through imsg

Cannot be done literally with Windows named pipes.

Windows equivalent:

- avoid handle passing in the core protocol
- perform open/read/write in the side that owns the file
- stream data over protocol
- duplicate handles only inside trusted Windows backend paths if needed

Implementation requirement:

- Keep file transfer as protocol messages.
- Do not require OS handle passing for normal commands.

Expected fidelity:

- User-facing behavior can match.
- Transport mechanism differs.

### waitpid and SIGCHLD

Literal tmux mechanism:

- `waitpid(WAIT_ANY, ..., WNOHANG|WUNTRACED)`
- child status macros

Cannot be done literally on Windows.

Windows equivalent:

- wait on process handles
- IOCP/threadpool waits
- query exit code
- normalize to pane exit status

Implementation requirement:

- Process backend emits `PaneExited { id, status }`.
- Core does not care whether it came from SIGCHLD or process handle wait.

Expected fidelity:

- High for process exit.
- Unix stopped/continued status is not identical.

### Unix Shell Semantics

Literal tmux mechanism:

- default shell from `$SHELL` or passwd
- login shell argv convention
- POSIX shell quoting
- Unix cwd/path rules

Cannot be done literally on Windows.

Windows equivalent:

- configurable default shell
- default to PowerShell or user's configured shell
- direct executable launch when command is argv
- shell-command launch through selected shell adapter
- Windows path/env conversion

Implementation requirement:

- Model shell adapters explicitly:
  - PowerShell adapter
  - cmd adapter
  - WSL adapter
  - MSYS/Git Bash adapter
  - direct exec adapter

Expected fidelity:

- tmux command behavior can match.
- Child shell semantics depend on chosen shell.

### Unicode Width

Literal tmux mechanism:

- UTF-8 locale
- wcwidth/utf8proc-like width handling
- tmux internal UTF-8 cells

Windows issue:

- console rendering and Unicode width behavior may differ between terminals and fonts.

Windows equivalent:

- keep internal UTF-8/Unicode width table
- prefer Unicode Standard/utf8proc-compatible width
- expose compatibility overrides
- test against Windows Terminal behavior

Implementation requirement:

- Width calculation is core logic.
- Renderer may apply terminal-specific corrections if unavoidable.

Expected fidelity:

- Good but not perfect across every font/terminal.

### Applications Expecting Unix TTY Semantics

Literal tmux mechanism:

- Unix applications run under Unix PTY

Cannot be guaranteed for native Windows apps.

Windows equivalent:

- native apps run under ConPTY
- Unix apps should run through WSL/MSYS/Cygwin backend if supported

Implementation requirement:

- Do not pretend native Windows ConPTY is a Unix PTY.
- Provide explicit WSL integration later if needed.

Expected fidelity:

- Native Windows CLI apps can work well.
- Unix-specific programs need Unix-like environments.

## Exactness Policy

Use this rule:

```text
Exact at the tmux semantic layer.
Native equivalent at the OS mechanism layer.
Documented difference only when the OS makes identical behavior impossible.
```

Semantic layer that should be exact:

- sessions
- windows
- panes
- winlinks
- layouts
- copy mode
- scrollback
- status
- options
- key tables
- command language
- target syntax
- hooks
- paste buffers
- attach/detach
- control mode

Mechanism layer that cannot be exact:

- Unix PTY
- fork/exec
- Unix signals
- Unix process groups
- termios
- Unix socket credentials
- fd passing
- shell/job control

The mechanism layer should be hidden behind platform backends.

## Performance Requirements

To be tmux-fast, focus on:

- zero-copy or low-copy pane output ingestion where reasonable
- streaming parser
- compact grid cells
- dirty region tracking
- redraw diffing
- batched terminal writes
- backpressure for slow clients
- bounded memory per pane
- scrollback limits
- async IO without per-byte tasks
- deterministic single-owner server state

Avoid:

- full redraw on every byte
- storing screen as strings
- reparsing terminal state from output
- unbounded buffers
- global locks around hot paths
- POSIX emulation layers for Windows backend
- spawning one thread per pane unless proven acceptable

## Robustness Requirements

To be tmux-robust:

- server survives client disconnects
- pane processes survive detach
- client terminal modes are restored
- pane process trees are cleaned reliably
- protocol is versioned
- parser is fuzzed
- command parser is fuzzed
- malformed IPC messages are rejected
- resize storms are coalesced
- slow clients cannot stall pane processing
- huge output cannot exhaust memory unboundedly
- server shutdown is orderly
- logs exist for IPC, process, parser, redraw, and backend errors

Windows-specific robustness:

- every pane has a Job Object
- every inherited handle is intentional
- ConPTY pipe closure is handled correctly
- process exit and pipe EOF races are tested
- terminal resize races are tested
- console mode restoration is best effort
- named pipe security is tested under multiple users/sessions

## Testing Strategy

### Unit Tests

- grid cell storage
- Unicode width
- layout tree operations
- command parsing
- target resolution
- options inheritance
- format expansion
- key table transitions
- paste buffer ordering

### Parser Tests

- VT escape sequences
- CSI attributes
- OSC title/clipboard
- alternate screen
- scroll regions
- wide characters
- combining characters
- invalid UTF-8
- large paste
- bracketed paste
- mouse sequences

### Golden Tests

Input:

- byte stream from child process
- terminal size
- initial screen

Expected:

- final grid
- cursor position
- dirty regions
- rendered client output where stable

### Stress Tests

- massive output
- rapid resize
- many panes
- many clients
- detach/attach loops
- kill pane during output
- server shutdown during active pane
- slow client output buffer
- ConPTY EOF while process still exits
- process exits before pipe EOF
- nested shells

### Compatibility Tests

Windows shells:

- PowerShell
- Windows PowerShell
- cmd.exe
- WSL shell
- Git Bash/MSYS2 if supported

Terminals:

- Windows Terminal
- conhost
- VS Code integrated terminal
- mintty/MSYS terminal if supported

## Implementation Phases

### Phase 1: Minimal Native Server

- server process
- client process
- named-pipe IPC
- version handshake
- create session
- attach/detach
- spawn one shell pane through ConPTY
- pass input/output
- basic resize

### Phase 2: Real Pane Model

- VT parser
- screen/grid
- scrollback
- renderer
- dirty redraw
- pane lifecycle
- process exit handling

### Phase 3: tmux Object Model

- sessions
- windows
- panes
- winlinks
- split panes
- layout tree
- active pane/window/session switching

### Phase 4: Command System

- command parser
- command queue
- target resolution
- core commands
- key tables
- prefix handling

### Phase 5: User Features

- status line
- copy mode
- paste buffers
- options
- formats
- hooks
- control mode

### Phase 6: Hardening

- fuzzing
- stress tests
- named pipe security
- ConPTY race testing
- renderer optimization
- crash recovery behavior
- logging/diagnostics

### Phase 7: Cross-OS Backend

- Unix PTY backend
- Unix socket backend
- termios backend
- signal backend
- compatibility matrix

## Rust Feasibility

Rust can implement this.

Good Rust fits:

- server state ownership
- parser safety
- protocol decoding safety
- async IO
- process lifecycle abstractions
- enum-based command/message types
- property tests/fuzzing
- memory-safe grid implementation

Areas requiring care:

- Windows API unsafe blocks
- ConPTY startup attributes
- handle ownership
- overlapped IO
- cancellation/drop behavior
- process/job object lifetime
- high-throughput buffers
- avoiding async task explosion

Recommended crates/types to consider later:

- `windows` crate for Windows APIs
- Tokio or another async runtime if it supports the required Windows IO model cleanly
- custom arena/index IDs for server object graph
- `bytes`-style buffers for IPC and pane IO
- `arbitrary`/libFuzzer/AFL style fuzzing for parsers

Do not let Rust's borrow checker force a bad architecture. Use stable IDs and centralized state stores for sessions/windows/panes/clients. Avoid pointer-heavy graph ownership.

Example core identity model:

```rust
struct SessionId(u32);
struct WindowId(u32);
struct PaneId(u32);
struct ClientId(u32);

struct ServerState {
    sessions: SlotMap<SessionId, Session>,
    windows: SlotMap<WindowId, Window>,
    panes: SlotMap<PaneId, Pane>,
    clients: SlotMap<ClientId, Client>,
}
```

The exact container is a design choice, but the principle is important: stable IDs are easier than trying to mirror tmux's raw pointer graph.

## Final Position

A native Windows tmux-equivalent is possible.

The correct promise is:

```text
tmux semantics exactly where OS-independent.
Windows-native equivalents where Unix mechanisms do not exist.
No POSIX emulation dependency.
Server/client architecture like tmux.
Terminal state owned by the server like tmux.
Performance from native async IO, compact grids, and redraw diffing.
Robustness from deterministic server state, versioned IPC, fuzzed parsers, and Job Object cleanup.
```

The wrong promise is:

```text
Byte-for-byte Unix tmux behavior on native Windows for every PTY, signal,
process group, and shell job-control edge case.
```

That is not a Rust limitation. It is an OS semantics limitation.

Build the tmux architecture. Make the Windows backend native. Keep the core OS-neutral from the beginning. That gives the best path to Windows-first quality and later cross-OS compatibility.
