# Phase 5 Cross-OS Platform Contract Freeze Design

**Date:** 2026-08-21

**Status:** Approved for implementation by the Phase 5 execution request

**Roadmap task:** Task 5 of 11 in `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md`

## Goal

Freeze the smallest OS-neutral interface that can support the existing Windows
implementation and the upcoming Linux/macOS backend. `wmux-server` and
`wmux-client` must compile without importing `wmux-windows`; native selection
is limited to their binary composition roots. A mock platform must be able to
exercise the complete persistent-server lifecycle through the real framed
protocol.

The contract must preserve the existing performance model: pane output remains
bounded and chunked, one state owner mutates mux state, and adapter dispatch
must not introduce a task, lock, frame, redraw, or allocation per byte.

## Scope

Phase 5 includes:

- classified, bounded platform errors;
- opaque endpoint, peer, and pane identities;
- async byte-stream listener and client transport interfaces;
- semantic terminal input, resize, output, clipboard, and restoration;
- pane spawn/write/resize/terminate requests plus output/exit/closure/error
  events;
- injected server and client construction;
- Windows adapters for the frozen interfaces;
- a mock-backed real-protocol lifecycle test;
- portable lifecycle conformance and one centralized expected-differences
  registry;
- portable compile checks and a dynamic-dispatch performance gate.

Phase 5 does not implement AF_UNIX, Unix PTYs, termios, signals, Linux process
groups, macOS peer credentials, or UI styling. Those begin in Tasks 6 and 7.

## Researched Model

The local references are fixed at:

- tmux `7b833d07d9f1b58343fc88d7de3c2e0bd9f9aa8c`;
- zellij `82c4a24d701ecf9a48aa01bcc5c0bb3882747fe7`.

Tmux `client.c`, `server.c`, `proc.c`, `server-client.c`, and `spawn.c` keep
socket creation, peer credentials, daemonization, PTY process mechanics, and
terminal modes at the process edge. Accepted client messages and pane bytes are
then routed through the authoritative server loop.

Zellij's `ServerOsApi` and `ClientOsApi`, platform-specific
`os_input_output_{windows,unix}.rs` adapters, and integration-test fake OS APIs
demonstrate constructor injection and mock-backed lifecycle testing. Wmux copies
that seam placement, but not zellij's broad interface: the Phase 5 contract
contains only behavior already required by both ConPTY and Unix PTYs.

Official platform behavior constrains the adapters, not shared semantics:

- Windows named-pipe DACLs and impersonated client tokens establish the peer
  identity before protocol processing.
- Linux `SO_PEERCRED` and macOS `getpeereid` can produce the same opaque
  `PeerIdentity` at connection time.
- ConPTY and Unix PTYs both accept character-cell resize requests, emit byte
  output, report child exit, and eventually close their output stream.

References:

- <https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights>
- <https://learn.microsoft.com/en-us/windows/win32/console/creating-a-pseudoconsole-session>
- <https://man7.org/linux/man-pages/man7/unix.7.html>
- <https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/getpeereid.3.html>

## Architecture

### Deep platform modules

`wmux-platform` becomes four focused modules:

```text
error       PlatformErrorKind + bounded PlatformError
pane        stable pane tokens, requests/events, PtyBackend
transport   endpoint/peer values, boxed IPC stream, listener/client interfaces
terminal    normalized terminal input and terminal restoration/output interface
```

These modules define interfaces only. They contain no Windows or Unix imports,
native handles, descriptors, process identifiers used as handles, or runtime
ownership policy.

### Error contract

Adapters map native errors once into:

```rust
pub enum PlatformErrorKind {
    NotFound,
    AlreadyRunning,
    Busy,
    PermissionDenied,
    Disconnected,
    TimedOut,
    InvalidInput,
    InvalidData,
    Unsupported,
    Other,
}

pub struct PlatformError {
    pub kind: PlatformErrorKind,
    pub operation: &'static str,
    pub message: String,
}
```

Messages are capped at 4 KiB on a UTF-8 boundary. Shared startup policy uses
`PlatformErrorKind`, never Windows error 231 or Unix `errno`. Conversion to
`io::Error` happens only at CLI/server process boundaries.

### Pane interface

`PlatformPaneId` remains a wmux-issued opaque token. Its numeric value is for
deterministic adapter lookup only and is never a handle, descriptor, or PID.

```rust
pub trait PtyBackend: Send {
    fn submit(&mut self, request: PlatformRequest) -> PlatformResult<()>;
    fn try_next_event(
        &mut self,
        pane: PlatformPaneId,
    ) -> PlatformResult<Option<PlatformEvent>>;
}
```

Requests remain `SpawnPane`, `WritePane`, `ResizePane`, and `TerminatePane`.
Events are `PtyOutput`, `PtyExited`, explicit `PtyClosed`, and `BackendError`.
`PtyExited` may carry a portable numeric exit code; signal numbers and native
status words never cross the seam. `PtyClosed` means no later event for that
pane. Exit and closure are separate because both ConPTY and Unix PTYs may drain
final output after process exit.

The server polls by pane under its existing byte/time/round budgets. The
backend owns queues and native controllers. The server never sees a Tokio
receiver or a ConPTY controller. A notifier merely wakes the owner loop; it
does not mutate state.

### Transport interface

`BoxedIpcStream` is an OS-neutral Tokio `AsyncRead + AsyncWrite` byte stream.
Framing remains entirely in `wmux-protocol` and server/client code.

```rust
pub trait ServerListener: Send {
    fn endpoint(&self) -> &Endpoint;
    fn owner_identity(&self) -> &PeerIdentity;
    fn accept(&mut self) -> PlatformFuture<'_, AcceptedConnection>;
}

pub trait ServerPlatform: Send {
    fn bind(&mut self) -> PlatformResult<Box<dyn ServerListener>>;
    fn create_pty_backend(
        &mut self,
        notifier: PlatformNotifier,
    ) -> PlatformResult<Box<dyn PtyBackend>>;
}

pub trait ClientTransport: Send + Sync {
    fn endpoint(&self) -> &Endpoint;
    fn connect(&self) -> PlatformFuture<'_, BoxedIpcStream>;
    fn spawn_server(&self, spec: &DaemonSpec) -> PlatformResult<()>;
}
```

An accepted stream carries an opaque peer identity. The shared server compares
it with the listener owner before reading `Hello`; the adapter must acquire the
identity from the authenticated native connection. Endpoint display text is
diagnostic only.

The Windows listener owns its server lock, named-pipe factory, current accept
instance, DACL, and owner SID. Dropping it releases all listener state. The
later Unix listener will own its socket path, permissions, lock, and unlink
guard behind the same interface.

### Terminal interface

The client terminal adapter returns normalized keys plus exact application
bytes, paste, cell-coordinate mouse input, and resize events. It also owns raw
mode restoration, render transactions, clipboard writes, and size queries.

```rust
pub trait TerminalBackend: Send + Sync {
    fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>>;
    fn read_input(&self) -> PlatformResult<Option<TerminalInput>>;
    fn write_output(&self, bytes: &[u8]) -> PlatformResult<()>;
    fn write_render_transaction(
        &self,
        bytes: &[u8],
        synchronized: bool,
    ) -> PlatformResult<()>;
    fn write_clipboard_text(&self, text: &str) -> PlatformResult<()>;
    fn size(&self) -> PlatformResult<TerminalSize>;
}
```

`TerminalModeGuard` restores the host terminal on drop. `TerminalInput::Key`
uses platform-owned key types which the client converts to protocol v6; the
Windows crate no longer depends on `wmux-protocol` merely to normalize input.

### Composition roots

`wmux-server` exports `run_with_platform(Box<dyn ServerPlatform>)` and contains
no native imports in its library. Its tiny binary chooses
`WindowsServerPlatform` under `cfg(windows)`; later binaries select the Unix
adapter without editing the owner loop.

`wmux-client` becomes a library plus tiny binary. The library accepts
`Arc<dyn ClientTransport>` and `Arc<dyn TerminalBackend>`. Its attach loop,
framing, startup retry policy, and protocol behavior are shared. The binary
constructs Windows adapters under `cfg(windows)`.

### Mock lifecycle

A memory listener uses Tokio duplex streams and opaque test identities. A
scripted PTY backend records semantic requests and queues semantic events.
Through the real server, protocol codec, and command queue, the contract test
performs:

```text
create -> attach -> type -> split -> resize -> detach
       -> background output -> reattach -> kill pane
       -> kill session -> kill server
```

The test proves session persistence across disposable streams, authoritative
background output, explicit pane termination, peer rejection before `Hello`,
and listener/server cleanup. Focused tests cover malformed/out-of-order backend
events so no adapter failure can panic the owner.

## Conformance and Expected Differences

`wmux-conformance` gains a deterministic platform-lifecycle case based on the
same semantic request/event vocabulary. Expected shared-semantic differences
are registered only in one `EXPECTED_DIFFERENCES` table keyed by stable ID and
platform set. The Phase 5 registry is empty: named pipes versus AF_UNIX,
ConPTY versus PTY, SID versus UID, and Job Objects versus process groups are
mechanism differences, not mux-semantic exceptions.

Any later nonempty entry must name the observable contract, affected platform,
rationale, and evidence issue. Scattered `cfg` assertions are forbidden.

## Performance

The owner continues to drain at most 64 KiB or one millisecond per pane and
four milliseconds per output round. `PtyBackend::try_next_event` is nonblocking
and must not allocate for an empty poll or a resize request. The release suite
adds `platform-dispatch`: ten million trait-object resize submissions through
a deterministic in-memory adapter, at least 20 million operations/second with
zero measured allocations. Existing gates may not be weakened.

## Testing and Compile Gates

Implementation follows red-green-refactor. Coverage includes:

- error classification and diagnostic bounds;
- opaque identity equality and non-leakage;
- terminal key/input conversions;
- Windows adapter request/event mapping;
- exit-before-EOF, EOF-before-exit, backend-error, duplicate-close, missing
  pane, and forced cleanup;
- real-protocol mock lifecycle and wrong-peer rejection;
- client attach using memory transport and terminal adapters;
- deterministic conformance and expected-difference lookup;
- release dispatch performance and allocation count;
- `cargo check` for core, protocol, config, platform, server library, client
  library, and conformance with no native crate selected;
- source audits rejecting native imports in shared crates.

## Acceptance Criteria

Phase 5 is complete when:

1. shared server/client libraries import no `wmux-windows` or native APIs;
2. their binaries are the only platform selection points;
3. server owner state uses only `PtyBackend` requests/events;
4. the client attach loop uses injected transport and terminal interfaces;
5. pane exit and final stream closure are distinct, deterministic events;
6. endpoint, peer, pane, and error values expose no native handles;
7. a mock platform passes the full real-protocol lifecycle;
8. portable conformance has one centralized expected-differences registry;
9. portable shared-crate checks pass on the CI OS matrix;
10. all workspace tests, fuzz checks, conformance, formatting, clippy, and
    release performance gates pass on Windows without weakening thresholds.

