# Frozen Platform Contract

Phase 5 freezes the OS boundary used by the persistent server and disposable
clients. Shared multiplexer behavior lives in `wmux-core`, `wmux-server`, and
`wmux-client`; native mechanisms live behind four modules in `wmux-platform`:

```text
error       classified and bounded diagnostics
pane        PTY requests, events, stable pane tokens, backend dispatch
transport   authenticated listener/client byte streams and daemon launch
terminal    normalized input, rendering, clipboard, size, restoration guard
```

The model follows tmux revision
`7b833d07d9f1b58343fc88d7de3c2e0bd9f9aa8c`: native connection, credential,
PTY, and terminal-mode mechanics remain at process edges while one server loop
owns mux state. It follows zellij revision
`82c4a24d701ecf9a48aa01bcc5c0bb3882747fe7` for injected server/client OS APIs
and fake-backed lifecycle tests, while keeping wmux's interface smaller.

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
and SID verification. Linux must use protected Unix-socket filesystem
permissions plus `SO_PEERCRED`; macOS must use protected socket permissions
plus `getpeereid`. A client-provided identity is never trusted. Relevant native
contracts are documented by [Windows named-pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights),
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

## Conformance and change rule

The real server is exercised over protocol v6 with a memory listener and
scripted backend through this lifecycle:

```text
create -> attach -> type -> split -> resize -> detach
       -> background output -> reattach -> kill pane/session -> kill server
```

The deterministic portable suite has a `platform-lifecycle` case and one
central `EXPECTED_DIFFERENCES` registry. The Phase 5 registry is empty.
Platform-specific assertions may test mechanics, but shared semantic exceptions
must be registered with the affected platform, observable contract, rationale,
and evidence issue.

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

The researched reference revisions were tmux
`7b833d07d9f1b58343fc88d7de3c2e0bd9f9aa8c` and zellij
`82c4a24d701ecf9a48aa01bcc5c0bb3882747fe7`. Extra in-scope work was limited
to making benchmark allocation measurement per-thread and refreshing the fuzz
lockfile after `wmux-platform` gained its OS-neutral Tokio stream dependency.
