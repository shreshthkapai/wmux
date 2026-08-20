# Cross-Platform Beta Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn the current `wmux-clean` implementation into a reliable Windows, Linux, and macOS beta while keeping all visual UI/UX, flair, and visual configuration decisions in the final implementation phase.

**Architecture:** Preserve the existing server-authoritative, OS-neutral core and validate it through a shared conformance contract. Complete portable correctness and command/input architecture first, add Linux and macOS through one Unix backend, then finish shared multiplexer semantics across all platforms. Pane application styling remains authoritative; later mux chrome is composed around pane content and must not rewrite pane cells or override the host terminal theme.

**Tech Stack:** Rust 2021 workspace, Tokio, `vte`, Windows ConPTY/IOCP/named pipes, Unix PTY/termios/process groups/AF_UNIX, shared deterministic conformance fixtures.

**Spec:** `AGENTS.md` and `docs/windows-first-cross-os-execution-plan.md`. This roadmap supersedes only the old phase ordering that placed Unix after advanced UI; the architectural rules remain unchanged.

## Global Constraints

- Treat `wmux-clean/` as the current implementation source until Task 1 establishes a single canonical workspace.
- Core, protocol, conformance, configuration, and server semantics must not import Windows or Unix APIs.
- Raw handles, file descriptors, PIDs used as handles, console records, and signal numbers must not cross the platform boundary.
- The server event loop remains the only mutator of sessions, windows, panes, clients, layouts, modes, options, buffers, jobs, and command queues.
- Pane output continues through `PTY bytes -> TerminalEngine -> Screen/Grid -> client-scoped renderer`.
- No core or portability phase may introduce visual themes, decorative borders, palettes, icons, gradients, animations, or user-facing flair.
- Pane application `Style` values and terminal-default colours remain authoritative. Mux chrome must be a separate compositing layer and may style only cells it owns.
- Visual options and final configuration names are reserved for Task 10, after the user supplies the UI/UX specification.
- Functional configuration needed for keys, commands, shell selection, history, security, and lifecycle may be implemented before Task 10.
- Research local tmux and zellij before every semantic change, and official platform documentation before every native backend change.
- Use test-driven development for every phase-specific implementation plan: failing focused test, minimal implementation, focused pass, workspace pass, commit.
- Do not allocate, schedule a task, emit an IPC frame, trigger a redraw, or perform a terminal write per byte.
- Every shared semantic change must pass on Windows, Linux, and macOS once those CI jobs exist.
- UI/UX work begins only after the Beta Core Gate in Task 9 passes and the user approves a separate UI specification.

## Roadmap Execution Rules

This document is the master dependency and acceptance roadmap. Each task below is independently reviewable and must receive its own bite-sized implementation plan before code changes begin. Do not combine two tasks into one implementation branch.

Paths below describe the current `wmux-clean/` layout. If Task 1 promotes that workspace to the repository root, each phase-specific plan must use the promoted canonical path and must not recreate the nested layout.

The safe concurrency model is:

```text
Tasks 1-5: sequential foundation and contract freeze

After Task 5:
  Lane A -> Task 6 Linux backend
  Lane B -> remaining Windows lifecycle/security stress coverage
  Lane C -> cross-OS CI and conformance fixtures

Task 7 macOS begins after Linux proves the shared Unix backend.
Task 8 shared semantics begins after Linux native smoke passes.
Tasks 9-11 return to sequential release gates.
```

If only one engineer or agent is active, run Lane A, Lane B, then Lane C in that order. Parallel work is useful only when each lane has a separate worktree and does not edit the same files.

---

### Task 1: Establish One Canonical Workspace and Reproducible Baseline

**Files:**
- Inspect: `Cargo.toml`
- Inspect: `crates/**`
- Inspect: `wmux-clean/Cargo.toml`
- Inspect: `wmux-clean/crates/**`
- Create: `docs/baseline/cross-platform-beta-baseline.md`
- Create or modify: CI workflow files selected by the repository after Git metadata is restored

**Interfaces:**
- Consumes: the older root workspace and the newer `wmux-clean` workspace.
- Produces: one declared source of truth, clean repository metadata, and reproducible baseline commands used by every later gate.

- [ ] Inventory both workspaces by crate, source file, test count, documentation, and generated artifacts; record the comparison in `docs/baseline/cross-platform-beta-baseline.md`.
- [ ] Declare the newer implementation as canonical unless the inventory finds functionality present only in the root workspace; explicitly list every root-only item that must be ported.
- [ ] Write a separate safe consolidation plan that preserves both trees until the canonical workspace builds and all baseline checks pass. Do not delete either tree during discovery.
- [ ] Restore usable Git history or initialize a clean repository boundary before feature work so every phase can be reviewed and reverted.
- [ ] Exclude `target/`, `target-*`, logs, lock files, and packaged binaries from source control.
- [ ] Run the canonical Windows baseline:

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo run -p wmux-conformance --release
cargo run -p wmux-bench --release -- --suite full --gate
```

- [ ] Record toolchain version, test counts, conformance fingerprint, performance results, supported terminal/shell combinations, and known failures in the baseline document.
- [ ] Commit the canonical-workspace declaration, ignore rules, baseline document, and CI foundation as one reviewable repository-foundation change.

**Exit gate:** One workspace is authoritative; a clean checkout can reproduce the Windows test, conformance, lint, formatting, and performance baseline.

---

### Task 2: Complete Portable Terminal Correctness

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/grid.rs`
- Modify: `wmux-clean/crates/wmux-core/src/screen.rs`
- Modify: `wmux-clean/crates/wmux-core/src/terminal.rs`
- Modify: `wmux-clean/crates/wmux-core/src/render.rs`
- Modify: `wmux-clean/crates/wmux-core/Cargo.toml`
- Create: `wmux-clean/crates/wmux-core/src/text.rs`
- Create: `wmux-clean/fuzz/Cargo.toml`
- Create: `wmux-clean/fuzz/fuzz_targets/protocol_frame.rs`
- Create: `wmux-clean/fuzz/fuzz_targets/terminal_bytes.rs`
- Extend: `wmux-clean/crates/wmux-conformance/src/lib.rs`

**Interfaces:**
- Consumes: the current `Cell`, `Line`, `Grid`, `Screen`, `TerminalEngine`, and renderer APIs.
- Produces: a compact `CellText` representation that preserves base characters plus combining code points, deterministic width calculation, and crash-resistant parser/protocol fuzz targets.

- [x] Research tmux UTF-8 cell storage and zellij terminal-cell/grapheme handling; record the chosen compatibility model in `wmux-clean/docs/terminal-text-model.md`.
- [x] Write failing unit tests for combining marks, variation selectors, emoji sequences, wide-cell replacement, split UTF-8, zero-width input at column zero, resize/reflow, and copy-mode extraction.
- [x] Replace the `char`-only printable payload with `CellText`, using inline storage for the common one-scalar case and owned overflow only for combined sequences.
- [x] Replace the hand-maintained width ranges with a reviewed Unicode width dependency and pin the dependency version in `Cargo.lock`.
- [x] Preserve terminal-default colours as `Color::Default`; do not resolve them to a palette in core.
- [x] Extend renderer and copy-mode tests to prove combined text survives full render, diff render, scrollback, selection, resize, detach, and reattach.
- [x] Add bounded parser limits for CSI, OSC, DCS, and string controls, with malformed-input tests that prove recovery without panic or unbounded memory.
- [x] Add protocol and terminal-byte fuzz targets with checked-in seed corpora derived from the conformance fixtures.
- [x] Add deterministic conformance cases for multilingual prompts, combining text, wide text, emoji, alternate screen, synchronized output, OSC titles, and malformed control strings.
- [x] Run focused tests, the full workspace suite, conformance, and performance gate. Reject the design if cell storage materially regresses the existing performance thresholds.

**Exit gate:** Pane text round-trips through parser, grid, scrollback, copy mode, resize, rendering, detach, and reattach without losing combining data or changing application styles.

---

### Task 3: Harden IPC, Daemon Lifecycle, CLI, and Windows Security

**Files:**
- Modify: `wmux-clean/crates/wmux-windows/src/pipe.rs`
- Modify: `wmux-clean/crates/wmux-windows/src/conpty.rs`
- Modify: `wmux-clean/crates/wmux-client/src/main.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-protocol/src/lib.rs`
- Create: `wmux-clean/crates/wmux-cli/Cargo.toml`
- Create: `wmux-clean/crates/wmux-cli/src/lib.rs`
- Extend: `wmux-clean/docs/cross-os-conformance.md`

**Interfaces:**
- Consumes: protocol version 5, current named-pipe endpoint, client server-spawn path, and server lock.
- Produces: shared CLI parsing, authenticated per-user IPC endpoints, explicit daemon ownership, graceful shutdown, and stable user-facing diagnostics.

- [ ] Research tmux server discovery/startup and Windows named-pipe security, token/SID validation, console process groups, and Job Object breakaway behavior.
- [ ] Write failing CLI tests for `--help`, `--version`, no-server diagnostics, implicit server startup for state-creating commands, explicit failure for read-only commands when policy requires it, and protocol mismatch messaging.
- [ ] Move invocation classification and command argv handling into `wmux-cli`; keep terminal attachment mechanics in `wmux-client`.
- [ ] Define endpoint identity from the current user SID rather than mutable username text.
- [ ] Create the named pipe with an explicit DACL scoped to the owning SID and verify the connected client token before accepting protocol messages.
- [ ] Make server startup explicitly independent from the launching client console and document the exact Windows process/job ownership rules.
- [ ] Replace unconditional process termination in graceful shutdown with an owner-loop shutdown sequence that closes listeners, drains control replies, terminates configured pane jobs, releases the lock, and exits.
- [ ] Add native tests for stale lock recovery, stale endpoint recovery, client crash, terminal-tab closure, server restart, wrong-user connection rejection, kill-server with attached clients, and ConPTY EOF/exit races.
- [ ] Update protocol documentation to version 5 and add a check preventing documentation/version drift.
- [ ] Run the Windows compatibility smoke matrix in Windows Terminal, conhost, and VS Code terminal with PowerShell 7, Windows PowerShell, and cmd.exe.

**Exit gate:** Closing or crashing any client cannot kill detached sessions; graceful shutdown leaves no endpoint/lock debris; another user cannot connect; normal CLI failures are human-readable.

---

### Task 4: Move Commands, Targets, and Key Tables into Server-Owned Core

**Files:**
- Split: `wmux-clean/crates/wmux-core/src/command.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/lexer.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/parser.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Create: `wmux-clean/crates/wmux-core/src/target.rs`
- Create: `wmux-clean/crates/wmux-core/src/keys.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-client/src/main.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`

**Interfaces:**
- Consumes: current `Command`, `CommandQueue`, `ServerState`, `ClientInput`, and hardcoded prefix translation.
- Produces: `CommandList`, `TargetSpec`, `TargetResolver`, `KeyCode`, `KeyTable`, and `KeyBinding`, all owned and executed by the server.

- [ ] Research tmux `cmd-parse`, target resolution, key tables, repeatable bindings, and default destructive confirmations; compare zellij input normalization where it maps cleanly to Rust.
- [ ] Write lexer golden tests for quotes, escapes, empty arguments, comments, semicolon chains, command aliases, unique prefixes, and invalid syntax spans.
- [ ] Implement one lexer/parser path used by CLI argv, IPC command strings, config files, key bindings, hooks, and control clients.
- [ ] Write target-resolution tests for client, session, window, winlink, pane, relative, current, last, ambiguous, and absent targets.
- [ ] Implement centralized `TargetResolver`; remove command-specific ad hoc lookup logic.
- [ ] Write key-table tests for root/prefix/copy tables, repeat time, send-prefix, literal passthrough, bracketed paste, mouse precedence, and client-independent bindings.
- [ ] Move prefix interpretation from `wmux-client` into server-owned key tables; the client should only normalize terminal input and transmit semantic input events.
- [ ] Implement `bind-key`, `unbind-key`, `list-keys`, `send-keys`, `send-prefix`, `switch-client`, and `refresh-client` through the serialized command queue.
- [ ] Represent kill-pane and kill-window defaults as confirmation commands in the binding table; the confirmation UI remains a plain functional prompt until Task 10.
- [ ] Add malformed-command property tests and verify that one client cannot block or corrupt another client's queue.

**Exit gate:** All command sources share one parser, all targets share one resolver, and clients contain no multiplexer key-binding policy.

---

### Task 5: Freeze the Cross-OS Platform and Conformance Contract

**Files:**
- Modify: `wmux-clean/crates/wmux-platform/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-client/src/main.rs`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`
- Modify: `wmux-clean/docs/event-contract.md`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Create: `wmux-clean/docs/platform-contract.md`

**Interfaces:**
- Consumes: existing `PtyBackend`, `TerminalBackend`, `PlatformRequest`, and `PlatformEvent`.
- Produces: the minimal semantic contract required by both ConPTY and Unix PTYs, plus a platform-neutral server runtime entry point.

- [ ] Audit every direct `wmux-windows` import in client/server and classify it as PTY, terminal, IPC, credential, daemon, clock, or filesystem behavior.
- [ ] Extend the platform contract only for semantics required by both implementations: endpoint discovery, listener/connection streams, peer identity, terminal resize events, child exit, pane closure, and backend error classification.
- [ ] Keep `PlatformPaneId` opaque and ensure no native handle, descriptor, or process identifier becomes core identity.
- [ ] Refactor server construction to accept backend implementations rather than selecting Windows types inside the owner loop.
- [ ] Refactor client construction to accept terminal and IPC backends while preserving the same attach protocol.
- [ ] Move platform-neutral server tests out of Windows-only build paths.
- [ ] Expand `wmux-conformance` with lifecycle scripts for create, attach, type, split, resize, detach, background output, reattach, kill-pane, kill-session, and kill-server.
- [ ] Define an expected-differences registry; OS differences must be explicit entries, never scattered conditional assertions.
- [ ] Add compile checks proving `wmux-core`, `wmux-protocol`, `wmux-config`, `wmux-server`, and `wmux-conformance` build without Windows features.
- [ ] Freeze the contract for the Unix bring-up. Any later contract change requires Windows and Unix tests in the same change.

**Exit gate:** A mock backend can run the complete server lifecycle and no shared crate imports native APIs.

---

### Task 6: Implement the Linux Backend and Start Two-Platform Development

**Files:**
- Create: `wmux-clean/crates/wmux-unix/Cargo.toml`
- Create: `wmux-clean/crates/wmux-unix/src/lib.rs`
- Create: `wmux-clean/crates/wmux-unix/src/pty.rs`
- Create: `wmux-clean/crates/wmux-unix/src/process.rs`
- Create: `wmux-clean/crates/wmux-unix/src/ipc.rs`
- Create: `wmux-clean/crates/wmux-unix/src/terminal.rs`
- Create: `wmux-clean/crates/wmux-unix/src/signals.rs`
- Modify: `wmux-clean/Cargo.toml`
- Modify: client/server manifests and entry points for target-specific backend selection
- Create: Linux CI job and native integration-test scripts

**Interfaces:**
- Consumes: the frozen Task 5 platform contract.
- Produces: AF_UNIX IPC, PTY process spawning, process-group cleanup, termios client mode, signal integration, and Linux-native conformance.

- [ ] Research zellij's Rust Unix PTY/client implementation, tmux server/socket/process semantics, and official Linux PTY, termios, `setsid`, process-group, `SIGCHLD`, `SIGHUP`, and `SIGWINCH` documentation.
- [ ] Implement a per-user runtime socket path with owner-only permissions and peer UID verification.
- [ ] Implement PTY allocation, session/process-group creation, controlling-terminal setup, exec error reporting, nonblocking master IO, resize ioctl, and exit reaping.
- [ ] Implement process-group termination with graceful and forced modes and prove that descendants do not survive explicit pane/session/server kills.
- [ ] Implement terminal raw-mode entry, panic/error restoration guards, resize event delivery, input reads, and batched output writes.
- [ ] Make bare `wmux`, detached commands, attach, detach, and server startup follow the same CLI semantics as Windows.
- [ ] Add native tests for PTY round-trip, EOF/exit ordering, process-tree cleanup, socket permissions, peer identity, stale endpoint recovery, terminal restoration, and resize storms.
- [ ] Run the full portable suite and compare conformance fingerprints with Windows.
- [ ] Run interactive smoke tests using bash, zsh where available, vim/neovim, less, Git interactive commands, and high-volume build output.
- [ ] Keep Windows CI required for every Unix change.

**Exit gate:** Linux can create, attach, split, resize, detach, produce background output, reattach from authoritative state, and clean up process trees with the same semantic fingerprint as Windows.

---

### Task 7: Extend the Unix Backend to macOS

**Files:**
- Modify: `wmux-clean/crates/wmux-unix/src/*.rs`
- Add: macOS-specific modules only where compile-time differences require them
- Add: macOS CI job and native integration-test scripts
- Extend: `wmux-clean/docs/cross-os-conformance.md`

**Interfaces:**
- Consumes: the Linux-proven `wmux-unix` backend.
- Produces: macOS PTY, process, socket, terminal, and signal behavior behind the same public Unix backend interfaces.

- [ ] Research official macOS PTY, termios, process-group, peer-credential, signal, and Unix-socket behavior before adding target conditionals.
- [ ] Compile the Unix crate on macOS and isolate every difference behind focused private modules or `cfg` blocks.
- [ ] Add macOS-native tests matching the Linux native contract: PTY round-trip, EOF/exit, descendant cleanup, socket identity/permissions, raw-mode restoration, and resize storms.
- [ ] Run the same portable conformance binary and require the same aggregate semantic fingerprint.
- [ ] Exercise Terminal.app, iTerm2 if available, and VS Code terminal with zsh, bash, vim/neovim, less, and build output.
- [ ] Document unavoidable OS-mechanism differences without changing shared mux semantics.

**Exit gate:** macOS passes the portable suite and native lifecycle contract without introducing macOS types or conditionals into core.

---

### Task 8: Complete Shared Multiplexer Semantics Across All Platforms

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/options.rs`
- Create: `wmux-clean/crates/wmux-core/src/formats.rs`
- Create: `wmux-clean/crates/wmux-core/src/paste.rs`
- Create: `wmux-clean/crates/wmux-core/src/hooks.rs`
- Create: `wmux-clean/crates/wmux-core/src/jobs.rs`
- Create: `wmux-clean/crates/wmux-core/src/control.rs`
- Modify: `wmux-clean/crates/wmux-config/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-protocol/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Extend: `wmux-clean/crates/wmux-conformance/src/lib.rs`

**Interfaces:**
- Consumes: cross-platform commands, targets, key tables, backend lifecycle, and versioned protocol.
- Produces: nonvisual option/format state, functional configuration, paste buffers, hooks, bounded jobs, and control-mode records.

- [ ] Implement typed option scopes and inheritance for server, session, window, pane, and client state; do not choose visual defaults beyond neutral functional behavior.
- [ ] Implement a deterministic format engine over server-owned values, with bounded recursion and explicit error reporting.
- [ ] Implement config discovery and source execution using the shared command parser; keep visual configuration keys reserved for Task 10.
- [ ] Implement server-owned named and automatic paste buffers, large-paste throttling, byte preservation, and platform clipboard adapters.
- [ ] Implement event notifications and option-backed hooks through serialized command queues, with recursion limits and deterministic ordering.
- [ ] Implement bounded asynchronous jobs and command continuations; platform backends own process mechanics while core owns job identity and queue state.
- [ ] Implement versioned control-mode command replies, lifecycle notifications, and backpressured pane-output subscriptions.
- [ ] Extend conformance for option inheritance, format expansion, config parsing, buffers, hook ordering, jobs, and control records.
- [ ] For every semantic feature, run the same tests on Windows, Linux, and macOS before merging.

**Exit gate:** The remaining nonvisual tmux-style automation and configuration backbone works consistently on all three platforms.

---

### Task 9: Pass the Cross-Platform Beta Core Gate

**Files:**
- Extend: `wmux-clean/crates/wmux-bench/src/main.rs`
- Extend: `wmux-clean/crates/wmux-conformance/src/lib.rs`
- Create: `wmux-clean/tests/stress/` scripts or a dedicated stress crate
- Create: `wmux-clean/docs/compatibility-matrix.md`
- Create: `wmux-clean/docs/known-differences.md`
- Create: `wmux-clean/docs/beta-core-gate.md`

**Interfaces:**
- Consumes: all portable semantics and three native backends.
- Produces: measured evidence that the core is ready for final UI integration.

- [ ] Run repeated attach/detach, client-crash, server-restart, resize-storm, pane-exit, kill-during-output, slow-client, many-client, many-pane, 100k-line history, and large-paste stress scenarios.
- [ ] Run parser and protocol fuzzing for a recorded duration and store corpus/crash-handling instructions.
- [ ] Verify terminal mode restoration after normal detach, client error, panic, parent-terminal closure, and forced client termination on every platform.
- [ ] Verify process-tree cleanup after pane, window, session, and server termination on every platform.
- [ ] Verify bounded memory and queues under detached output, slow clients, synchronized output, resize storms, and control-mode subscriptions.
- [ ] Run the full performance gate on representative release builds and compare against the Task 1 baseline.
- [ ] Complete the terminal × shell × scenario compatibility matrix for Windows, Linux, and macOS.
- [ ] Classify every discrepancy as a shared bug, native backend bug, intentional platform difference, or unsupported terminal capability.
- [ ] Require formatting, clippy, all tests, portable conformance, native conformance, fuzz smoke, stress suite, and release performance gates in CI.

**Exit gate — Beta Core:** No known session-loss bug, cross-client corruption, privilege-boundary failure, terminal-restoration failure, process-tree leak, unbounded queue, parser crash, or unexplained cross-OS semantic divergence remains open.

---

### Task 10: UI/UX, Terminal-Theme Inheritance, and User Flair Hold Point

**Files:**
- Create only after user design approval: `docs/superpowers/specs/2026-08-20-wmux-ui-ux-design.md`
- Expected core additions after approval: a chrome/status model and overlay/mode rendering modules under `wmux-core`
- Expected config additions after approval: visual option definitions under `wmux-config`
- Expected renderer changes after approval: compositing in `wmux-core/src/render.rs`

**Interfaces:**
- Consumes: stable pane scenes, formats, options, modes, commands, keys, and cross-platform terminal capabilities.
- Produces: mux-owned chrome and interaction surfaces without altering pane-owned application styling.

- [ ] Stop before implementation and collect the user's flair, layout, status, border, mode, prompt, motion, icon, and configuration requirements.
- [ ] Produce two or three UI approaches and mockups against the real terminal constraints; obtain explicit user approval.
- [ ] Specify a separate `ChromeModel`/overlay layer. Pane cells remain unchanged; chrome occupies reserved status rows, border cells, or temporary overlays.
- [ ] Define theme inheritance so `Color::Default` continues to mean the host terminal's default colour and application-emitted SGR styles are never remapped by mux themes.
- [ ] Define capability fallbacks for true colour, 256 colour, basic colour, Unicode border glyphs, ASCII borders, mouse, synchronized output, and unsupported terminal features.
- [ ] Add configurable status, pane borders, active-pane indication, copy/search state, prompts, confirmation surfaces, help/key discovery, messages, menus, and popups according to the approved design.
- [ ] Keep every visual option scoped to mux chrome. Any option capable of transforming pane application content requires separate explicit approval.
- [ ] Add golden scene tests for each capability profile and interaction tests proving overlay input never leaks into panes.
- [ ] Run UI smoke tests across the compatibility matrix without weakening any Beta Core gate.

**Exit gate:** The user approves the final terminal UI; pane applications retain their own colours/theme; mux flair is optional, capability-aware, and fully confined to chrome.

---

### Task 11: Package and Publish the Cross-Platform Beta

**Files:**
- Create or modify: platform package definitions and release workflow
- Create: `README.md`
- Create: `docs/installation.md`
- Create: `docs/commands.md`
- Create: `docs/configuration.md`
- Create: `docs/troubleshooting.md`
- Create: `docs/security.md`
- Create: `CHANGELOG.md`

**Interfaces:**
- Consumes: approved UI and all Beta Core evidence.
- Produces: installable, diagnosable, versioned beta artifacts for Windows, Linux, and macOS.

- [ ] Define supported OS versions, architectures, terminals, shells, upgrade policy, config compatibility policy, and protocol compatibility policy.
- [ ] Produce signed or checksummed release artifacts with Windows installer/package, Linux archives or packages, and macOS universal or per-architecture artifacts.
- [ ] Add shell completion generation and accurate `--help`, `--version`, command, target, key, and config references.
- [ ] Document default endpoint locations, logs, crash diagnostics, security boundaries, known differences, and recovery from stale servers/endpoints.
- [ ] Test clean install, first run, implicit server start, create/attach/detach/reattach, upgrade, protocol mismatch, uninstall, and reinstall on clean machines or VMs.
- [ ] Run every Task 9 gate against the exact packaged binaries rather than Cargo-built development binaries.
- [ ] Tag the beta only after packaged-binary evidence is attached to the release checklist.

**Exit gate — Public Beta:** A new user can install wmux, preserve sessions through client restarts, use the documented core workflow on Windows/Linux/macOS, diagnose failures, and uninstall cleanly.

## Phase Plan Sequence

Before implementing each task, create and review these focused plans in order:

1. `canonical-workspace-and-ci`
2. `terminal-text-and-fuzzing`
3. `windows-ipc-lifecycle-cli`
4. `commands-targets-key-tables`
5. `platform-contract-freeze`
6. `linux-backend`
7. `macos-backend`
8. `shared-mux-semantics` — split into options/formats, buffers/clipboard, hooks/jobs, and control-mode plans
9. `cross-platform-beta-core-gate`
10. `wmux-ui-ux` — created only after receiving the user's final design/config requirements
11. `cross-platform-beta-packaging`

## Definition of Done

The roadmap is complete only when all task exit gates pass. Passing the Windows tests alone is not cross-platform beta evidence; passing portable tests alone is not native-backend evidence; passing the Beta Core Gate without Task 10 is an internal beta candidate, not the public beta.
