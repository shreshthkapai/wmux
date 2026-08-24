# Frozen Platform Contract

Phase 5 freezes the OS boundary used by the persistent server and disposable
clients. Shared multiplexer behavior lives in `wmux-core`, `wmux-server`, and
`wmux-client`; native mechanisms live behind four modules in `wmux-platform`:

```text
error       classified and bounded diagnostics
job         shell-job requests, events, stable job tokens, backend dispatch
pane        PTY requests, events, stable pane tokens, backend dispatch
transport   authenticated listener/client byte streams and daemon launch
terminal    normalized input, rendering, clipboard, size, restoration guard
```

Native connection, credential, PTY, and terminal-mode mechanics remain at
process edges while one server loop owns multiplexer state. Injected client and
server platform interfaces allow lifecycle tests to run against deterministic
backends without widening the production contract.

## Pane ordering and ownership

The state owner submits these requests in serialized mux-command order:

```text
SpawnPane -> WritePane / ResizePane -> TerminatePane
```

The backend emits zero or more `PtyOutput` and `BackendError` events, at most
one meaningful `PtyExited`, then exactly one final `PtyClosed`. Exit does not
imply EOF: output produced before child termination may still be unread.
`PtyClosed` is terminal and no event may follow it. A close without an observed
exit is normalized to an unknown exit exactly once. Wrong-pane, duplicate-exit,
and post-close events do not mutate authoritative state or panic the server.

`PlatformPaneId` is a wmux stable token, never a native handle, descriptor, or
PID. The backend owns all ConPTY/PTY controllers, queues, process-tree cleanup,
and native resources. The server polls `try_next_event` nonblockingly under a
64 KiB/one-millisecond per-pane and four-millisecond per-round budget.

## Transport identity and security

`Endpoint` is diagnostic text. `PeerIdentity` is an opaque equality token. A
listener must derive its owner and peer identities from authenticated native
state; the shared server compares them before reading protocol `Hello`.

On Windows the listener owns its named-pipe DACL, lock, factory, current pipe,
and SID verification. On Linux the adapter uses protected Unix-socket
filesystem permissions plus `SO_PEERCRED`; on macOS it uses protected socket
permissions plus `getpeereid`. A client-provided identity is never trusted.
Relevant native contracts are documented by [Windows named-pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights),
[Linux unix(7)](https://man7.org/linux/man-pages/man7/unix.7.html), and
[Apple getpeereid(3)](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/getpeereid.3.html).

## Terminal and errors

`TerminalBackend::enter` returns a drop guard that must restore all terminal
modes on success, error, detach, and shutdown. Input is normalized to semantic
key, paste, cell-coordinate mouse, and resize values; exact key bytes remain
available for application forwarding. Rendering and clipboard operations stay
outside core state.

Adapters classify native failures once as `PlatformErrorKind`. Shared startup
logic never examines Windows error codes or Unix `errno`. Diagnostic payloads
are truncated on a UTF-8 boundary to exactly 4,096 bytes maximum, and conversion
to `io::Error` occurs only at process-facing boundaries.

## Unix adapter realization

`wmux-unix` implements the frozen contract without exposing descriptors, PIDs,
UIDs, signals, or termios values to shared crates. The client and server
composition roots select it under `cfg(unix)`; shared state continues to see
only stable pane IDs, opaque peer identities, and semantic events.

Each pane is its own session and process group. Graceful termination sends
`SIGHUP` to the complete group, forced termination sends `SIGKILL`, and backend
drop kills and reaps all remaining pane trees. PTY output is read in reusable
16 KiB chunks and delivered through a bounded 64-entry queue. Input uses one
bounded ordered writer queue with complete short-write retry. Linux epoll and
macOS kqueue readiness are both consumed through Tokio `AsyncFd`; neither
reactor is visible across the platform boundary.

Before every pane, shell-job, or daemon `exec`, the Unix adapter marks every
descriptor above standard error close-on-exec. Linux uses one
`close_range(..., CLOSE_RANGE_CLOEXEC)` syscall; other Unix targets use a
bounded descriptor-table walk. This prevents a concurrent launch from holding
another pane's PTY, pipe, or socket open.

The reader and direct-child waiter report independently. The adapter coalesces
their results into the same portable ordering used by ConPTY: all queued output,
at most one `PtyExited`, then exactly one `PtyClosed`. The terminal guard saves
the exact original termios state, applies raw mode, and restores the saved state
on every drop path. Detached daemon startup creates a new session and disconnects
standard streams without invoking a shell.

## Conformance and change rule

The real server is exercised over protocol v7 with a memory listener and
scripted backend through this lifecycle:

```text
create -> attach -> type -> split -> resize -> detach
       -> background output -> reattach -> kill pane/session -> kill server
```

The deterministic portable suite has a `platform-lifecycle` case and one
central `EXPECTED_DIFFERENCES` registry. The registry remains empty in Phase 6.
Platform-specific assertions may test mechanics, but shared semantic exceptions
must be registered with the affected platform, observable contract, rationale,
and evidence issue.

## Non-interactive shell jobs

Phase 7 adds a distinct job backend for `run-shell`, `if-shell`, hooks, and
later format expansion. `PlatformJobId` is an opaque wmux token, never a PID or
native handle. A spawn supplies a shell command, optional working directory,
and explicit environment changes. Adapters combine output into bounded chunks,
emit at most one `Exited`, and finish with exactly one terminal `Closed`; no
event may follow close.

Job output uses reusable 16 KiB reads and a bounded 64-entry adapter queue.
Termination and backend drop apply to the complete process tree and reap native
resources. Shared code owns job limits, captured-output limits, foreground
command-queue suspension, branch selection, and hook dispatch. Native code
owns shell selection, redirection, process groups or Job Objects, waiting, and
cleanup.

Changing any public platform request/event, ordering rule, identity obligation,
error classification, terminal-restoration behavior, or performance budget
requires the same commit to update contract tests, deterministic conformance,
this document, and the aggregate fingerprint. No shared crate may import
Windows or Unix APIs, native crates, handles, descriptors, SIDs, UIDs, signals,
or runtime-specific native objects.

## Phase 5 verification evidence

The final Windows verification on 2026-08-21 produced:

- 283 workspace unit tests passed, including 35 server and 36 Windows-adapter
  tests;
- format and workspace clippy with warnings denied passed;
- every fuzz binary compiled and passed clippy with warnings denied;
- 14 portable conformance cases passed with aggregate fingerprint
  `f71b72b35879a1c6`;
- shared core, platform, protocol, config, server library, client library, and
  conformance compile checks passed;
- native-import source audits found no matches in shared library sources;
- the complete release performance gate passed, with `platform-dispatch`
  completing 10,000,000 operations in 68.906 ms at 145,124,611
  operations/second, zero allocations, and zero violations.

Extra in-scope work was limited to making benchmark allocation measurement
per-thread and refreshing the fuzz lockfile after `wmux-platform` gained its
OS-neutral Tokio stream dependency.

## Phase 6 verification evidence

The final local verification on 2026-08-22 produced:

- 285 Windows workspace tests passed, including 36 server and 36
  Windows-adapter tests;
- 289 Linux Unix/shared tests passed, including 39 `wmux-unix` unit tests and
  one real socket/PTY lifecycle integration test;
- format checks and warnings-denied clippy passed on Windows and for every
  Unix/shared package on Linux;
- both Unix binaries compiled and the release Linux CLI completed detached
  create, list, split, pane listing, session destruction, server shutdown, and
  endpoint cleanup through the production composition roots;
- all 14 portable conformance cases produced aggregate fingerprint
  `f71b72b35879a1c6` on both Windows and Linux, with no expected differences;
- `x86_64-apple-darwin` and `aarch64-apple-darwin` checks passed for
  `wmux-unix`, `wmux`, and `wmux-server`;
- the native seam audit found no Unix API, descriptor, native credential,
  signal constant, or `wmux_unix` dependency in shared library sources; and
- the complete Windows release performance gate passed, with
  `platform-dispatch` completing 10,000,000 operations in 67.546 ms at
  148,047,914 operations/second with zero allocations and zero violations.

The Linux/macOS `native-unix` CI matrix is configured, but no remote runner was
available during this local pass. Native macOS execution therefore remains
CI-gated rather than reported as verified.

Narrow extra work was limited to entering the configured Tokio runtime when a
state-owner thread registers a PTY and marking both freshly allocated PTY
descriptors close-on-exec. This prevents concurrent child launches from holding
another pane's PTY open past process-group termination.

## Phase 7 verification evidence

The final local verification on 2026-08-22 produced:

- 343 Windows workspace tests and 349 Linux Unix/shared tests passed;
- format checks and warnings-denied clippy passed on Windows and Linux;
- portable protocol conformance produced all 16 case fingerprints and aggregate
  `d5670ad858ef5735` on both systems, with no expected differences;
- real Windows and Linux control clients created a session, listed state, ran
  a native shell job, observed structured ordered records, shut down the
  daemon, and released the endpoint;
- Intel and Apple Silicon macOS compile checks passed for the complete Unix
  adapter, server, and client composition;
- the shared-source audit found no native API, descriptor, handle, credential,
  process, or platform-crate dependency outside composition roots; and
- the unchanged full release performance gate passed, with platform dispatch
  completing 10,000,000 operations in 69.533 ms at 143,816,398
  operations/second and zero measured allocations or violations.

Windows jobs use an explicitly inherited combined-output pipe, suspended
`CreateProcessW`, a kill-on-close Job Object assigned before resume, and one
terminal close event. Unix jobs use `/bin/sh -c`, a dedicated process group,
combined bounded output, group termination, and the same descriptor sanitation
as pane and daemon children. Shared job IDs, limits, command suspension,
capture, hooks, and control records remain OS-neutral.

## Phase 8 verification evidence

The Phase 8 local gate on 2026-08-22 produced:

- 350 Windows and 354 Linux workspace tests with strict workspace clippy;
- identical 16-case portable aggregate `d5670ad858ef5735` twice on both hosts;
- identical full stress aggregate `d537f5686435cc2e` twice on both hosts;
- isolated real Windows and Linux lifecycle tests covering abrupt disconnect,
  authoritative reattach, native descendant cleanup, endpoint removal, and
  restart on the same endpoint;
- exact native terminal-mode round-trip tests plus shared client tests proving
  one guard release on every recoverable attach exit;
- 30-second sanitizer fuzz smoke for command, protocol, and terminal input on
  Linux with no crash artifact;
- Intel and Apple Silicon compile checks for conformance, the Unix adapter,
  server, and client; and
- the unchanged Windows full release performance gate.

Stress exposed one shared ordering defect: close after a resize hold could omit
deferred final output. The server now publishes that frame before close and a
focused regression protects the ordering contract. Client error cleanup and
the Windows crate's cross-target gate were also tightened. These changes do
not move native state into shared crates or alter the frozen platform API.

Actual native macOS execution remains pending. The authoritative evidence and
status vocabulary are recorded in [beta-core-gate.md](beta-core-gate.md),
[compatibility-matrix.md](compatibility-matrix.md), and
[known-differences.md](known-differences.md).

On Unix, a socket path that remains after its listener exits is semantically an
absent server, not a terminal client error. The client startup policy may start
one replacement daemon, while the endpoint binder retains authority for owner
validation and stale socket/lock cleanup.
