# Phase 7 Hooks and Jobs Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add deterministic inherited hooks, bounded native background jobs, and serialized command continuations.

**Architecture:** Core modules own hook/job identity and state; the command queue owns continuation order. A narrow `JobBackend` seam owns Windows/Unix process creation, output, termination, and reaping and emits only semantic events to the state owner.

**Tech Stack:** Rust 2021, bounded standard/Tokio channels, Windows Job Objects, Unix process groups, existing platform error model.

**Spec:** `docs/superpowers/specs/2026-08-22-phase-7-shared-mux-semantics-design.md`

## Global Constraints

- Commit tested slices directly to `main`; do not create a branch or worktree.
- Do not use subagents.
- Research and follow existing tmux/zellij job/hook models and official native process documentation.
- Keep native handles, PIDs, signals, descriptors, and exit-status types out of shared crates.
- Limit hooks to 256 registrations/depth 16 and jobs to 64 running/1 MiB output each.
- Server shutdown terminates and reaps all remaining jobs.

---

### Task 1: Add hook storage and queue insertion semantics

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/hooks.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`

**Interfaces:**
- Produces: `HookEvent`, `HookStore::{set,unset,resolve,list}`, `CommandQueue::insert_after_active`, and depth-carrying `CommandSource::Hook`.
- Consumes: `OptionTarget` inheritance and immutable `CommandList`.

- [ ] **Step 1: Add failing hook/queue tests**

  Cover most-specific inheritance, registration order, replacement/removal,
  stable listing, insertion before remaining commands, per-client isolation,
  error continuation, and recursion depth 16.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core hooks::tests command::tests::continuation -- --nocapture
  ```

- [ ] **Step 3: Implement the deep hook module and mutable pending queue**

  Use:

  ```rust
  pub enum HookEvent { ClientAttached, ClientDetached, SessionCreated, SessionClosed, WindowCreated, WindowClosed, PaneCreated, PaneClosed, BufferChanged, BufferDeleted, JobFinished }
  pub struct HookStore { hooks: BTreeMap<OptionTarget, BTreeMap<HookEvent, Vec<CommandList>>> }
  ```

  Refactor a pending invocation to a `VecDeque<Command>` so
  `insert_after_active` prepends commands without changing invocation identity
  or allowing another command for that client to overtake them.

- [ ] **Step 4: Add `set-hook`/`show-hooks` and event effects**

  Parse nested hook commands with the existing depth checker. Successful state
  mutations emit `CommandEffect::Notify { event, target }`; the owner resolves
  and inserts hooks with `CommandSource::Hook { depth }`.

- [ ] **Step 5: Verify and commit**

  ```powershell
  cargo test -p wmux-core
  cargo clippy -p wmux-core --all-targets -- -D warnings
  git add wmux-clean/crates/wmux-core
  git commit -m "feat(hooks): serialize inherited event commands"
  ```

---

### Task 2: Freeze the native-free job platform seam

**Files:**
- Create: `wmux-clean/crates/wmux-platform/src/job.rs`
- Modify: `wmux-clean/crates/wmux-platform/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-platform/src/transport.rs`
- Modify: `wmux-clean/docs/platform-contract.md`

**Interfaces:**
- Produces: `PlatformJobId`, `SpawnJob`, `JobRequest`, `JobEvent`, `JobBackend`, `JobNotifier`, and `ServerPlatform::create_job_backend`.
- Consumes: `PlatformError`, `CommandSpec`-style environment/cwd values.

- [ ] **Step 1: Add failing seam tests and adapter compile failures**

  Assert all semantic types are `Send + Sync`, IDs are opaque wmux tokens,
  output/exit/close are distinct, diagnostics are bounded, and no native field
  appears in debug output. Add the method to the trait only after recording
  Windows/Unix adapter compile failures.

- [ ] **Step 2: Implement semantic types**

  Define:

  ```rust
  pub struct PlatformJobId(u64);
  pub struct SpawnJob { pub job: PlatformJobId, pub command: String, pub cwd: Option<PathBuf>, pub environment: Vec<(OsString, OsString)> }
  pub enum JobRequest { Spawn(SpawnJob), Terminate { job: PlatformJobId } }
  pub enum JobEvent { Output { job: PlatformJobId, bytes: Vec<u8> }, Exited { job: PlatformJobId, exit_code: Option<u32> }, Closed { job: PlatformJobId }, BackendError { job: PlatformJobId, error: PlatformError } }
  ```

  Require no event after `Closed` and at most one meaningful `Exited`.

- [ ] **Step 3: Update mock adapters/docs and verify**

  ```powershell
  cargo test -p wmux-platform -p wmux-server
  cargo check -p wmux-windows -p wmux-unix
  git add wmux-clean/crates/wmux-platform wmux-clean/crates/wmux-server wmux-clean/docs/platform-contract.md
  git commit -m "feat(platform): freeze native job contract"
  ```

---

### Task 3: Implement Windows and Unix job adapters

**Files:**
- Create: `wmux-clean/crates/wmux-windows/src/job.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/platform.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/lib.rs`
- Create: `wmux-clean/crates/wmux-unix/src/job.rs`
- Modify: `wmux-clean/crates/wmux-unix/src/platform.rs`
- Modify: `wmux-clean/crates/wmux-unix/src/lib.rs`

**Interfaces:**
- Produces: `WindowsJobBackend` and `UnixJobBackend` implementing Task 2.
- Consumes: adapter-local runtime handles, process-tree primitives, and notifier.

- [ ] **Step 1: Add failing native contract tests**

  On each native target test stdout/stderr capture, 2 MiB producer
  backpressure, exit-before-close ordering, custom cwd/environment, explicit
  terminate, descendant cleanup, backend-drop cleanup, and isolation between
  two jobs.

- [ ] **Step 2: Verify RED on Windows and Linux**

  ```powershell
  cargo test -p wmux-windows job::tests -- --nocapture
  docker run --init --rm -v "${PWD}/wmux-clean:/work" -w /work rust:1.96-bookworm cargo test -p wmux-unix job::tests -- --nocapture
  ```

- [ ] **Step 3: Implement bounded native execution**

  Unix runs `/bin/sh -c` in a new process group, closes unrelated descriptors,
  reads 16 KiB chunks into a bounded 64-event channel, signals the group, and
  reaps the direct child. Windows runs `%COMSPEC% /D /S /C` detached from a
  console, assigns it to a kill-on-close Job Object before resume, reads pipes
  in 16 KiB chunks, and closes every inherited handle intentionally.

- [ ] **Step 4: Verify native suites and commit**

  ```powershell
  cargo test -p wmux-windows
  cargo clippy -p wmux-windows --all-targets -- -D warnings
  docker run --init --rm -v "${PWD}/wmux-clean:/work" -v wmux-cargo-registry:/usr/local/cargo/registry -v wmux-linux-target:/target -w /work -e CARGO_TARGET_DIR=/target rust:1.96-bookworm cargo test -p wmux-unix
  git add wmux-clean/crates/wmux-windows wmux-clean/crates/wmux-unix
  git commit -m "feat(platform): add bounded native jobs"
  ```

---

### Task 4: Add job state, commands, and continuations

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/jobs.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/ids.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`

**Interfaces:**
- Produces: `JobId`, `JobState`, `Command::{RunShell,IfShell}`, `CommandEffect::StartJob`, and owner completion routing.
- Consumes: Tasks 1-3 and shared format expansion.

- [ ] **Step 1: Add failing job/continuation tests**

  Test the 64-job cap, 1 MiB capture truncation marker, foreground queue pause,
  background immediate completion, success/failure branch insertion, output in
  `run-shell` completion, job-finished hook order, disconnect behavior, and
  shutdown cleanup.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core -p wmux-server job -- --nocapture
  ```

- [ ] **Step 3: Implement core job state and command effects**

  Add:

  ```rust
  pub enum JobState { Running, Exited(Option<u32>), Closed }
  Command::RunShell { background: bool, command: String }
  Command::IfShell { background: bool, shell_command: String, if_true: CommandList, if_false: Option<CommandList> }
  ```

  `StartJob` carries already-expanded command/cwd/environment plus an optional
  continuation. It does not expose platform IDs as core identity.

- [ ] **Step 4: Integrate owner polling and deferred completion**

  Construct both backends in `run_async`. Poll job events whenever
  `PlatformReady` wakes the owner and during active turns. Foreground jobs keep
  their `QueuedCommand` active; on close, insert the selected branch, finish
  the item, then fire `JobFinished`. Background jobs complete their command
  immediately but retain bounded state until close.

- [ ] **Step 5: Run all native/shared tests and commit**

  ```powershell
  cargo fmt --all -- --check
  cargo test --workspace
  cargo clippy --workspace --all-targets -- -D warnings
  git add wmux-clean
  git commit -m "feat(jobs): serialize bounded command continuations"
  ```
