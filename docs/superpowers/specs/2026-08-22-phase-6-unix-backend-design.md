# Phase 6 Unix Backend Design

## Goal

Phase 6 adds a production Unix platform adapter for Linux and macOS without
changing wmux's server-authoritative multiplexer semantics or the platform
contract frozen in Phase 5. A native Unix build must be able to start the
server, create and attach to sessions, run shells in PTYs, split and resize
panes, detach and reattach, preserve background output, and clean up complete
pane process trees.

UI/UX, mux chrome, new shared commands, and post-beta packaging are out of
scope. Pane application cells and the host terminal theme remain authoritative.

## Researched Model

The design follows tmux revision
`7b833d07d9f1b58343fc88d7de3c2e0bd9f9aa8c`:

- `server.c` binds a filesystem AF_UNIX socket under a restrictive umask;
- `proc.c` derives peer credentials from the accepted native socket;
- `spawn.c` allocates a PTY, creates a session and controlling terminal, and
  keeps terminal parsing and mux state outside the PTY wrapper;
- process lifecycle and signals stay at the server's native edge.

It follows zellij revision
`82c4a24d701ecf9a48aa01bcc5c0bb3882747fe7` for a Rust Unix adapter using
`openpty`, a nonblocking master registered with the async reactor, a dedicated
process waiter, terminal-mode restoration, and normalized client input.

The native rules come from Linux `pty(7)`, `unix(7)`, `termios(3)`, `setsid(2)`,
and the controlling-terminal and window-size ioctl documentation, plus Apple's
`openpty(3)` and `getpeereid(3)` manuals. Rust child setup must obey
`CommandExt::pre_exec`'s async-signal-safety requirements.

## Architecture

Add one `wmux-unix` crate compiled only on Unix. It is a deep adapter at the
existing `wmux-platform` seam. Its public interface is limited to:

```rust
pub struct UnixServerPlatform;
pub struct UnixClientTransport;
pub struct UnixTerminalBackend;
```

Each type implements the corresponding frozen `wmux-platform` interface. The
crate privately owns socket paths and locks, native file descriptors, child
PIDs and process groups, terminal attributes, signal handling, and OS-specific
credential calls. No Unix type, descriptor, UID, PID, signal, or errno crosses
the seam.

The implementation is split by responsibility:

```text
wmux-unix/src/lib.rs       public adapter exports
wmux-unix/src/platform.rs  frozen-trait composition and error classification
wmux-unix/src/ipc.rs       endpoint, lock, socket permissions, peer identity
wmux-unix/src/pty.rs       PTY ownership, async I/O, request/event ordering
wmux-unix/src/process.rs   shell setup, daemon launch, groups, termination/reap
wmux-unix/src/terminal.rs  raw-mode guard, input, output, size, clipboard
```

Signal behavior belongs in `pty.rs`, `process.rs`, or `terminal.rs` where it is
consumed. A shallow pass-through `signals.rs` module will not be created.

## IPC And Identity

The endpoint is a pathname socket, never an abstract Linux socket. Prefer
`$XDG_RUNTIME_DIR/wmux/wmux.sock` only when the runtime directory is owned by
the effective user and is not writable by group or other users. Otherwise use
a UID-qualified directory beneath the system temporary directory. The wmux
directory is mode `0700`; the socket is mode `0600`.

Binding is serialized by an owner-only lock file. A live endpoint rejects a
second server. An unreachable endpoint may be removed only after validating
that the socket and lock are owned by the current effective UID and are not
links or unexpected file types. Listener drop removes only the exact socket
and lock it owns.

Accepted connections are authenticated before protocol decoding. Linux reads
`SO_PEERCRED`; macOS calls `getpeereid`. The native UID is converted to an
opaque `PeerIdentity`, and the server accepts only the listener owner's
identity. Filesystem permissions are defense in depth and never replace native
peer verification.

The client uses Tokio `UnixStream`. Server startup creates a new native session
with standard streams disconnected from the invoking terminal. A small reaper
waits for the launched server without blocking the client or leaking a zombie.

## PTY And Process Lifecycle

Pane spawn allocates one master/slave PTY pair with the final requested size.
The child creates a new session and process group, makes the slave its
controlling terminal, connects it to standard input/output/error, applies the
requested working directory and environment, and executes the requested
program. With no explicit command it executes `$SHELL` when it is an absolute
path, otherwise `/bin/sh`. Only async-signal-safe native calls may run in the
post-fork `pre_exec` closure.

The parent closes the slave and owns the master through an RAII descriptor.
The master is nonblocking and registered with Tokio `AsyncFd`, mapping to epoll
on Linux and kqueue on macOS. One reader task emits chunks of at most 16 KiB
into the existing bounded event path. One ordered writer task handles short
writes and readiness without blocking the state owner. Input and output bytes
are never dropped.

Resize applies `TIOCSWINSZ` to the master. Graceful termination sends `SIGHUP`
to the pane process group; forced termination sends `SIGKILL`. Explicit pane,
session, and server kills target the group, not only the shell PID, so ordinary
descendants cannot survive. The direct child is always reaped.

The adapter preserves the Phase 5 event contract:

```text
zero or more PtyOutput / BackendError
at most one meaningful PtyExited
exactly one final PtyClosed
no event after PtyClosed
```

Child exit does not imply PTY EOF. The reader drains output until EOF before
closing. If EOF arrives without a usable wait status, the adapter emits an
unknown exit once before `PtyClosed`. Adapter drop force-terminates and reaps
all remaining pane groups.

## Terminal Client

`UnixTerminalBackend::enter` snapshots stdin termios, enters raw mode, and
returns a guard whose `Drop` restores the exact saved attributes. Restoration
must also occur after input errors, detach, protocol failure, and unwind.

Crossterm 0.28 provides event decoding, but the Unix adapter owns conversion to
the same semantic `TerminalInput` values used on Windows. Raw key bytes remain
available for application forwarding. `SIGWINCH` becomes a resize event, while
the existing 500 ms size check remains a fallback. Terminal size is read from
`TIOCGWINSZ` and clamped to at least one cell.

Rendering uses one locked, complete write per render transaction and retains
the existing synchronized-output wrapper. Clipboard output uses a bounded OSC
52 transaction so copy mode works without importing desktop clipboard APIs.

## Dependencies And Portability

Use the existing Rust 2021 workspace and Rust 1.96 CI toolchain. Tokio remains
the reactor and socket runtime. Add `nix` 0.31 with only the `fs`, `process`,
`signal`, `socket`, and `term` features plus `libc` for the small credential or
ioctl differences not exposed portably by `nix`. Continue using crossterm 0.28
for terminal events. Native `cfg` blocks live only inside `wmux-unix`.

The client and server binaries select `wmux-windows` on Windows and
`wmux-unix` on Unix. Shared libraries remain native-free and keep the same
interfaces and protocol version.

## Testing And Verification

All production behavior is implemented test-first. Native Unix tests exercise:

- endpoint selection, owner-only modes, second-server rejection, safe stale
  recovery, and cleanup;
- Linux peer UID verification and macOS `getpeereid` verification;
- PTY shell round-trip, custom command/cwd/environment, large ordered writes,
  resize storms, EOF/exit/close ordering, and output after child exit;
- graceful and forced descendant cleanup plus backend-drop cleanup;
- raw-mode entry and exact restoration through a test PTY;
- semantic key/paste/mouse/resize conversion, batched output, and OSC 52;
- real client/server create, attach, split, resize, detach, background output,
  reattach, pane/session kill, and server shutdown.

Windows verification remains required for every Unix change. Linux native
tests run locally in an isolated Linux environment and in CI. macOS compiles
through cross-checking where possible and runs the same native suite in
`macos-latest` CI. The existing portable conformance fingerprint must remain
unchanged on Windows, Linux, and macOS.

## Exit Gate

Phase 6 is complete only when:

1. Windows workspace tests, formatting, clippy, conformance, and performance
   gates still pass.
2. Linux builds the real binaries and passes the complete native lifecycle and
   portable conformance suite.
3. macOS builds the same Unix adapter and passes its native lifecycle and
   portable conformance suite in CI.
4. Shared source audits find no native imports or leaked native identities.
5. All Phase 6 changes and verification evidence are committed directly to
   `main`.
