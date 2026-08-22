# Phase 6 Unix Backend Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the real Linux and macOS platform adapter behind the Phase 5 contract, including secure AF_UNIX transport, PTY process lifecycle, terminal mechanics, native composition roots, and native verification.

**Architecture:** Add one deep `wmux-unix` adapter whose public surface is the three construction types consumed by the existing `wmux-platform` traits. Keep all descriptors, UIDs, PIDs, process groups, signals, termios values, and target-specific credential calls private; retain the server's single state owner and existing bounded event contract.

**Tech Stack:** Rust 2021, Rust 1.96, Tokio, crossterm 0.28, nix 0.31, libc, Linux epoll and macOS kqueue through Tokio `AsyncFd`, native AF_UNIX sockets and PTYs.

**Spec:** `docs/superpowers/specs/2026-08-22-phase-6-unix-backend-design.md`

## Global Constraints

- Commit every tested slice directly to `main`; do not create a feature branch or worktree.
- Do not use subagents.
- Do not change the public `wmux-platform` interface or protocol v6.
- No Unix import, descriptor, UID, PID, signal, errno, or native adapter dependency may enter a shared crate.
- The server remains the only state mutator; native tasks emit semantic events only.
- PTY I/O is chunked and bounded: at most 16 KiB per read and no per-byte task, allocation, event, or write.
- Preserve every input/output byte under backpressure and preserve `PtyExited`/`PtyClosed` ordering.
- UI/UX and mux chrome remain out of scope.
- Every production behavior starts with a failing test and a verified RED result.
- Windows verification remains required for every Unix commit.

---

### Task 1: Scaffold the Unix adapter and secure endpoint module

**Files:**
- Modify: `wmux-clean/Cargo.toml`
- Modify: `wmux-clean/Cargo.lock`
- Create: `wmux-clean/crates/wmux-unix/Cargo.toml`
- Create: `wmux-clean/crates/wmux-unix/src/lib.rs`
- Create: `wmux-clean/crates/wmux-unix/src/ipc.rs`

**Interfaces:**
- Consumes: `wmux_platform::{Endpoint, PeerIdentity, PlatformError, PlatformResult}`.
- Produces: private `UnixEndpoint::current_user()`, `UnixEndpoint::bind()`, `UnixEndpoint::connect()`, and an accepted stream with a native-derived `PeerIdentity`.

- [ ] **Step 1: Add the manifest and endpoint tests without an implementation**

  Add `wmux-unix` as a workspace member and declare target-Unix dependencies:

  ```toml
  [package]
  name = "wmux-unix"
  version.workspace = true
  edition.workspace = true
  license.workspace = true

  [dependencies]
  crossterm.workspace = true
  libc.workspace = true
  nix.workspace = true
  tokio.workspace = true
  wmux-platform = { path = "../wmux-platform" }
  ```

  In `ipc.rs`, add tests named:

  ```rust
  endpoint_directory_and_socket_are_owner_only
  live_endpoint_rejects_a_second_server
  stale_owned_socket_is_recovered
  symlink_or_foreign_stale_endpoint_is_never_removed
  accepted_peer_identity_comes_from_the_native_socket
  listener_drop_removes_only_its_owned_files
  ```

  Tests must use a unique directory under `std::env::temp_dir`, create it with
  mode `0700`, and remove the exact test directory on drop.

- [ ] **Step 2: Run the endpoint tests and verify RED**

  Run on Linux:

  ```powershell
  docker run --rm -v "${PWD}:/work" -v wmux-cargo-registry:/usr/local/cargo/registry -v wmux-linux-target:/target -w /work -e CARGO_TARGET_DIR=/target rust:1.96-bookworm cargo test -p wmux-unix ipc::tests -- --nocapture
  ```

  Expected: compile failure because `UnixEndpoint`, secure bind, and peer
  credential functions do not exist.

- [ ] **Step 3: Implement endpoint discovery and safe filesystem ownership**

  Implement these private shapes:

  ```rust
  pub(crate) struct UnixEndpoint {
      directory: PathBuf,
      socket: PathBuf,
      lock: PathBuf,
      owner_uid: libc::uid_t,
  }

  pub(crate) struct BoundEndpoint {
      listener: tokio::net::UnixListener,
      endpoint: UnixEndpoint,
      lock_file: File,
  }

  impl UnixEndpoint {
      pub(crate) fn current_user() -> io::Result<Self>;
      pub(crate) fn bind(&self) -> io::Result<BoundEndpoint>;
      pub(crate) async fn connect(&self) -> io::Result<tokio::net::UnixStream>;
  }
  ```

  Validate ownership and mode with `symlink_metadata`; reject symlinks and
  unexpected file types. Acquire the lock atomically with `create_new(true)`.
  Probe a pre-existing socket before treating it as stale. Set the directory
  and socket to `0700` and `0600` explicitly rather than relying only on umask.

- [ ] **Step 4: Implement platform-specific peer credentials**

  Use a private function with focused target blocks:

  ```rust
  fn peer_identity(stream: &tokio::net::UnixStream) -> io::Result<PeerIdentity>;
  ```

  On Linux read `SO_PEERCRED`; on macOS call `getpeereid`. Encode the effective
  UID as fixed-width native-independent bytes before constructing the opaque
  `PeerIdentity`. Never accept identity bytes from the protocol peer.

- [ ] **Step 5: Run native tests and Windows regression checks**

  Run the Linux endpoint command from Step 2 and then:

  ```powershell
  cargo test --workspace
  cargo clippy --workspace --all-targets -- -D warnings
  ```

  Expected: all tests pass and Windows shared/native crates remain warning-free.

- [ ] **Step 6: Commit**

  ```powershell
  git add wmux-clean/Cargo.toml wmux-clean/Cargo.lock wmux-clean/crates/wmux-unix
  git commit -m "feat(unix): secure native IPC endpoints"
  ```

---

### Task 2: Implement the Unix terminal backend and restoration guard

**Files:**
- Create: `wmux-clean/crates/wmux-unix/src/terminal.rs`
- Modify: `wmux-clean/crates/wmux-unix/src/lib.rs`

**Interfaces:**
- Consumes: frozen `TerminalBackend`, `TerminalInput`, semantic key/mouse types, and `TerminalSize`.
- Produces: public `UnixTerminalBackend` and a drop guard that restores exact saved termios.

- [ ] **Step 1: Write failing terminal behavior tests**

  Add tests named:

  ```rust
  raw_mode_guard_restores_exact_termios_on_drop
  raw_mode_guard_restores_after_a_simulated_error
  semantic_keys_keep_identity_modifiers_and_raw_bytes
  paste_mouse_and_resize_events_remain_distinct
  terminal_size_is_clamped_to_one_cell
  render_transaction_is_one_synchronized_write
  osc52_clipboard_is_bounded_and_encoded
  ```

  Allocate a test PTY for termios/size tests so CI does not require stdin to be
  an interactive terminal. Test pure `normalize_event`, `write_transaction_to`,
  and `write_osc52_to` helpers through observable bytes.

- [ ] **Step 2: Verify terminal tests fail for missing behavior**

  ```powershell
  docker run --rm -v "${PWD}:/work" -v wmux-cargo-registry:/usr/local/cargo/registry -v wmux-linux-target:/target -w /work -e CARGO_TARGET_DIR=/target rust:1.96-bookworm cargo test -p wmux-unix terminal::tests -- --nocapture
  ```

  Expected: compile failure for the missing terminal module and guard.

- [ ] **Step 3: Implement raw mode, event normalization, and size**

  Use a saved `nix::sys::termios::Termios` plus owned stdin descriptor in:

  ```rust
  struct UnixTerminalGuard {
      input: OwnedFd,
      saved: Termios,
  }
  ```

  `enter` applies `cfmakeraw` and `tcsetattr(TCSANOW)`. `Drop` restores `saved`.
  Convert crossterm key, paste, mouse, and resize events to the frozen semantic
  values, using the same normalized key/raw-byte cases as Windows. Query size
  through `TIOCGWINSZ` and clamp rows/columns to one.

- [ ] **Step 4: Implement batched output and OSC 52**

  Lock stdout once per call. Write synchronized output as:

  ```text
  ESC[?2026h + complete frame + ESC[?2026l
  ```

  Encode clipboard text as one OSC 52 transaction, cap decoded input at 1 MiB,
  and reject larger clipboard writes with `PlatformErrorKind::InvalidInput`.

- [ ] **Step 5: Verify terminal behavior on Linux and Windows**

  Run the Step 2 command plus Windows workspace tests and clippy. Expected: the
  Unix terminal tests pass and Windows behavior is unchanged.

- [ ] **Step 6: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-unix
  git commit -m "feat(unix): add restoring terminal backend"
  ```

---

### Task 3: Implement PTY spawn, nonblocking I/O, resize, and event ordering

**Files:**
- Create: `wmux-clean/crates/wmux-unix/src/process.rs`
- Create: `wmux-clean/crates/wmux-unix/src/pty.rs`
- Modify: `wmux-clean/crates/wmux-unix/src/lib.rs`

**Interfaces:**
- Consumes: `SpawnPane`, `PlatformRequest`, `PlatformEvent`, `PlatformNotifier`, and `TerminationMode`.
- Produces: `UnixPtyBackend::new(Handle, PlatformNotifier)` implementing `PtyBackend`.

- [ ] **Step 1: Write failing native PTY tests**

  Add tests named:

  ```rust
  default_shell_roundtrips_bytes_through_a_real_pty
  custom_command_cwd_and_environment_are_applied
  large_input_survives_short_nonblocking_writes_in_order
  resize_storm_leaves_the_final_kernel_winsize
  output_after_child_exit_is_drained_before_closed
  eof_without_status_emits_unknown_exit_then_closed
  duplicate_and_post_close_events_are_impossible
  ```

  Each asynchronous test must use a timeout of at most ten seconds and kill its
  process group in its cleanup guard.

- [ ] **Step 2: Verify the PTY tests are RED**

  ```powershell
  docker run --rm -v "${PWD}:/work" -v wmux-cargo-registry:/usr/local/cargo/registry -v wmux-linux-target:/target -w /work -e CARGO_TARGET_DIR=/target rust:1.96-bookworm cargo test -p wmux-unix pty::tests -- --nocapture
  ```

  Expected: compile failure for the missing backend.

- [ ] **Step 3: Implement safe parent-side PTY ownership and child setup**

  `process.rs` exposes only private crate types:

  ```rust
  pub(crate) struct SpawnedPane {
      pub master: OwnedFd,
      pub child: std::process::Child,
      pub process_group: libc::pid_t,
  }

  pub(crate) fn spawn_pane(request: &SpawnPane) -> io::Result<SpawnedPane>;
  pub(crate) fn signal_group(group: libc::pid_t, signal: Signal) -> io::Result<()>;
  ```

  Allocate with `openpty` at the final size. Configure `Command` before spawn;
  in `pre_exec`, call only async-signal-safe operations needed for `setsid`,
  controlling-terminal setup, descriptor duplication/closure, and signal-mask
  reset. Default to absolute `$SHELL`, otherwise `/bin/sh`.

- [ ] **Step 4: Implement reactor-driven reader and ordered writer tasks**

  Set the master nonblocking before `AsyncFd::new`. The reader uses one reused
  16 KiB buffer and sends owned chunks through a bounded 64-entry queue. The
  writer consumes one bounded ordered queue, handles `EINTR`, retries readiness
  on `EAGAIN`, and completes all short writes. Both tasks notify the state owner
  only when an event becomes available.

- [ ] **Step 5: Implement resize and final event normalization**

  Apply `TIOCSWINSZ` for nonzero clamped sizes. A child waiter sends one exit
  status; the reader sends EOF independently. The backend state machine drains
  both sources, preserves queued output order even when output follows exit,
  emits at most one exit, and emits exactly one final close. Remove pane state
  only after returning `PtyClosed`.

- [ ] **Step 6: Verify PTY tests and regressions**

  Run the Step 2 command, `cargo test --workspace`, and Windows clippy. Expected:
  native PTY tests and all existing 285 workspace tests pass.

- [ ] **Step 7: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-unix
  git commit -m "feat(unix): add native PTY lifecycle"
  ```

---

### Task 4: Prove process-group cleanup and backend shutdown

**Files:**
- Modify: `wmux-clean/crates/wmux-unix/src/process.rs`
- Modify: `wmux-clean/crates/wmux-unix/src/pty.rs`

**Interfaces:**
- Consumes: the Task 3 pane process group and frozen graceful/force requests.
- Produces: descendant-safe termination, direct-child reaping, and force cleanup on adapter drop.

- [ ] **Step 1: Write failing cleanup tests**

  Add tests named:

  ```rust
  graceful_termination_hangs_up_the_complete_process_group
  forced_termination_kills_the_complete_process_group
  dropping_the_backend_kills_and_reaps_every_remaining_pane
  terminating_one_pane_does_not_signal_another_pane_group
  ```

  Spawn a shell that records both its PID and a long-lived descendant PID.
  Assert both are gone using signal zero plus bounded wait; ensure cleanup runs
  even when an assertion fails.

- [ ] **Step 2: Verify RED**

  Run only the four cleanup tests in the Linux container. Expected: at least the
  descendant or backend-drop assertion fails before group cleanup exists.

- [ ] **Step 3: Implement group termination and reap ownership**

  Graceful requests send `SIGHUP` to `-pgid`; force requests send `SIGKILL`.
  Termination is idempotent. Keep the waiter alive until `wait` returns. `Drop`
  closes queues, force-signals remaining groups, and synchronously reaps only
  children not already owned by a waiter.

- [ ] **Step 4: Verify cleanup and all native tests**

  Run `cargo test -p wmux-unix` inside Linux and the Windows workspace suite.
  Expected: no surviving PID and no hung test process.

- [ ] **Step 5: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-unix/src/process.rs wmux-clean/crates/wmux-unix/src/pty.rs
  git commit -m "fix(unix): own complete pane process groups"
  ```

---

### Task 5: Implement platform adapters, daemon startup, and Unix composition roots

**Files:**
- Create: `wmux-clean/crates/wmux-unix/src/platform.rs`
- Modify: `wmux-clean/crates/wmux-unix/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-server/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-server/src/main.rs`
- Modify: `wmux-clean/crates/wmux-client/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-client/src/main.rs`

**Interfaces:**
- Consumes: Tasks 1-4 and all four frozen platform traits.
- Produces: public `UnixServerPlatform`, `UnixClientTransport`, and `UnixTerminalBackend` construction used by the real binaries.

- [ ] **Step 1: Write failing adapter contract tests**

  Add tests named:

  ```rust
  unix_adapter_exposes_only_semantic_endpoint_and_identity
  server_listener_rejects_a_peer_with_another_uid
  daemon_server_survives_launcher_exit_and_has_no_terminal
  native_client_and_server_complete_a_detached_command
  ```

  The daemon test launches a bounded helper process and verifies its session ID
  differs from the launcher. The command test uses real Unix sockets and the
  actual `wmux-server` binary.

- [ ] **Step 2: Verify adapter tests and Unix binaries are RED**

  ```powershell
  docker run --rm -v "${PWD}:/work" -v wmux-cargo-registry:/usr/local/cargo/registry -v wmux-linux-target:/target -w /work -e CARGO_TARGET_DIR=/target rust:1.96-bookworm cargo test -p wmux-unix platform::tests -- --nocapture
  ```

  Then run `cargo check -p wmux -p wmux-server --bins` in the container.
  Expected: missing adapters or the old Phase 6 unsupported composition error.

- [ ] **Step 3: Implement the frozen trait adapters**

  `UnixServerPlatform::bind` owns `BoundEndpoint`; `accept` verifies credentials
  before returning `AcceptedConnection`. `create_pty_backend` constructs Task
  3 using `TokioHandle::current`. `UnixClientTransport::connect` returns a boxed
  Tokio Unix stream.

- [ ] **Step 4: Implement detached server launch**

  Spawn the exact `DaemonSpec` executable/arguments/current directory with
  stdin/stdout/stderr set to null. In a strictly async-signal-safe `pre_exec`,
  call `setsid`. Start one bounded reaper thread for the direct server child.
  Return startup errors synchronously and never invoke a shell.

- [ ] **Step 5: Wire target-specific binary composition**

  Add `wmux-unix` only under `target.'cfg(unix)'.dependencies`. Replace the
  non-Windows unsupported paths with `#[cfg(unix)]` constructors. Retain an
  unsupported fallback only for targets that are neither Windows nor Unix.

- [ ] **Step 6: Verify real Linux commands**

  In one Linux container, build both binaries and run with an isolated runtime
  directory:

  ```text
  wmux new -d -s phase6
  wmux list-sessions
  wmux split-window -t phase6
  wmux kill-session -t phase6
  wmux kill-server
  ```

  Expected: commands complete over the real socket and PTY backend with no
  surviving server or pane descendants.

- [ ] **Step 7: Run regressions and commit**

  Run Linux `cargo test -p wmux-unix`, binary checks, portable conformance, then
  Windows workspace tests/clippy. Commit:

  ```powershell
  git add wmux-clean/crates/wmux-unix wmux-clean/crates/wmux-server wmux-clean/crates/wmux-client
  git commit -m "feat(unix): compose native client and server"
  ```

---

### Task 6: Add Linux/macOS native CI and lifecycle coverage

**Files:**
- Modify: `.github/workflows/ci.yml`
- Create: `wmux-clean/crates/wmux-unix/tests/native_lifecycle.rs`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Modify: `wmux-clean/docs/platform-contract.md`
- Modify: `wmux-clean/docs/native-async-io.md`

**Interfaces:**
- Consumes: the production Unix binaries and frozen portable conformance suite.
- Produces: repeatable native Linux/macOS evidence for the full Phase 6 lifecycle.

- [ ] **Step 1: Write the failing native lifecycle test**

  The integration test must drive:

  ```text
  create -> attach protocol -> type -> split -> resize -> detach
         -> background output -> reattach -> kill pane/session -> kill server
  ```

  Use a real `UnixServerPlatform`, real PTY shell, real socket, and protocol v6.
  Use a test-owned runtime directory and shell commands available on both Linux
  and macOS (`/bin/sh`, `printf`, `sleep`). Assert final output and cleanup, not
  implementation state.

- [ ] **Step 2: Verify RED before adding orchestration**

  Run `cargo test -p wmux-unix --test native_lifecycle` in Linux. Expected:
  failure for the first lifecycle behavior not yet exposed to the test.

- [ ] **Step 3: Add only the test harness needed to pass**

  Permit explicit test endpoint injection inside `wmux-unix` without changing
  shared traits. Drive the existing client/server library entry points or the
  real binaries; do not duplicate mux semantics in the test.

- [ ] **Step 4: Add native Unix CI matrix**

  Add `native-unix` for `ubuntu-latest` and `macos-latest` using Rust 1.96.0.
  Each job runs format check, Unix clippy with warnings denied, `wmux-unix`
  tests including `native_lifecycle`, Unix binary checks, and release portable
  conformance. Keep `windows-baseline` unchanged and required.

- [ ] **Step 5: Cross-check both macOS architectures locally**

  ```powershell
  rustup target add x86_64-apple-darwin aarch64-apple-darwin
  cargo check -p wmux-unix -p wmux -p wmux-server --target x86_64-apple-darwin
  cargo check -p wmux-unix -p wmux -p wmux-server --target aarch64-apple-darwin
  ```

  Expected: both compile checks pass without native types leaking into shared
  code. Native behavior remains gated by `macos-latest` CI.

- [ ] **Step 6: Update architecture and conformance documentation**

  Record socket credential differences, identical event ordering, epoll/kqueue
  reactor mapping, process-group cleanup, terminal restoration, native test
  commands, and the unchanged portable fingerprint. Do not add any expected
  shared semantic difference.

- [ ] **Step 7: Verify and commit**

  Run Linux native tests/conformance and Windows workspace tests/clippy, then:

  ```powershell
  git add .github/workflows/ci.yml wmux-clean/crates/wmux-unix/tests wmux-clean/docs
  git commit -m "test(unix): gate native lifecycle on Linux and macOS"
  ```

---

### Task 7: Complete Phase 6 verification and evidence

**Files:**
- Modify: `wmux-clean/docs/platform-contract.md`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Modify: `wmux-clean/docs/performance.md`

**Interfaces:**
- Consumes: every Phase 6 implementation and test.
- Produces: auditable completion evidence without changing runtime behavior.

- [ ] **Step 1: Run the full Windows quality gate**

  ```powershell
  cargo fmt --all -- --check
  cargo test --workspace
  cargo clippy --workspace --all-targets -- -D warnings
  cargo run -p wmux-conformance --release
  cargo run -p wmux-bench --release -- --suite full --gate
  ```

  Expected: zero failures and the performance gate remains above every recorded
  floor.

- [ ] **Step 2: Run the full Linux quality gate**

  In the Rust 1.96 Linux container run formatting, Unix/shared clippy, all Unix
  and portable tests, both real binary builds, release conformance, and the real
  detached CLI smoke sequence. Expected: zero failures and the same aggregate
  fingerprint as Windows.

- [ ] **Step 3: Audit the native seam**

  Run `rg` over shared library sources for `std::os::unix`, `libc`, `nix`,
  `RawFd`, `OwnedFd`, UID/PID/signal names, and `wmux_unix`. Expected: no match
  outside binary composition roots and `wmux-unix`.

- [ ] **Step 4: Record exact evidence**

  Update the three docs with test counts, conformance fingerprint, native
  platform coverage, cross-compile results, performance results, reference
  revisions, and any narrowly scoped extra work. Do not claim a native macOS
  pass unless the `macos-latest` job actually ran successfully.

- [ ] **Step 5: Verify the evidence diff and commit**

  ```powershell
  git diff --check
  git status --short
  git add wmux-clean/docs/platform-contract.md wmux-clean/docs/cross-os-conformance.md wmux-clean/docs/performance.md
  git commit -m "docs: record phase 6 Unix backend evidence"
  ```

  Expected final status: clean except for the pre-existing root `.agents/`.
