# Phase 8 Cross-Platform Beta Core Gate Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce executable, cross-platform evidence that wmux's nonvisual core survives lifecycle failures and bounded stress without session loss, corruption, leaks, or unexplained semantic divergence.

**Architecture:** A dedicated memory-platform stress runner drives the real server and wire protocol, focused unit tests inspect private queue/guard invariants, native integration tests exercise real ConPTY/PTY and endpoints, and a repository-root CI workflow runs the gates from the canonical nested workspace. Documentation separates local, compile-only, CI, and manual evidence.

**Tech Stack:** Rust 2021, Tokio, wmux platform traits, wmux protocol v7, Windows ConPTY/Job Objects/named pipes, Unix PTY/process groups/AF_UNIX/termios, cargo-fuzz, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-08-22-phase-8-beta-core-gate-design.md`

## Global Constraints

- Work directly on `main` and commit every coherent checkpoint there, as explicitly requested by the user.
- Do not use subagents.
- Keep all product source in `wmux-clean`; only GitHub workflow discovery files and roadmap/spec/plan records live at repository root.
- Core and server library code must remain free of Windows and Unix imports.
- The server remains the sole state authority; stress tools interact through platform traits and protocol frames.
- No UI/UX, terminal chrome, theme, or packaging work belongs in this phase.
- Every behavior change follows red-green-refactor TDD.
- Never claim native macOS runtime support from cross-compilation alone.

---

### Task 1: Add the deterministic end-to-end stress runner

**Files:**
- Modify: `wmux-clean/Cargo.toml`
- Create: `wmux-clean/crates/wmux-stress/Cargo.toml`
- Create: `wmux-clean/crates/wmux-stress/src/lib.rs`
- Create: `wmux-clean/crates/wmux-stress/src/main.rs`

**Interfaces:**
- Consumes: `wmux_server::run_with_platform_and_config`, `ServerPlatform`, `ServerListener`, `PtyBackend`, `JobBackend`, and protocol v7 frames.
- Produces: `StressProfile`, `StressReport`, `run_suite(StressProfile) -> Result<StressReport, StressError>`, and `wmux-stress --profile ci|full`.

- [ ] **Step 1: Add the manifest and a failing public-contract test**

Create the package manifest and a `src/lib.rs` test that imports `run_suite`,
runs `StressProfile::Ci`, and expects these literal case names in order:

```rust
assert_eq!(
    report.cases.iter().map(|case| case.name).collect::<Vec<_>>(),
    ["lifecycle", "event-pressure", "fan-out", "storage"],
);
assert!(report.cases.iter().all(|case| case.operations > 0));
```

- [ ] **Step 2: Run the test and verify RED**

```text
cargo test -p wmux-stress stress_suite_covers_every_beta_core_scenario -- --exact
```

Expected: compilation fails because `run_suite` and `StressProfile` do not yet
exist.

- [ ] **Step 3: Implement the memory platform and protocol driver**

Implement a Tokio duplex listener with an authenticated `PeerIdentity`, a
scripted PTY backend that records requests and emits semantic events, and a
scripted job backend. Helpers perform the real hello handshake, write
`EncodedFrame`s, decode replies, enforce deadlines, and shut down cleanly.

- [ ] **Step 4: Implement four deterministic scenarios**

Use these literal profile limits:

```rust
StressProfile::Ci => Limits {
    attach_cycles: 25,
    resize_events: 200,
    clients: 8,
    panes: 8,
    history_lines: 2_000,
    paste_bytes: 256 * 1024,
}
StressProfile::Full => Limits {
    attach_cycles: 250,
    resize_events: 2_000,
    clients: 32,
    panes: 32,
    history_lines: 100_000,
    paste_bytes: 16 * 1024 * 1024,
}
```

Lifecycle drops clients without `Detach`, reconnects, observes preserved state,
kills the server, and starts a fresh server. Event pressure converges on the
literal final resize and preserves output preceding `PtyExited`/`PtyClosed`.
Fan-out leaves a controller responsive while one duplex client is not read.
Storage verifies full-profile history and exact maximum-paste bytes.

- [ ] **Step 5: Implement the CLI and stable report**

Accept only `--profile ci|full` and `--json`. Text output prints each case and
the aggregate fingerprint. Any invariant error exits nonzero.

- [ ] **Step 6: Verify GREEN and determinism**

```text
cargo test -p wmux-stress
cargo run -p wmux-stress --release -- --profile ci
cargo run -p wmux-stress --release -- --profile ci
```

Expected: identical case and aggregate fingerprints.

- [ ] **Step 7: Commit**

```text
git add wmux-clean/Cargo.toml wmux-clean/crates/wmux-stress
git commit -m "test(stress): add beta core scenario runner"
```

### Task 2: Close focused queue, client-loss, and terminal-guard gaps

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-client/src/lib.rs`

**Interfaces:**
- Consumes: private `ClientView`, render scheduling, control pause, attach I/O, and `TerminalModeGuard` contracts.
- Produces: focused regressions for private bounds and recoverable terminal restoration; production changes only if RED exposes a defect.

- [ ] **Step 1: Write server RED tests for composite pressure**

Add tests named for the production break they catch:

```rust
fn stalled_client_is_disconnected_without_blocking_a_responsive_client()
fn synchronized_output_timeout_releases_bounded_pending_render()
fn abrupt_disconnect_removes_only_client_scoped_queue_work()
fn kill_during_queued_output_preserves_output_before_close()
```

Assert observable state and literal byte/message ceilings. A test that passes
immediately is removed or strengthened; it does not justify production edits.

- [ ] **Step 2: Run each test and verify RED**

Run each exact test name and confirm that it fails because the contract is
missing rather than because of test setup.

- [ ] **Step 3: Implement minimum server fixes**

Detach or resynchronize the lagging client, never block the state owner, never
discard critical command completion, and never let client removal affect pane
or session ownership.

- [ ] **Step 4: Write client RED tests for guard lifetime**

Use a fake `TerminalBackend` whose guard increments an atomic drop counter.
Drive normal detach, server EOF, write/read error, and panic unwinding through
the real attach scope. Each case observes exactly one guard drop.

- [ ] **Step 5: Implement the minimum attach-scope fix and verify GREEN**

```text
cargo test -p wmux-server
cargo test -p wmux-client
```

- [ ] **Step 6: Commit**

```text
git add wmux-clean/crates/wmux-server/src/lib.rs wmux-clean/crates/wmux-client/src/lib.rs
git commit -m "test(runtime): harden client pressure and terminal cleanup"
```

### Task 3: Expand native lifecycle and process-tree coverage

**Files:**
- Modify: `wmux-clean/crates/wmux-windows/src/platform.rs`
- Create: `wmux-clean/crates/wmux-windows/tests/native_lifecycle.rs`
- Modify: `wmux-clean/crates/wmux-unix/tests/native_lifecycle.rs`
- Modify if RED requires it: native pane/job/terminal implementation files only.

**Interfaces:**
- Consumes: native transports, ConPTY/PTY, Job Objects/process groups, and protocol v7.
- Produces: `WindowsServerPlatform::for_instance`, `WindowsClientTransport::for_instance`, and symmetric isolated native lifecycle suites.

- [ ] **Step 1: Write the Windows constructor RED test**

Construct server and client transports with the same unique instance and a
second transport with another instance. Matching endpoints must be equal and
the other endpoint unequal.

- [ ] **Step 2: Run and verify RED**

```text
cargo test -p wmux-windows --test native_lifecycle --no-run
```

Expected: compilation fails because the public constructors do not exist.

- [ ] **Step 3: Add isolated Windows constructors**

Delegate to validated `WindowsEndpoint::for_instance`; do not mutate
`WMUX_INSTANCE` in tests.

- [ ] **Step 4: Implement symmetric native lifecycle scenarios**

On both native crates verify abrupt client loss, detach/background
output/reattach, natural pane exit, child/grandchild cleanup for pane/session/
server kills, endpoint release, and restart on the same endpoint. Use native
shell fixtures and bounded PID/process-handle polling. Never inspect or stop the
user's default wmux instance.

- [ ] **Step 5: Verify native GREEN locally**

```text
cargo test -p wmux-windows --test native_lifecycle -- --nocapture
```

Run Unix in the established Linux container and compile both Apple targets.
Actual macOS execution remains a CI requirement.

- [ ] **Step 6: Commit**

```text
git add wmux-clean/crates/wmux-windows/src/platform.rs wmux-clean/crates/wmux-windows/tests/native_lifecycle.rs wmux-clean/crates/wmux-unix/tests/native_lifecycle.rs
git commit -m "test(native): cover isolated lifecycle and tree cleanup"
```

### Task 4: Make malformed-input and fuzz smoke a release gate

**Files:**
- Modify: `wmux-clean/fuzz/README.md`
- Modify: checked-in corpora only when a new minimal seed is justified.
- Modify: owning parser/protocol/terminal tests and code only if replay exposes a defect.

**Interfaces:**
- Consumes: the three existing fuzz targets and corpora.
- Produces: fixed-duration sanitizer commands, artifact handling, and replay evidence.

- [ ] **Step 1: Run sanitizer smoke before edits**

On Linux run each target for 30 seconds with a dedicated artifact directory:

```text
cargo +nightly fuzz run command_text -- -max_total_time=30 -artifact_prefix=artifacts/command_text/
cargo +nightly fuzz run protocol_frame -- -max_total_time=30 -artifact_prefix=artifacts/protocol_frame/
cargo +nightly fuzz run terminal_bytes -- -max_total_time=30 -artifact_prefix=artifacts/terminal_bytes/
```

- [ ] **Step 2: Reproduce any crash as RED**

Move minimal bytes into an owning regression test and watch it fail on current
production code.

- [ ] **Step 3: Apply only required fixes and verify GREEN**

Malformed input may error or recover; it must not panic, allocate without a
bound, or leave the decoder stuck.

- [ ] **Step 4: Document exact replay and long-run commands**

Record 30-second CI, 15-minute release-candidate, minimization, and artifact
replay commands.

- [ ] **Step 5: Verify stable fuzz build and lint**

```text
cargo check --manifest-path fuzz/Cargo.toml --bins
cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings
```

- [ ] **Step 6: Commit**

```text
git add wmux-clean/fuzz wmux-clean/crates
git commit -m "test(fuzz): gate malformed input smoke"
```

### Task 5: Install repository-root three-OS CI gates

**Files:**
- Create: `.github/workflows/beta-core.yml`
- Delete: `wmux-clean/.github/workflows/conformance.yml`

**Interfaces:**
- Consumes: workspace tests, native suites, conformance, stress, fuzz, and performance executables.
- Produces: discoverable GitHub Actions jobs rooted at the repository with canonical working-directory.

- [ ] **Step 1: Run a workflow-discovery RED check**

Require at least one root `.github/workflows/*.yml` and verify its default
working-directory is `wmux-clean`. It must fail because the current workflow is
nested.

- [ ] **Step 2: Create `beta-core.yml`**

Set:

```yaml
defaults:
  run:
    working-directory: wmux-clean
```

Add `quality`, three-OS `portable`, three-OS `native`, three-OS `stress`, Ubuntu
`fuzz-smoke`, and Windows `performance` jobs. Use locked Cargo commands, cache
Cargo directories, and upload fuzz crash artifacts only on failure.

- [ ] **Step 3: Remove the undiscoverable nested workflow**

No workflow may accidentally execute the obsolete root Cargo workspace.

- [ ] **Step 4: Validate workflow paths and local equivalents**

Parse YAML if a parser is available, inspect every `run` command, and run the
Windows equivalents. Confirm `rg --files .github/workflows` finds the root file.

- [ ] **Step 5: Commit**

```text
git add .github/workflows/beta-core.yml wmux-clean/.github/workflows/conformance.yml
git commit -m "ci: enforce cross-platform beta core gates"
```

### Task 6: Publish compatibility, differences, and gate evidence

**Files:**
- Create: `wmux-clean/docs/compatibility-matrix.md`
- Create: `wmux-clean/docs/known-differences.md`
- Create: `wmux-clean/docs/beta-core-gate.md`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Modify: `wmux-clean/docs/performance.md`
- Modify: `wmux-clean/docs/platform-contract.md`

**Interfaces:**
- Consumes: fresh local output and actual CI results.
- Produces: auditable Phase 8 status without upgrading compile evidence into runtime claims.

- [ ] **Step 1: Run the complete fresh local exit gate**

From `wmux-clean` run:

```text
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo check --manifest-path fuzz/Cargo.toml --bins
cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings
cargo run -p wmux-conformance --release
cargo run -p wmux-stress --release -- --profile full
cargo run -p wmux-bench --release -- --suite full --gate
git diff --check
```

Run equivalent full native/unit/stress/conformance gates on Linux and both Apple
target compile checks. Record counts, fingerprints, measurements, triples, and
dates.

- [ ] **Step 2: Write the compatibility matrix**

Rows cover OS, architecture, IPC, PTY, shell, detach/reattach, process cleanup,
terminal restoration, stress, conformance, and tested terminal. Cells use only
`verified`, `CI-verified`, `compile-only`, `manual-pending`, or `unsupported`.

- [ ] **Step 3: Classify every difference**

Use exactly four classes: shared bug, native backend bug, intentional platform
difference, unsupported terminal capability. Empty classes say `None known`.

- [ ] **Step 4: Write the beta gate decision**

Map every master-plan scenario to a command/test. If macOS CI has not run, set
overall status to `Beta Core implementation complete; native macOS runtime gate
pending` and do not call Phase 8 complete.

- [ ] **Step 5: Re-run the final gate after docs**

Completion claims require this fresh run, not earlier development output.

- [ ] **Step 6: Commit**

```text
git add wmux-clean/docs
git commit -m "docs: record phase 8 beta core evidence"
```

