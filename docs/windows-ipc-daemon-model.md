# Windows IPC and Daemon Lifecycle Model

Phase 3 hardens wmux's Windows control plane without moving platform policy
into the core. The server remains the sole state owner; this document defines
how Windows discovers that server, authenticates clients, starts it outside a
terminal tab's lifetime, and shuts it down without bypassing the owner loop.

## Lifecycle constraints

Command metadata determines whether a missing server may be started. Startup
is serialized with an owner lock and stale endpoint state is removed before
launch. The server is independent from the launching client, and shutdown stops
new work before draining client output and leaving the owner loop.

wmux adds one deliberate Windows-specific mechanism. A
Windows Terminal tab may be placed in a kill-on-close Job Object that does not
permit breakaway. Microsoft's Job Object rules mean that
`CREATE_BREAKAWAY_FROM_JOB` cannot guarantee survival in that case. The client
therefore asks the local Windows WMI provider to create the server in the
current user's security context. The provider, rather than the terminal-tab
process, becomes the creator. The client passes its complete environment via
`Win32_ProcessStartup.EnvironmentVariables` so pane shells retain the launching
environment. The PowerShell process used to issue the local WMI call is only a
short-lived bootstrap and never owns server or pane lifetime.

## Endpoint identity and authorization

The endpoint key is the current process token's user SID, not `USERNAME`,
`USER`, a display name, or another mutable string. An optional sanitized wmux
instance suffix may distinguish test or user-selected instances, but it never
replaces the SID.

The first named-pipe instance is created with
`FILE_FLAG_FIRST_PIPE_INSTANCE`. Every instance receives the same protected
DACL, granting access to the owning SID and no generic Everyone or Anonymous
ACE. After a client sends its hello, the server verifies the connected client
token's user SID before registering the client or accepting a command. Failure
is fail-closed.

The endpoint DACL is the primary authorization check. Token verification is a
second check against accidental descriptor drift, endpoint pre-creation, and
future transport changes. Protocol `pid` fields are diagnostic only and never
serve as identity.

## Startup policy

The OS-neutral `wmux-cli` crate parses process arguments and classifies each
server command as one of:

- `StartIfMissing`: bare wmux, `new-session`, `attach-session`, and
  `start-server`.
- `RequireExisting`: read-only inspection commands, destructive commands such
  as `kill-server`, and every other command unless explicitly classified.

The client attempts a connection first. `StartIfMissing` commands serialize a
daemon launch and retry
the authenticated handshake. `RequireExisting` commands return one stable,
human-readable no-server diagnostic without starting anything.

`--help`, `-h`, `--version`, and `-V` are local CLI operations and never touch
IPC. `wmux-client` retains console mode, terminal input, renderer output, and
attachment mechanics; it does not own argument policy.

## Shutdown transaction

`kill-server` is a serialized core command, but process exit is a server-runtime
transaction:

1. The owner marks the runtime as shutting down and rejects new registration.
2. The async listener is told to stop accepting connections.
3. The owner terminates all pane Job Objects through the Windows backend.
4. The requesting client receives its command reply; attached clients receive
   an explicit shutdown message.
5. Each connection writer drains its terminal control reply and exits normally.
6. The async runtime joins or boundedly aborts connection tasks.
7. The owner thread exits, the listener and lock are dropped, and the process
   returns from `main` normally.

No worker calls `process::exit`. Client disconnection remains a detach event and
does not enter this sequence.

Process exit and ConPTY output completion are also ordered explicitly. The
process waiter is the authoritative source of the exit code. On its first exit
event, the server closes the process-facing ConPTY endpoints while leaving the
output reader in forwarding mode. Buffered terminal output drains to EOF; once
the event channel closes, the platform pane is released while the dead core
pane and its authoritative screen remain available. A simultaneous generic I/O
failure cannot replace a concrete process status or append a second logical
exit marker.

## Windows ownership rules

- The daemon process is created by the local WMI provider in the calling user's
  security context and is not a child lifetime of the terminal tab.
- The daemon owns the named-pipe listener, server lock, state-owner thread, and
  all pane backends.
- Each pane owns one kill-on-close Job Object. Pane, window, session, and server
  destruction terminate the corresponding pane jobs idempotently.
- Clients own only their console-mode restoration guard and their IPC handles.
  Closing or crashing a client cannot close a pane Job Object.
- Server exit closes the endpoint and lock only after control replies drain or
  the bounded shutdown deadline expires.

## Phase 3 verification record

Native tests use unique instance suffixes and bounded waits. They verify the
owner-only DACL and token SID comparison, first-pipe exclusion, stale and live
lock ownership, WMI persistence, disconnect/reattach independence, one logical
ConPTY exit, complete shutdown frames, lock release, and same-endpoint restart.
The protocol constants are tied to `docs/ipc-protocol.md` by a compile-time
included unit test so version, magic, and maximum payload documentation cannot
drift silently.

The exit/EOF regression was evidence-led: before the lifecycle fix, the native
test timed out because the pseudoconsole remained owned after process exit, and
the server coalescing test lost a concrete exit code when a generic indication
arrived afterward. The fix closes the ConPTY endpoint on the first process exit,
drains buffered output, prefers the concrete status, and drops the completed
platform pane. Other Phase 3 race tests passed as characterization and did not
trigger speculative implementation changes.

## Primary platform references

- Microsoft, Named Pipe Security and Access Rights:
  <https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights>
- Microsoft, Impersonating a Named Pipe Client:
  <https://learn.microsoft.com/en-us/windows/win32/ipc/impersonating-a-named-pipe-client>
- Microsoft, GetTokenInformation:
  <https://learn.microsoft.com/en-us/windows/win32/api/securitybaseapi/nf-securitybaseapi-gettokeninformation>
- Microsoft, Process Creation Flags:
  <https://learn.microsoft.com/en-us/windows/win32/procthread/process-creation-flags>
- Microsoft, Job Objects:
  <https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects>
- Microsoft, Win32_ProcessStartup:
  <https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-processstartup>
