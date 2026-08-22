# Phase 7 Control Mode Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add protocol-v7 structured control commands, ordered replies/notifications, and bounded pane-output subscriptions exposed by `wmux -C`.

**Architecture:** Core defines platform-neutral control records; protocol v7 encodes them; the owner tags command sources and publishes semantic events through each control client's existing bounded outbound queue. The client is a line/record adapter and contains no mux policy.

**Tech Stack:** Rust 2021, versioned `wmux-protocol`, existing framed async IPC, bounded client queues, shared command parser.

**Spec:** `docs/superpowers/specs/2026-08-22-phase-7-shared-mux-semantics-design.md`

## Global Constraints

- Commit tested slices directly to `main`; do not create a branch or worktree.
- Do not use subagents.
- Preserve ordinary client behavior and human-readable protocol mismatch errors.
- Slice pane output records to 64 KiB and retain the 64-frame/4 MiB client caps.
- Never block the owner or another client on a slow control consumer.
- Octal-escape arbitrary bytes; never use lossy UTF-8 for `%output`.

---

### Task 1: Define core control records and protocol v7

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/control.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-protocol/src/lib.rs`
- Modify: `wmux-clean/docs/ipc-protocol.md`

**Interfaces:**
- Produces: `ControlRecord`, `ControlNotification`, and protocol messages `EnterControl`, `ControlCommand`, `ControlRecord`.
- Consumes: stable core IDs and bounded byte vectors.

- [ ] **Step 1: Add failing record/codec tests**

  Golden-test every record variant, arbitrary byte round trips, invalid tags,
  truncated length prefixes, oversized records, v6 mismatch diagnostics, v7
  docs/version drift, and allocation reuse.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core control::tests -- --nocapture
  cargo test -p wmux-protocol control -- --nocapture
  ```

- [ ] **Step 3: Implement core record types**

  Define:

  ```rust
  pub enum ControlRecord { Ready, Begin { sequence: u64 }, Output { pane: PaneId, bytes: Vec<u8> }, Notification(ControlNotification), End { sequence: u64, output: String }, Error { sequence: u64, message: String }, Pause { pane: Option<PaneId> } }
  ```

  Notifications cover client/session/window/pane/buffer/job lifecycle using
  stable IDs and bounded strings.

- [ ] **Step 4: Bump and encode protocol v7**

  Set `VERSION = 7`, `MAGIC = *b"WMX7"`, add explicit tags, and encode fixed
  integers plus length-prefixed variable fields without serializing Rust enum
  layout. Reject invalid UTF-8 only for textual fields; preserve output bytes.

- [ ] **Step 5: Verify and commit**

  ```powershell
  cargo test -p wmux-protocol -p wmux-core
  cargo clippy -p wmux-protocol -p wmux-core --all-targets -- -D warnings
  git add wmux-clean/crates/wmux-core wmux-clean/crates/wmux-protocol wmux-clean/docs/ipc-protocol.md
  git commit -m "feat(protocol): add versioned control records"
  ```

---

### Task 2: Integrate ordered command replies and notifications

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`

**Interfaces:**
- Produces: `CommandSource::Control { sequence }`, per-client control state, and semantic notification fanout.
- Consumes: protocol records and Phase 7 hook events.

- [ ] **Step 1: Add failing owner tests**

  Assert enter-control readiness, monotonic client sequence validation,
  begin-before-command-output-before-end, parse errors as begin/error,
  multi-command list completion, notification order relative to command end,
  ordinary client responses unchanged, and disconnect cleanup.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-server control -- --nocapture
  ```

- [ ] **Step 3: Implement control client state and source-aware replies**

  Add to `ClientView`:

  ```rust
  struct ControlClient { next_sequence: u64, paused: bool, subscribed_output: bool }
  ```

  `EnterControl` installs state and sends `Ready`. `ControlCommand` parses via
  `parse_command_text`, validates sequence monotonicity, sends `Begin`, and
  enqueues `CommandSource::Control { sequence }`. Completion sends `End` or
  `Error`; it never also sends ordinary `CommandOk`/`CommandErr`.

- [ ] **Step 4: Publish shared semantic notifications**

  Map the same owner events used by hooks to `ControlNotification` and enqueue
  them deterministically for every control client. Slice strings and diagnose
  over-limit values instead of panicking.

- [ ] **Step 5: Verify and commit**

  ```powershell
  cargo test -p wmux-server
  git add wmux-clean/crates/wmux-core wmux-clean/crates/wmux-server
  git commit -m "feat(control): order commands and notifications"
  ```

---

### Task 3: Add bounded pane-output subscriptions

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`

**Interfaces:**
- Produces: bounded `%output` delivery, explicit pause, and refresh/resume behavior.
- Consumes: authoritative `PtyOutput` events and existing client byte accounting.

- [ ] **Step 1: Add failing backpressure tests**

  Test 64 KiB slicing, per-client ordering, attached-session filtering,
  independent fast/slow control clients, 4 MiB high-water pause, one pause
  record, no output while paused, command replies while output is paused, and
  `refresh-client` resume from current authoritative scene.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-server control_output -- --nocapture
  ```

- [ ] **Step 3: Publish bounded output before terminal parsing**

  On `ServerEvent::PtyOutput`, identify subscribed control clients attached to
  the pane's session and enqueue ordered 64 KiB `ControlRecord::Output` frames.
  If a frame cannot fit, mark only that control client paused and reserve one
  `Pause` record; continue parsing bytes into the authoritative screen.

- [ ] **Step 4: Extend conformance and verify**

  ```powershell
  cargo test -p wmux-server -p wmux-conformance
  cargo run -p wmux-conformance --release
  ```

- [ ] **Step 5: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-server wmux-clean/crates/wmux-conformance
  git commit -m "feat(control): bound pane output subscriptions"
  ```

---

### Task 4: Expose `wmux -C` and complete Phase 7 evidence

**Files:**
- Modify: `wmux-clean/crates/wmux-cli/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-client/src/lib.rs`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Modify: `wmux-clean/docs/performance.md`
- Modify: `wmux-clean/docs/platform-contract.md`

**Interfaces:**
- Produces: line-oriented control client and final Phase 7 verification record.
- Consumes: protocol-v7 control messages.

- [ ] **Step 1: Add failing CLI/client tests**

  Assert `-C` is recognized, does not enter terminal raw/alternate-screen mode,
  streams stdin lines with monotonic sequences, renders every record in stable
  tmux-like syntax, octal-escapes control/backslash bytes, flushes each record,
  and exits cleanly on EOF/server shutdown.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-cli -p wmux -- control --nocapture
  ```

- [ ] **Step 3: Implement the thin control adapter**

  Add `Invocation::Control` and a current-thread async loop that sends
  `EnterControl`, waits for `Ready`, reads bounded UTF-8 lines from stdin, sends
  `ControlCommand`, and writes formatted records to stdout. It must not inspect
  or mutate sessions, panes, options, hooks, jobs, or buffers.

- [ ] **Step 4: Run the complete Phase 7 gate**

  Run Windows format/tests/clippy/conformance/performance; Linux shared/native
  tests, clippy, conformance, and real CLI/control smoke; both macOS target
  checks; native seam audit; protocol-doc drift check; and `git diff --check`.
  Record exact counts/fingerprint/timings without claiming native macOS runtime
  execution unless a macOS runner actually completed.

- [ ] **Step 5: Commit implementation and evidence**

  ```powershell
  git add wmux-clean/crates/wmux-cli wmux-clean/crates/wmux-client wmux-clean/docs
  git commit -m "feat(control): expose line-oriented client"
  git status --short
  ```

  Expected final status: clean except the pre-existing root `.agents/`.

