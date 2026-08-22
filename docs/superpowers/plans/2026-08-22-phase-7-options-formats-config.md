# Phase 7 Options, Formats, and Config Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add typed inherited nonvisual options, deterministic target-aware formats, and command-based functional configuration.

**Architecture:** `ServerState` owns deep `OptionStore` and `FormatEngine` core modules. Commands mutate state through the existing queue; filesystem config reads stay in `wmux-config`/`wmux-server`, and all executable config text goes through `parse_command_text`.

**Tech Stack:** Rust 2021, existing `wmux-core`, `wmux-config`, `wmux-server`, shared command parser and target resolver.

**Spec:** `docs/superpowers/specs/2026-08-22-phase-7-shared-mux-semantics-design.md`

## Global Constraints

- Commit tested slices directly to `main`; do not create a branch or worktree.
- Do not use subagents.
- Add no visual option, chrome, palette, border, icon, or pane-style behavior.
- Core/config/server source must remain free of Windows and Unix APIs.
- The state-owner thread is the only option/config mutator.
- Input/output limits and error strings must match the design spec exactly.
- Start each production change with a focused failing test and record the RED result.

---

### Task 1: Add the typed inherited option module

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/options.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`

**Interfaces:**
- Produces: `OptionScope`, `OptionTarget`, `OptionValue`, `OptionError`, `OptionStore::{set,unset,get,list,remove_target}`.
- Consumes: stable client/session/window/pane IDs and an `OptionParents` value built from `ServerState`.

- [ ] **Step 1: Add failing inheritance and validation tests**

  Add tests for local-over-parent resolution, all five target scopes, user `@`
  strings, wrong types, unknown names, value bounds, deterministic listing, and
  object cleanup. Use these public shapes:

  ```rust
  let target = OptionTarget::Pane(pane);
  state.options.set(target, "history-limit", "5000")?;
  assert_eq!(state.option(target, "history-limit")?, OptionValue::Number(5000));
  state.options.unset(target, "history-limit")?;
  ```

- [ ] **Step 2: Run the focused tests and verify RED**

  Run:

  ```powershell
  cargo test -p wmux-core options::tests -- --nocapture
  ```

  Expected: compilation fails because `options` and its exported types do not
  exist.

- [ ] **Step 3: Implement definitions, parsing, and inheritance**

  Implement:

  ```rust
  pub enum OptionTarget { Server, Session(SessionId), Window(WindowId), Pane(PaneId), Client(ClientId) }
  pub enum OptionValue { Flag(bool), Number(i64), String(String) }
  pub struct OptionParents { pub session: Option<SessionId>, pub window: Option<WindowId> }
  pub struct OptionStore { values: BTreeMap<OptionTarget, BTreeMap<String, OptionValue>> }
  ```

  Register only `buffer-limit`, `history-limit`, `remain-on-exit`,
  `repeat-time`, `set-clipboard`, and `exit-empty`; accept bounded `@name`
  string options. Resolve pane -> window -> session -> server and client ->
  attached session -> server without retaining object references.

- [ ] **Step 4: Integrate lifecycle cleanup and pass tests**

  Add `ServerState::option`, build parents from the centralized stores, and call
  `remove_target` whenever a client/session/window/pane is destroyed. Run:

  ```powershell
  cargo test -p wmux-core options::tests state::tests -- --nocapture
  ```

  Expected: all focused tests pass.

- [ ] **Step 5: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-core/src
  git commit -m "feat(options): add typed inherited state"
  ```

---

### Task 2: Add option commands through the shared parser and resolver

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`

**Interfaces:**
- Consumes: Task 1 `OptionStore` and existing `TargetResolver`.
- Produces: `Command::{SetOption,ShowOptions}` and aliases `set`, `setw`, `show`, `showw`.

- [ ] **Step 1: Add parser and execution tests that fail**

  Cover `set-option [-g|-w|-p|-c] [-t target] [-u] name [value]`, window
  aliases, inherited versus local `show-options`, invalid mixed scopes, and
  target errors. Assert that target selection goes through `TargetResolver`.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core command::tests::option -- --nocapture
  ```

  Expected: the commands are unknown.

- [ ] **Step 3: Implement parse, format, execute, and command effects**

  Add:

  ```rust
  Command::SetOption { scope: OptionScope, target: Option<TargetSpec>, unset: bool, name: String, value: Option<String> }
  Command::ShowOptions { scope: OptionScope, target: Option<TargetSpec>, local_only: bool }
  ```

  Resolve the selected object once, mutate/list via `OptionStore`, and emit no
  native effect. Serialize commands through `format_command` so bind/config
  round trips remain exact.

- [ ] **Step 4: Run command/core regressions and commit**

  ```powershell
  cargo test -p wmux-core
  git add wmux-clean/crates/wmux-core/src/command
  git commit -m "feat(commands): expose typed options"
  ```

---

### Task 3: Add the bounded deterministic format engine

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/formats.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`

**Interfaces:**
- Produces: `FormatContext`, `FormatError`, and `FormatEngine::expand(&ServerState, FormatContext, &str) -> Result<String, FormatError>`.
- Consumes: Task 1 inherited options and stable object state.

- [ ] **Step 1: Add failing golden/property tests**

  Test literals, `##`, `#{name}`, nested `#{?condition,then,else}`, stable IDs,
  names, geometry, option lookup, missing variables, malformed offsets,
  32-level recursion, 1 MiB input/output, and arbitrary bounded bytes never
  panicking.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-core formats::tests -- --nocapture
  ```

  Expected: compilation fails for the missing format module.

- [ ] **Step 3: Implement one-pass parsing with bounded recursive expansion**

  Keep the public interface to:

  ```rust
  pub struct FormatContext { pub client: Option<ClientId>, pub session: Option<SessionId>, pub window: Option<WindowId>, pub pane: Option<PaneId> }
  pub struct FormatEngine;
  impl FormatEngine { pub fn expand(state: &ServerState, context: FormatContext, input: &str) -> Result<String, FormatError>; }
  ```

  Parse comma-separated condition branches while tracking nested `#{...}`
  depth. Never run jobs, read clocks, read environment variables, or perform
  filesystem I/O.

- [ ] **Step 4: Add `display-message` and verify**

  Parse `display-message|display [-t target] format`, resolve its context, and
  return expanded text as the command result. Run:

  ```powershell
  cargo test -p wmux-core
  cargo clippy -p wmux-core --all-targets -- -D warnings
  ```

- [ ] **Step 5: Commit**

  ```powershell
  git add wmux-clean/crates/wmux-core/src
  git commit -m "feat(formats): add deterministic expansion"
  ```

---

### Task 4: Execute functional config and `source-file` through one parser

**Files:**
- Modify: `wmux-clean/crates/wmux-config/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`

**Interfaces:**
- Produces: `WmuxConfig::command_source()`, `Command::SourceFile { path: PathBuf }`, `CommandEffect::SourceFile`, and `CommandQueue::insert_after_active`.
- Consumes: the shared lexer/parser and existing bootstrap config fields.

- [ ] **Step 1: Add failing config/continuation tests**

  Test legacy key/value compatibility, retention of command lines and source
  line numbers, whole-file parse before execution, command insertion before the
  next original command, 1 MiB files, include depth 16, cycle rejection, and
  path-qualified diagnostics.

- [ ] **Step 2: Verify RED**

  ```powershell
  cargo test -p wmux-config -p wmux-core -p wmux-server config -- --nocapture
  ```

  Expected: config command retention and source effects are absent.

- [ ] **Step 3: Extend config without a second command grammar**

  Store unrecognized nonempty lines as:

  ```rust
  pub struct ConfigCommandSource { pub text: String, pub first_line: usize }
  ```

  Keep recognized bootstrap `key = value` lines unchanged. In the server,
  concatenate/parse command text once with `parse_command_text` and enqueue it
  as `CommandSource::Config` on the reserved owner client ID.

- [ ] **Step 4: Implement bounded source insertion**

  `CommandEffect::SourceFile` carries the requested path. The owner resolves it
  relative to the including file/current directory, canonicalizes for cycle
  detection, reads with a 1 MiB cap, parses the entire file, then invokes:

  ```rust
  queue.insert_after_active(&queued, parsed_commands)?;
  ```

  No command from a syntactically invalid file may execute.

- [ ] **Step 5: Run full Windows checks and commit**

  ```powershell
  cargo fmt --all -- --check
  cargo test --workspace
  cargo clippy --workspace --all-targets -- -D warnings
  git add wmux-clean/crates/wmux-config wmux-clean/crates/wmux-core wmux-clean/crates/wmux-server
  git commit -m "feat(config): execute shared command sources"
  ```

