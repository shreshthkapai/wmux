# Windows IPC and Daemon Lifecycle Model

Phase 3 hardens wmux's Windows control plane without moving platform policy
into the core. The server remains the sole state owner; this document defines
how Windows discovers that server, authenticates clients, starts it outside a
terminal tab's lifetime, and shuts it down without bypassing the owner loop.

## Reference model

tmux establishes four useful rules:

- `client.c:248-299` parses enough command metadata to decide whether a missing
  server may be started. Commands without `CMD_STARTSERVER` report that no
  server is running instead of silently creating one.
- `client.c:103-180` serializes startup with a lock, retries after taking the
  lock, removes stale endpoint state, and only then starts the server.
- `server.c:174-260` makes the server independent from the launching client;
  the client is not the lifetime owner.
- `server.c:262-305` and `server-client.c:2150-2185` stop accepting work, flush
  client output, wait for clients and jobs, and then leave the server loop.

Zellij provides the Rust boundary model:

- `zellij-utils/src/cli.rs` owns declarative CLI parsing separately from
  terminal attachment code.
- `zellij-client/src/lib.rs:349-393` gives Windows a dedicated background
  server spawn path.
- `zellij-client/src/os_input_output.rs:138-142,284-300` keeps server spawning
  and IPC connection behind client OS interfaces.

wmux follows these models with one deliberate Windows-specific extension. A
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

This copies tmux's conservative startup discipline. The client attempts a
connection first. `StartIfMissing` commands serialize a daemon launch and retry
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
