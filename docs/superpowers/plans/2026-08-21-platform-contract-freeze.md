# Phase 5 Cross-OS Platform Contract Freeze Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Freeze injectable pane, transport, terminal, identity, lifecycle, and error interfaces so the real server/client libraries compile without native dependencies and a mock backend runs the complete protocol lifecycle.

**Architecture:** Split `wmux-platform` into four deep OS-neutral modules, implement those interfaces in `wmux-windows`, and move platform selection into tiny binary composition roots. Preserve the single owner loop and per-pane bounded drain; validate the seam with memory transport, scripted PTYs, deterministic conformance, compile audits, and a release dispatch gate.

**Tech Stack:** Rust 2021, Tokio async byte streams, Windows ConPTY/IOCP/named pipes behind adapters, custom framed protocol v6, deterministic conformance and benchmark harnesses.

**Spec:** `docs/superpowers/specs/2026-08-21-platform-contract-freeze-design.md`

## Global Constraints

- Commit every completed task directly to `main`; do not create a phase branch or worktree.
- Do not stage, modify, or remove the pre-existing untracked root `.agents/` directory.
- Do not use subagents.
- No shared crate may import `std::os::windows`, `std::os::unix`, `windows-sys`, `libc`, or `wmux-windows`.
- Native selection is allowed only in `wmux-server/src/main.rs` and `wmux-client/src/main.rs`.
- Core/server identities remain wmux stable tokens; no handle, descriptor, process identifier used as a handle, console record, signal number, SID object, or uid/gid type crosses the seam.
- The server owner loop remains the only mux-state mutator.
- Exit and output closure remain distinct; final PTY bytes may arrive after child exit.
- `PlatformError` diagnostics are capped at exactly 4,096 UTF-8 bytes.
- Pane output budgets remain 64 KiB/one millisecond per pane and four milliseconds per round.
- No production behavior is written before a focused test fails for the expected missing behavior.
- The `platform-dispatch` gate is ten million operations, at least 20,000,000 operations/second, and zero measured allocations.
- UI/UX, Unix mechanics, options, hooks, buffers, and control mode remain out of scope.

---

### Task 1: Add the Frozen OS-Neutral Contract

**Files:**
- Create: `wmux-clean/crates/wmux-platform/src/error.rs`
- Create: `wmux-clean/crates/wmux-platform/src/pane.rs`
- Create: `wmux-clean/crates/wmux-platform/src/terminal.rs`
- Create: `wmux-clean/crates/wmux-platform/src/transport.rs`
- Modify: `wmux-clean/crates/wmux-platform/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-platform/Cargo.toml`

**Interfaces:**
- Consumes: current pane, mouse, terminal-size, request/event, and backend types.
- Produces: `PlatformError`, `PtyBackend`, `ServerPlatform`, `ServerListener`, `ClientTransport`, `TerminalBackend`, and re-exported semantic values.

- [ ] Write tests that catch unbounded errors, unsupported modifier bits, native-free opaque identity comparison, explicit exit/close ordering, and trait-object thread safety.
- [ ] Run `cargo test -p wmux-platform` and verify RED because the new types and behaviors do not exist.
- [ ] Implement the four modules and preserve source compatibility for existing mouse, size, pane ID, request, and output/exit constructors where practical.
- [ ] Replace blocking `next_event` with nonblocking `try_next_event(pane)` and add `PtyClosed` plus `BackendError`.
- [ ] Add Tokio `io-util` only for OS-neutral boxed `AsyncRead + AsyncWrite` streams; do not expose runtime handles.
- [ ] Run `cargo test -p wmux-platform`, formatting, clippy, and `git diff --check`.
- [ ] Commit with `feat(platform): freeze cross-os runtime contract`.

### Task 2: Implement Windows Adapters Behind the Contract

**Files:**
- Create: `wmux-clean/crates/wmux-windows/src/platform.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/conpty.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/console.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/pipe.rs`
- Modify: `wmux-clean/crates/wmux-windows/Cargo.toml`

**Interfaces:**
- Consumes: frozen platform traits and existing ConPTY, named-pipe, console, and WMI implementations.
- Produces: `WindowsServerPlatform`, `WindowsClientTransport`, `WindowsTerminalBackend`, and `WindowsPtyBackend`.

- [ ] Write failing tests for request mapping, missing-pane classification, exit-then-close, close-without-exit recovery, idempotent force termination, busy-pipe classification, peer verification, normalized terminal keys, and terminal restoration guard creation.
- [ ] Run focused Windows tests and verify RED on missing adapter types.
- [ ] Implement a pane map that owns every `ConptyPane` and receiver; on process exit finish ConPTY internally, and emit one explicit close after receiver disconnection.
- [ ] Implement a listener that owns the lock/factory/current named-pipe instance and verifies the SID before returning `AcceptedConnection`.
- [ ] Implement client transport with raw Windows connection errors classified inside the adapter and WMI daemon launch behind `DaemonSpec`.
- [ ] Implement terminal input/output/clipboard/size methods and convert crossterm keys to platform key values; remove the Windows crate's protocol dependency.
- [ ] Run `cargo test -p wmux-windows`, formatting, clippy, and `git diff --check`.
- [ ] Commit with `feat(windows): implement frozen platform adapters`.

### Task 3: Inject the Pane and Listener Backends into the Server

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-server/src/main.rs`
- Modify: `wmux-clean/crates/wmux-server/Cargo.toml`

**Interfaces:**
- Consumes: `Box<dyn ServerPlatform>`, `Box<dyn ServerListener>`, and `Box<dyn PtyBackend>`.
- Produces: `run_with_platform`, a native-free owner loop, and a Windows-only composition root.

- [ ] Replace channel/controller test setup with a scripted `PtyBackend`; write failing server tests for spawn/write/resize/terminate request emission, exit/close ordering, backend error recovery, and wrong-peer rejection before `Hello`.
- [ ] Run focused tests and verify RED because `ServerOwner` still constructs ConPTY and the accept loop still owns named pipes.
- [ ] Refactor `Runtime` to own `Box<dyn PtyBackend>` and store only pane size/lifecycle flags; submit semantic requests for all mechanics.
- [ ] Refactor bounded output collection to call `try_next_event` while retaining the existing byte/time/round budgets.
- [ ] Refactor the accept loop and handshake over `AcceptedConnection`; compare opaque peer and owner identities before decoding the first frame.
- [ ] Move Windows construction to `main.rs` under `cfg(windows)` and make the non-Windows binary report that its native backend arrives in Phase 6.
- [ ] Make `wmux-windows` a Windows-target dependency and verify `cargo check -p wmux-server --lib` has no native selection.
- [ ] Run server tests, workspace tests, formatting, clippy, and `git diff --check`.
- [ ] Commit with `refactor(server): inject platform runtime backends`.

### Task 4: Inject Transport and Terminal Backends into the Client

**Files:**
- Create: `wmux-clean/crates/wmux-client/src/lib.rs`
- Replace: `wmux-clean/crates/wmux-client/src/main.rs`
- Modify: `wmux-clean/crates/wmux-client/Cargo.toml`

**Interfaces:**
- Consumes: `Arc<dyn ClientTransport>` and `Arc<dyn TerminalBackend>`.
- Produces: shared `run_with_platform`, protocol/attach loops independent of native APIs, and a Windows-only composition root.

- [ ] Move existing policy tests to the library and add failing memory-adapter tests for connection startup classification, attach input/output, resize, clipboard, restoration, and exact semantic-key protocol conversion.
- [ ] Run `cargo test -p wmux-client --lib` and verify RED on missing injected construction.
- [ ] Replace named-pipe concrete parameters with `BoxedIpcStream`; pass cloned terminal adapters to the input worker and attach loop.
- [ ] Replace raw Windows error checks with `PlatformErrorKind`; keep all current human diagnostics and retry timing.
- [ ] Use `std::env::consts::EXE_SUFFIX` for portable server executable discovery and send the same protocol v6 handshake.
- [ ] Move Windows adapter creation to `main.rs`, use a target dependency, and verify the shared library has no native import.
- [ ] Run client tests, workspace tests, formatting, clippy, and `git diff --check`.
- [ ] Commit with `refactor(client): inject transport and terminal backends`.

### Task 5: Prove the Complete Lifecycle with a Mock Platform

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-conformance/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`

**Interfaces:**
- Consumes: real server entry point, framed protocol, memory listener, and scripted PTY events.
- Produces: full create/attach/type/split/resize/detach/output/reattach/kill/shutdown evidence.

- [ ] Write a failing real-protocol lifecycle test using Tokio duplex streams and a scripted backend; name each missing observable transition.
- [ ] Run the focused lifecycle test and verify RED before adding its harness support.
- [ ] Implement only the public configured server entry point needed to inject deterministic config; keep owner internals private.
- [ ] Drive the lifecycle with protocol v6 frames, assert recorded requests and rendered background output, reject a wrong peer before `Hello`, and join the server after `kill-server`.
- [ ] Add malformed and out-of-order event cases that prove one exit, one close, no panic, and no state mutation from unknown pane events.
- [ ] Run server and conformance tests plus workspace verification.
- [ ] Commit with `test(server): prove mock platform lifecycle`.

### Task 6: Freeze Conformance, Differences, and Portable CI

**Files:**
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`
- Modify: `.github/workflows/ci.yml`
- Modify: `wmux-clean/docs/event-contract.md`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Create: `wmux-clean/docs/platform-contract.md`

**Interfaces:**
- Consumes: complete frozen contract and lifecycle evidence.
- Produces: deterministic lifecycle fingerprint, `EXPECTED_DIFFERENCES`, shared-crate compile matrix, and reference documentation.

- [ ] Write failing tests requiring a platform-lifecycle case and centralized registry lookup with no conditional assertion paths.
- [ ] Run conformance tests and verify RED because the case and registry are absent.
- [ ] Add the deterministic case, update the aggregate fingerprint, and document that the Phase 5 semantic-difference registry is empty.
- [ ] Extend the CI OS matrix to check/test core, platform, protocol, config, server library, client library, and conformance without selecting a native crate.
- [ ] Document exact request/event ordering, identity/security obligations, terminal restoration, error classification, and contract-change rule.
- [ ] Run conformance, shared compile checks, formatting, clippy, and source audits.
- [ ] Commit with `docs: freeze cross-os platform conformance`.

### Task 7: Add Dispatch Performance and Complete the Phase Gate

**Files:**
- Modify: `wmux-clean/crates/wmux-bench/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-bench/src/main.rs`
- Modify: `wmux-clean/docs/performance.md`
- Modify: `wmux-clean/docs/performance-gates.md`
- Modify: `wmux-clean/docs/platform-contract.md`
- Modify: `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md`

**Interfaces:**
- Consumes: trait-object pane dispatch and all Phase 5 evidence.
- Produces: `platform-dispatch` release gate and final recorded verification.

- [ ] Write a failing benchmark registration/allocation test for `platform-dispatch`.
- [ ] Run the benchmark tests and verify RED because the workload is absent.
- [ ] Implement ten million deterministic resize submissions through `&mut dyn PtyBackend`, checksum all values, assert zero allocations, and enforce at least 20,000,000 operations/second.
- [ ] Run the focused release workload and optimize adapter dispatch before considering any threshold change.
- [ ] Run the full gate: format, clippy with warnings denied, workspace tests, fuzz build/clippy, conformance, full release benchmarks, shared crate checks, native-import audits, and `git diff --check`.
- [ ] Record exact test count, fingerprint, benchmark result, reference revisions, compile evidence, and any extra work in `platform-contract.md`.
- [ ] Mark Task 5 roadmap checkboxes complete only after every gate passes.
- [ ] Commit with `docs: record phase 5 platform contract evidence` and verify tracked status is clean except root `.agents/`.

## Phase 5 Exit Gate

Phase 5 is complete only when the shared server and client libraries have no
native dependency, both real Windows binaries use injected adapters, exit and
closure remain distinct semantic events, a mock platform passes the complete
real-protocol lifecycle, portable conformance has one expected-differences
registry, the dispatch and all previous performance gates pass, and every
verification command above is green on `main`.
