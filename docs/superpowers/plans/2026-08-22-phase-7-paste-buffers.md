# Phase 7 Paste Buffers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add binary-safe server paste buffers, file commands, clipboard routing, and bounded chunked pane pastes.

**Architecture:** A deep core store owns immutable buffer data and deterministic ordering. Commands emit semantic file/clipboard/paste effects; the server performs file I/O and advances pending pane writes in bounded chunks, while platform clipboard mechanics remain client-side.

**Tech Stack:** Rust 2021, `Arc<[u8]>`, existing command queue, protocol clipboard message, and terminal adapters.

**Spec:** `docs/superpowers/specs/2026-08-22-phase-7-shared-mux-semantics-design.md`

## Global Constraints

- Commit tested slices directly to `main`; do not create a branch or worktree.
- Do not use subagents.
- Preserve arbitrary bytes; do not convert buffer/file/pane payloads through UTF-8.
- Limit one buffer to 16 MiB and aggregate storage to 64 MiB.
- Submit at most 64 KiB of a pending paste per owner turn.
- Clipboard mechanics stay behind the existing terminal backend and protocol.

---

### Task 1: Add the server-owned paste buffer store

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/paste.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`

**Interfaces:**
- Produces: `PasteBuffer`, `PasteBufferStore::{set_named,add_automatic,get,remove,list,set_limit}`.
- Consumes: inherited `buffer-limit` from the options slice.

- [ ] **Step 1: Add failing store tests**

  Cover exact binary bytes, monotonic `bufferN` naming, newest-first order,
  named replacement, automatic-only eviction, empty automatic input, invalid
  names, per-buffer/aggregate limits, and deterministic samples.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core paste::tests -- --nocapture
  ```

- [ ] **Step 3: Implement the bounded store**

  Use:

  ```rust
  pub struct PasteBuffer { name: String, data: Arc<[u8]>, automatic: bool, order: u64 }
  pub struct PasteBufferStore { by_name: BTreeMap<String, PasteBuffer>, newest: VecDeque<String>, next_name: u64, next_order: u64, bytes: usize }
  ```

  Keep ordering indexes consistent on replace/remove, and reject an insertion
  before mutating when any byte/count limit would be exceeded.

- [ ] **Step 4: Integrate `ServerState` and commit**

  ```powershell
  cargo test -p wmux-core paste::tests state::tests
  git add wmux-clean/crates/wmux-core/src
  git commit -m "feat(buffers): add binary-safe server store"
  ```

---

### Task 2: Add buffer commands and bounded file effects

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`

**Interfaces:**
- Produces: buffer command variants plus `CommandEffect::{ReadBufferFile,WriteBufferFile,Clipboard,PastePane}`.
- Consumes: Task 1 store, `TargetResolver`, and state-owner effects.

- [ ] **Step 1: Add failing parser/execution tests**

  Cover `set-buffer [-b name] [-w] data`, `load-buffer [-b name] path`,
  `save-buffer [-a] [-b name] path`, `show-buffer`, `list-buffers`,
  `delete-buffer`, and `paste-buffer [-b name] [-d] [-p] [-t pane]` including
  aliases and errors.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core command::tests::buffer -- --nocapture
  ```

- [ ] **Step 3: Implement core commands and semantic effects**

  Buffer mutations happen in `execute_state_command`. File reads/writes,
  clipboard requests, and pane delivery return explicit effects containing
  stable IDs and owned/`Arc` bytes; no core function opens a file or terminal.

- [ ] **Step 4: Implement bounded file handling in the owner**

  Read at most 16 MiB plus one sentinel byte before accepting a file. For save,
  write the selected immutable bytes with create/truncate or append semantics.
  Return path-qualified errors through the original command completion.

- [ ] **Step 5: Verify and commit**

  ```powershell
  cargo test -p wmux-core -p wmux-server buffer -- --nocapture
  cargo clippy -p wmux-core -p wmux-server --all-targets -- -D warnings
  git add wmux-clean/crates/wmux-core wmux-clean/crates/wmux-server
  git commit -m "feat(buffers): add commands and file effects"
  ```

---

### Task 3: Store copy-mode selections and throttle pane pastes

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`

**Interfaces:**
- Produces: private `PendingPaste` queue and conformance cases for copy/buffer/paste behavior.
- Consumes: screen bracketed-paste mode, `PasteBufferStore`, and `Message::Clipboard`.

- [ ] **Step 1: Add failing owner tests**

  Assert copy mode stores an automatic buffer before clipboard delivery,
  `set-clipboard=off` suppresses only clipboard delivery, a 1 MiB paste is sent
  in 64 KiB chunks, bracket wrappers appear once, payload bytes remain exact,
  and other commands/events progress between chunks.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-server paste -- --nocapture
  ```

- [ ] **Step 3: Implement owner scheduling**

  Add:

  ```rust
  struct PendingPaste { pane: PaneId, bytes: Arc<[u8]>, offset: usize, prefix_sent: bool, suffix_sent: bool, bracketed: bool }
  ```

  Process one 64 KiB chunk per pending paste per turn after control events and
  commands. Preserve FIFO ordering per pane and never allocate per byte.

- [ ] **Step 4: Extend conformance and run regressions**

  ```powershell
  cargo test --workspace
  cargo run -p wmux-conformance --release
  ```

- [ ] **Step 5: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-server wmux-clean/crates/wmux-conformance
  git commit -m "feat(buffers): throttle clipboard and pane paste flow"
  ```

