# IPC, Daemon, CLI, and Windows Security Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden protocol-v5 IPC, Windows daemon lifetime, shared CLI behavior, and graceful server shutdown so detached wmux sessions survive disposable clients and only the owning Windows user can connect.

**Architecture:** Add an OS-neutral `wmux-cli` library for local argument policy, keep SID/DACL/token and WMI daemon mechanics inside `wmux-windows`, and replace worker-side process termination with an owner-loop shutdown transaction. The server remains authoritative and the client remains a disposable terminal adapter.

**Tech Stack:** Rust 2021, Tokio named pipes and task sets, `windows-sys` Win32 security/token APIs, Windows PowerShell 5.1 local CIM/WMI bootstrap, protocol version 5.

**Spec:** `AGENTS.md`, `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md` Task 3, and `docs/windows-ipc-daemon-model.md`.

## Global Constraints

- Build from the Phase 2 tip `b2f0378417f8ed08ea3ba21d0314a14d893119c0`; do not recreate or bypass its `CellText` and renderer fixes.
- Core, protocol, CLI, and server semantics must not import Windows APIs. Only `wmux-windows` owns SID, DACL, token, WMI, process, Job Object, and named-pipe mechanics.
- The server owner loop remains the only mutator. IPC tasks emit owner messages and never directly mutate server state or terminate the process.
- Protocol wire version remains exactly `5` with magic `WMX5`; this phase documents it and adds drift checks rather than making an incompatible wire change.
- Endpoint authorization uses the current token's user SID. `USERNAME`, `USER`, client-supplied PID, and protocol fields are never authorization inputs.
- Every pipe instance uses an explicit protected owner-only DACL. A peer-token mismatch fails closed before client registration.
- Client loss never kills panes. Explicit server shutdown drains control replies, terminates pane jobs, releases endpoint/lock state, and returns normally from `main`.
- The daemon launch must survive a launching terminal process that is killed with its Job Object; `CREATE_NO_WINDOW` alone is not sufficient.
- No UI themes, palettes, decorative chrome, flair, or visual configuration are in scope.
- Each behavior change follows red-green TDD and ends with focused tests, workspace tests, formatting, linting, and a reviewable commit.

---

### Task 1: Extract Shared CLI Parsing and Startup Policy

**Files:**
- Modify: `Cargo.toml`
- Create: `crates/wmux-cli/Cargo.toml`
- Create: `crates/wmux-cli/src/lib.rs`
- Modify: `crates/wmux-client/Cargo.toml`
- Modify: `crates/wmux-client/src/main.rs`

**Interfaces:**
- Consumes: `wmux_core::resolve_command_name` and the existing config subcommands.
- Produces: `parse(&[String]) -> Result<Invocation, CliError>`, `Invocation`, `ConfigAction`, `ServerInvocation`, `StartupPolicy`, `HELP`, and `version_line()`.

- [ ] **Step 1: Add the new crate and write failing parser tests**

Test the public surface directly in `crates/wmux-cli/src/lib.rs`:

```rust
assert_eq!(parse(&args(&[])).unwrap(), Invocation::Server(ServerInvocation {
    argv: args(&["new-session"]),
    attached: true,
    startup: StartupPolicy::StartIfMissing,
}));
assert_eq!(parse(&args(&["--help"])).unwrap(), Invocation::Help);
assert_eq!(parse(&args(&["-V"])).unwrap(), Invocation::Version);
assert_eq!(server(&["new-session", "-d"]).startup, StartupPolicy::StartIfMissing);
assert_eq!(server(&["attach-session"]).startup, StartupPolicy::StartIfMissing);
assert_eq!(server(&["list-sessions"]).startup, StartupPolicy::RequireExisting);
assert_eq!(server(&["kill-server"]).startup, StartupPolicy::RequireExisting);
assert!(parse(&args(&["--unknown-option"])).unwrap_err().to_string().contains("unknown option"));
```

- [ ] **Step 2: Run the CLI tests and verify RED**

Run: `cargo test -p wmux-cli`

Expected: FAIL because the crate/public parser does not yet exist.

- [ ] **Step 3: Implement the minimal CLI library**

Use these exact public types:

```rust
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum StartupPolicy { StartIfMissing, RequireExisting }

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ConfigAction { Path, Show, Effective }

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServerInvocation {
    pub argv: Vec<String>,
    pub attached: bool,
    pub startup: StartupPolicy,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Invocation { Help, Version, Config(ConfigAction), Server(ServerInvocation) }

pub fn parse(args: &[String]) -> Result<Invocation, CliError>;
pub const HELP: &str;
pub fn version_line() -> String;
```

Canonicalize tmux aliases/unique prefixes with `resolve_command_name`. Preserve
the current config commands. Classify bare wmux, `new-session`,
`attach-session`, and `start-server` as `StartIfMissing`; default every other
server command to `RequireExisting`. Reject top-level option-like values that
are not `--help`, `-h`, `--version`, or `-V` before command resolution.

- [ ] **Step 4: Make `wmux-client` consume the library**

Delete its local `Invocation` and `classify_invocation`. Keep `show_config_*`,
console mode, input handling, rendering, and attachment functions in the
client. `run` matches `wmux_cli::Invocation` and prints help/version without
opening IPC.

- [ ] **Step 5: Run focused and workspace tests**

Run: `cargo test -p wmux-cli && cargo test -p wmux --bin wmux && cargo test --workspace`

Expected: all tests pass, including prior alias and attachment behavior.

- [ ] **Step 6: Commit**

```text
feat(cli): extract shared invocation policy
```

---

### Task 2: Secure Per-SID Named-Pipe Endpoints

**Files:**
- Modify: `Cargo.toml`
- Modify: `crates/wmux-windows/Cargo.toml`
- Modify: `crates/wmux-windows/src/pipe.rs`
- Modify: `crates/wmux-client/src/main.rs`
- Modify: `crates/wmux-server/src/lib.rs`

**Interfaces:**
- Consumes: Tokio `ServerOptions::create_with_security_attributes_raw`, Windows process/thread token APIs, and current named-pipe connection paths.
- Produces: `Endpoint::current_user() -> io::Result<Endpoint>`, `Endpoint::for_instance(&str) -> io::Result<Endpoint>`, `ServerPipeFactory::new(Endpoint)`, `ServerPipeFactory::create()`, and `verify_client(&NamedPipeServer, &UserSid) -> io::Result<()>`.

- [ ] **Step 1: Add direct `windows-sys = "0.61"` workspace features and write failing SID tests**

Enable only the Win32 features needed for Foundation, Security,
Security.Authorization, System.Memory, System.Pipes, and System.Threading.
Add tests proving:

```rust
let endpoint = Endpoint::current_user().unwrap();
assert!(endpoint.owner_sid().as_str().starts_with("S-1-"));
assert!(endpoint.pipe_name().contains(endpoint.owner_sid().as_str()));

let before = Endpoint::current_user().unwrap().pipe_name().to_owned();
with_username_env("attacker-controlled", || {
    assert_eq!(Endpoint::current_user().unwrap().pipe_name(), before);
});
```

Also test sanitized instance suffixes and unique lock paths.

- [ ] **Step 2: Run the endpoint tests and verify RED**

Run: `cargo test -p wmux-windows pipe::tests::endpoint_ -- --nocapture`

Expected: FAIL because endpoint creation still uses mutable username text.

- [ ] **Step 3: Implement owned token SID lookup**

Add an owned `UserSid` containing copied SID bytes and canonical SID text.
Retrieve `TokenUser` from the current process token using the two-call
`GetTokenInformation` size/query pattern, copy `GetLengthSid` bytes, and format
with `ConvertSidToStringSidW`. Close token handles and `LocalFree` converted
strings on every path. `Endpoint::current_user` applies an optional sanitized
`WMUX_INSTANCE` suffix but always includes the SID.

- [ ] **Step 4: Write failing DACL, first-instance, and peer-token tests**

Add native tests that:

```rust
let endpoint = Endpoint::for_instance(&unique()).unwrap();
let mut factory = ServerPipeFactory::new(endpoint.clone()).unwrap();
let mut server = factory.create().unwrap();
let client = connect_async(&endpoint).unwrap();
server.connect().await.unwrap();
verify_client(&server, endpoint.owner_sid()).unwrap();
assert!(verify_client(&server, &UserSid::well_known_local_system_for_test()).is_err());
```

Create a competing first pipe before constructing a factory and assert that
the secure factory fails instead of joining a pre-created endpoint. Inspect the
generated protected SDDL in a unit test and assert it contains the owner SID
while containing neither `WD` nor `AN` ACEs.

- [ ] **Step 5: Implement the protected pipe factory and verification**

Build a self-relative descriptor from exact SDDL
`D:P(A;;GA;;;<OWNER_SID>)`. Keep the descriptor alive for every factory-created
instance. The first call sets `first_pipe_instance(true)`; later calls create
normal additional instances with the same security attributes. Set Tokio's
`reject_remote_clients(true)` explicitly on every instance.

After the server reads `Hello` and before it writes `HelloOk`, synchronously
verify the connected client token's user SID. Use an impersonation guard that
always calls `RevertToSelf`; do not cross an `.await` while impersonating.
Return `PermissionDenied` on any API failure or SID mismatch.

- [ ] **Step 6: Migrate client/server call sites and test stale lock recovery**

Propagate `Endpoint::current_user()?`, make the server own one
`ServerPipeFactory`, and keep `ServerLock` scoped to `run_async`. Add a unique
instance test that pre-creates only the lock file, acquires the lock, and
asserts the stale file is removed on drop.

- [ ] **Step 7: Run focused and workspace tests**

Run: `cargo test -p wmux-windows pipe::tests -- --nocapture && cargo test -p wmux-server && cargo test --workspace`

Expected: all tests pass and an authenticated same-user pipe remains full duplex.

- [ ] **Step 8: Commit**

```text
feat(windows): secure IPC with SID authentication
```

---

### Task 3: Launch the Daemon Outside Terminal Job Lifetime

**Files:**
- Create: `crates/wmux-windows/src/daemon.rs`
- Modify: `crates/wmux-windows/src/lib.rs`
- Modify: `crates/wmux-client/src/main.rs`

**Interfaces:**
- Consumes: the resolved `wmux-server.exe` path, current environment, and local Windows PowerShell/CIM.
- Produces: `DaemonSpec`, `quote_windows_argument`, and `spawn_user_daemon(&DaemonSpec) -> io::Result<u32>`.

- [ ] **Step 1: Write failing command-line and PowerShell-literal tests**

Use the exact public input type:

```rust
pub struct DaemonSpec {
    pub executable: PathBuf,
    pub arguments: Vec<OsString>,
    pub current_dir: PathBuf,
}
```

Test Windows command-line quoting for empty arguments, spaces, quotes, and
backslashes before a quote. Test PowerShell single-quoted literals by doubling
embedded apostrophes. The resulting WMI script must contain
`Win32_ProcessStartup`, `EnvironmentVariables`, `Invoke-CimMethod`, and a
nonzero-return check.

- [ ] **Step 2: Run the daemon unit tests and verify RED**

Run: `cargo test -p wmux-windows daemon::tests:: -- --nocapture`

Expected: FAIL because the daemon module does not exist.

- [ ] **Step 3: Implement the WMI bootstrap**

Resolve Windows PowerShell from
`%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe`. Start it with
`-NoLogo -NoProfile -NonInteractive -EncodedCommand` and
`CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP`. The encoded UTF-16LE script:

```powershell
$startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{
  ShowWindow = 0
  CreateFlags = 134219264
  EnvironmentVariables = @([Environment]::GetEnvironmentVariables().GetEnumerator() |
    ForEach-Object { "$($_.Key)=$($_.Value)" })
}
$result = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '<quoted server command line>'
  CurrentDirectory = '<quoted current directory>'
  ProcessStartupInformation = $startup
}
if ($result.ReturnValue -ne 0) { throw "Win32_Process.Create failed: $($result.ReturnValue)" }
[Console]::Out.Write($result.ProcessId)
```

Capture stdout/stderr, require a successful bootstrap exit, parse a nonzero
PID, and return bounded human-readable errors. Do not fall back to an ordinary
child process because that would silently violate persistence.

- [ ] **Step 4: Add a native detached-process test**

Spawn a 30-second PowerShell sleep through `spawn_user_daemon`, open the
returned PID with query/terminate rights, assert it remains active after the
bootstrap exits, then terminate and close it in an RAII cleanup guard. Bound
the test to ten seconds and include stderr in failures.

- [ ] **Step 5: Route client server startup through the daemon module**

Replace `process::Command::new(wmux-server.exe).spawn()` and the fixed 200 ms
sleep with `spawn_user_daemon`. Preserve the full client environment through
the WMI startup object. Server readiness is established only by a successful
authenticated handshake retry.

- [ ] **Step 6: Run focused and workspace tests**

Run: `cargo test -p wmux-windows daemon::tests:: -- --nocapture && cargo test -p wmux --bin wmux && cargo test --workspace`

Expected: quoting/native daemon tests and all existing tests pass.

- [ ] **Step 7: Commit**

```text
feat(windows): launch persistent daemon through WMI
```

---

### Task 4: Stabilize Client Connection Diagnostics and Protocol Errors

**Files:**
- Modify: `crates/wmux-client/src/main.rs`
- Modify: `crates/wmux-protocol/src/lib.rs`

**Interfaces:**
- Consumes: `wmux_cli::ServerInvocation`, authenticated endpoint connections, and protocol-v5 hello responses.
- Produces: one `connect_for_invocation` path, bounded retry policy, stable no-server errors, and protocol mismatch diagnostics.

- [ ] **Step 1: Write failing policy/diagnostic tests**

Extract pure helpers and test exact messages:

```rust
assert_eq!(no_server_message(r"\\.\pipe\wmux-S-1-5-21-x"),
           r"no wmux server running for this user (\\.\pipe\wmux-S-1-5-21-x)");
assert!(protocol_error(VERSION - 1).contains("client protocol 5"));
assert!(protocol_error(VERSION - 1).contains("server protocol 4"));
assert_eq!(retry_delays().len(), 20);
```

Add a mock connector test proving `RequireExisting` attempts once and never
calls the spawner, while `StartIfMissing` calls the spawner once and retries.

- [ ] **Step 2: Run client tests and verify RED**

Run: `cargo test -p wmux --bin wmux tests::connection_ -- --nocapture`

Expected: FAIL because connection and startup policy are currently split across
`ensure_server`, sync retry, and async retry paths.

- [ ] **Step 3: Implement one connection state machine**

Use `StartupPolicy` to attempt once for `RequireExisting`. For
`StartIfMissing`, attempt, serialize/start the daemon when absent, and retry 20
times at 50 ms. Treat access denied, invalid endpoint security, and protocol
mismatch as terminal errors rather than retryable absence. Remove
`is_running` probing and the unconditional startup sleep.

Both attached and one-shot commands use the same classification and error
formatting even though one transport is async and the other is blocking.

- [ ] **Step 4: Make handshake mismatch messages explicit**

Expose protocol magic as `pub const MAGIC: [u8; 4] = *b"WMX5"`. When the peer
reports another numeric version, name client and server versions. When frame
magic is incompatible, report that an incompatible wmux server owns the
endpoint and instruct the user to stop it; do not emit `bad magic` alone.

- [ ] **Step 5: Run focused and workspace tests**

Run: `cargo test -p wmux --bin wmux && cargo test -p wmux-protocol && cargo test --workspace`

Expected: all help/version/start-policy/no-server/mismatch and prior attachment tests pass.

- [ ] **Step 6: Commit**

```text
fix(client): make startup and protocol failures deterministic
```

---

### Task 5: Replace Worker Process Exit with Owner-Loop Shutdown

**Files:**
- Modify: `crates/wmux-server/src/lib.rs`
- Modify: `crates/wmux-windows/src/conpty.rs`

**Interfaces:**
- Consumes: serialized `Command::KillServer`, per-client outbound queues, pane Job Objects, and the Tokio accept loop.
- Produces: `ShutdownState`, `OwnerMessage::Stop`, an owner-to-accept-loop shutdown signal, drained connection tasks, and explicit idempotent pane termination.

- [ ] **Step 1: Write failing owner shutdown tests**

Add test-only owners with two clients and assert:

```rust
owner.handle_event(kill_server_from(requester)).unwrap();
assert!(owner.is_shutting_down());
assert!(matches!(requester_rx.try_recv(), Ok(Outbound::Shutdown(Message::CommandOk(_)))));
assert!(matches!(attached_rx.try_recv(), Ok(Outbound::Shutdown(Message::Shutdown))));
assert!(owner.runtime.platform_panes.is_empty());
```

Add a connection-writer test proving `Outbound::Shutdown` writes the complete
frame, reports it drained, and returns without calling `process::exit`.

- [ ] **Step 2: Run focused server tests and verify RED**

Run: `cargo test -p wmux-server shutdown -- --nocapture`

Expected: FAIL because shutdown currently exits from the connection writer and
fallback path.

- [ ] **Step 3: Add explicit pane shutdown**

Make `ConptyPane::terminate` idempotent and add `Runtime::shutdown_platform_panes`
that terminates then clears every live pane, output ring, synchronized-output
timer, and resize hold. Preserve `Drop` as a final safety net.

- [ ] **Step 4: Implement the owner shutdown transaction**

Store `shutting_down: bool` and a one-use async shutdown sender in
`ServerOwner`. On successful `KillServer`, mark shutting down, stop new client
registration, shut down platform panes, enqueue the requester's command reply,
enqueue `Message::Shutdown` for every other client, and notify `run_async` to
drop the listener. A failed enqueue disconnects that client but never exits the
process.

`ServerOwner::run` returns after shutdown and all clients disconnect, or on
`OwnerMessage::Stop`; cleanup runs on every return path.

- [ ] **Step 5: Drain and join async connection tasks**

Track connection tasks in `tokio::task::JoinSet`. On the shutdown signal, drop
the listener and wait up to five seconds for writers to flush and tasks to
finish. Abort only remaining connection tasks after the deadline, send
`OwnerMessage::Stop`, join the state-owner thread, and let `ServerLock` drop.
Return from `run_async` and `main` normally.

- [ ] **Step 6: Add native shutdown/restart tests**

With a unique SID endpoint, run the async server, connect two authenticated
clients, attach one, issue `kill-server` from the other, verify both final
frames, await server completion, assert the lock path is absent, then start a
new server at the same endpoint and shut it down again.

- [ ] **Step 7: Run focused and workspace tests**

Run: `cargo test -p wmux-server shutdown -- --nocapture && cargo test -p wmux-windows conpty::tests -- --nocapture && cargo test --workspace`

Expected: shutdown, restart, pane cleanup, and all prior tests pass with no worker-side process exit.

- [ ] **Step 8: Commit**

```text
fix(server): drain graceful shutdown through owner loop
```

---

### Task 6: Close Native Lifecycle Races and Document Protocol-v5 Evidence

**Files:**
- Modify: `crates/wmux-windows/src/pipe.rs`
- Modify: `crates/wmux-windows/src/conpty.rs`
- Modify: `crates/wmux-server/src/lib.rs`
- Modify: `crates/wmux-protocol/src/lib.rs`
- Create: `docs/ipc-protocol.md`
- Modify: `docs/hybrid-rendering.md`
- Modify: `docs/cross-os-conformance.md`
- Modify: `docs/windows-ipc-daemon-model.md`

**Interfaces:**
- Consumes: the completed Phase 3 CLI, secure endpoint, daemon launcher, graceful shutdown, and existing conformance/performance commands.
- Produces: deterministic native regression coverage, protocol-doc drift protection, and a Phase 3 verification record.

- [ ] **Step 1: Add missing lifecycle race tests before fixes**

Cover these exact native cases with unique instances and bounded waits:

- stale lock is recovered only when the authenticated endpoint is absent;
- a client that disconnects immediately after hello is removed without killing its detached session;
- simultaneous process-exit and ConPTY EOF events produce one logical pane exit and no blocked reader;
- kill-server with an attached client sends a complete final frame;
- daemon restart reuses the SID endpoint after listener and lock cleanup;
- peer verification rejects a deliberately mismatched expected SID before registration.

Run each new test alone and record its initial failure before changing code.

- [ ] **Step 2: Implement only race fixes demonstrated by the failing tests**

Keep endpoint/lock ownership in `wmux-windows`, lifecycle ordering in
`wmux-server`, and EOF/exit normalization in the ConPTY backend. Do not add
platform state to core and do not add sleeps where an event/handle condition is
available.

- [ ] **Step 3: Create protocol-v5 documentation and drift test**

`docs/ipc-protocol.md` must contain literal lines:

```text
Protocol version: 5
Wire magic: WMX5
Maximum frame payload: 16777216 bytes
```

Document the 9-byte header, all current message tags, hello/capability fields,
malformed-frame behavior, version mismatch behavior, and that Windows peer
identity comes from the transport token rather than `Hello.pid`.

Add a protocol unit test using `include_str!` to assert those three literals
match `VERSION`, `MAGIC`, and `MAX_FRAME`. Update the old hybrid-rendering
reference from protocol version 3 to current version 5 while preserving its
historical capability explanation.

- [ ] **Step 4: Record the compatibility matrix honestly**

Extend `docs/cross-os-conformance.md` with automated results for named-pipe
security, daemon persistence, stale state, crash/disconnect, kill-server,
restart, and EOF/exit. Add rows for Windows Terminal, conhost, and VS Code with
PowerShell 7, Windows PowerShell, and cmd.exe. Mark only combinations actually
run as verified; label unavailable interactive combinations `manual pending`
rather than claiming them.

- [ ] **Step 5: Run full Phase 3 verification**

Run:

```text
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo check --manifest-path fuzz/Cargo.toml --bins
cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings
cargo run -p wmux-conformance --release
cargo run -p wmux-bench --release -- --suite full --gate
git diff --check
```

Expected: zero failures/warnings, deterministic conformance fingerprint, and all performance thresholds pass.

- [ ] **Step 6: Commit**

```text
docs: record phase 3 IPC lifecycle evidence
```
