# Phase 4 Commands, Targets, and Key Tables Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move command parsing, target resolution, semantic key routing, key tables, and binding policy into server-owned core while upgrading IPC to version 6 and preserving a zero-extra-allocation unbound-key hot path.

**Architecture:** Split the current monolithic command module into a bounded lexer/parser and effect-returning executor, add one typed target resolver, and route normalized key events through compact server-owned tables. The client sends semantic key identity plus original bytes; the owner loop fairly schedules pre-parsed command lists and remains the only state mutator.

**Tech Stack:** Rust 2021 workspace, Tokio, crossterm, custom framed IPC, `smallvec`, deterministic release benchmark harness, libFuzzer compile targets.

**Spec:** `docs/superpowers/specs/2026-08-21-commands-targets-key-tables-design.md`

## Global Constraints

- Commit every completed task directly to `main`; do not create a phase branch or phase worktree.
- Do not stage, modify, or remove the untracked root `.agents/` directory.
- Do not use subagents; execute and review every task inline.
- Research conclusions recorded in the spec remain binding: tmux command/target/key semantics and zellij normalized-key-plus-raw-byte transport.
- Core, protocol, CLI policy, and shared server semantics must not import Windows or Unix APIs.
- The owner loop remains the sole mutator of mux state.
- No command parser or key router may perform I/O, spawn a task, acquire a lock, or resolve a native handle.
- No production behavior is written before its focused test has failed for the expected reason.
- Parser limits are exactly 1 MiB input, 4,096 tokens, 256 commands per list, and 64 KiB per token.
- Protocol constants become exactly version `6`, magic `WMX6`, and maximum payload `16,777,216` bytes.
- Paste stays semantic and bypasses key tables.
- UI/UX styling remains out of scope; confirmation is a plain client-owned overlay that never mutates pane cells.
- Every task ends with focused tests, `cargo test --workspace`, `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `git diff --check`, and the stated commit.

---

### Task 1: Split the Command Module and Add the Bounded Shared Parser

**Files:**
- Replace: `wmux-clean/crates/wmux-core/src/command.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/lexer.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/parser.rs`
- Create: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-cli/src/lib.rs`

**Interfaces:**
- Consumes: the current `Command`, `CommandQueue`, `parse_command`, `resolve_command_name`, and `execute` behavior.
- Produces: `SourcePosition`, `SourceSpan`, `CommandParseError`, immutable `CommandList`, `parse_command_text`, `parse_command_argv`, and the unchanged existing command variants behind the new module seam.

- [ ] **Step 1: Write failing lexer and parser interface tests**

Add these behaviors under `command::lexer::tests` and `command::parser::tests` before creating the implementations:

```rust
#[test]
fn quotes_escapes_empty_arguments_comments_and_chains_are_tokenized() {
    let tokens = lex("rename-window 'two words'; new-window \"\" # ignored\nlist-sessions")
        .unwrap();
    assert_eq!(
        token_values(&tokens),
        vec![
            "rename-window", "two words", ";", "new-window", "", ";",
            "list-sessions",
        ]
    );
}

#[test]
fn text_and_argv_share_command_resolution_without_losing_empty_arguments() {
    let text = parse_command_text("rename-window ''").unwrap();
    let argv = parse_command_argv(&["rename-window".into(), "".into()]).unwrap();
    assert_eq!(text, argv);
    assert_eq!(text.len(), 1);
}

#[test]
fn semicolon_chains_are_atomic_and_preserve_order() {
    let list = parse_command_text("new-window -n one; rename-window 'two words'").unwrap();
    assert_eq!(list.len(), 2);
    assert!(matches!(&list[0], Command::NewWindow { name: Some(name) } if name == "one"));
    assert!(matches!(&list[1], Command::RenameWindow { name } if name == "two words"));
}
```

Add table tests for `\e`, `\n`, `\s`, `\t`, `\u03bb`, `\U0001F642`, unmatched quotes, a dangling escape, an empty command between semicolons, exact aliases, unique prefixes, ambiguous prefixes, and unknown commands. Assert the unmatched-quote error begins at the opening quote's byte offset, line, and column.

- [ ] **Step 2: Run the new tests and verify RED**

Run:

```powershell
cargo test -p wmux-core command::lexer::tests -- --nocapture
cargo test -p wmux-core command::parser::tests -- --nocapture
```

Expected: compilation or assertion failure because `lex`, `CommandList`, `parse_command_text`, and `parse_command_argv` do not exist.

- [ ] **Step 3: Implement bounded lexical tokens and source spans**

Create these interfaces in `command/lexer.rs`:

```rust
pub const MAX_COMMAND_BYTES: usize = 1024 * 1024;
pub const MAX_TOKENS: usize = 4_096;
pub const MAX_TOKEN_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourcePosition {
    pub offset: usize,
    pub line: usize,
    pub column: usize,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct SourceSpan {
    pub start: SourcePosition,
    pub end: SourcePosition,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TokenKind {
    Word(String),
    Separator,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Token {
    pub kind: TokenKind,
    pub span: SourceSpan,
}

pub fn lex(input: &str) -> Result<Vec<Token>, CommandParseError>;
```

Use a single pass over UTF-8 `char_indices`, track quote state and positions, treat `#` as a comment only when it begins a token outside quotes, and reject every exact limit before growing the affected buffer.

- [ ] **Step 4: Implement the shared parser and preserve existing commands**

In `command/mod.rs`, define:

```rust
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandList(std::sync::Arc<[Command]>);

impl CommandList {
    pub fn new(commands: Vec<Command>) -> Result<Self, CommandParseError>;
    pub fn len(&self) -> usize;
    pub fn is_empty(&self) -> bool;
    pub fn iter(&self) -> std::slice::Iter<'_, Command>;
}

impl std::ops::Index<usize> for CommandList {
    type Output = Command;
    fn index(&self, index: usize) -> &Self::Output;
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CommandParseError {
    pub message: String,
    pub span: Option<SourceSpan>,
}

pub fn parse_command_text(input: &str) -> Result<CommandList, CommandParseError>;
pub fn parse_command_argv(argv: &[String]) -> Result<CommandList, CommandParseError>;
```

Move command-name lookup and argument validation to `parser.rs`, move the existing executor unchanged to `execute.rs`, and make both parser entry points call one private `parse_token_commands`. Keep a temporary `parse_command(argv) -> Result<Command, CommandParseError>` wrapper only while existing call sites migrate; it must reject lists whose length is not one.

Update `wmux-cli` to use the error's `Display` implementation and shared command metadata rather than tuple-field access. Do not join argv into a command string.

- [ ] **Step 5: Add exact parser-limit tests**

```rust
#[test]
fn parser_limits_reject_before_partial_execution() {
    let too_large = "x".repeat(MAX_COMMAND_BYTES + 1);
    assert_eq!(parse_command_text(&too_large).unwrap_err().message, "command input exceeds 1048576 bytes");

    let too_many = std::iter::repeat_n("list-sessions", MAX_COMMANDS + 1)
        .collect::<Vec<_>>()
        .join(";");
    assert_eq!(parse_command_text(&too_many).unwrap_err().message, "command list exceeds 256 commands");
}
```

Also generate 4,097 one-byte tokens and one 65,537-byte token; assert the exact token-count and token-size diagnostics.

- [ ] **Step 6: Run focused and workspace verification**

```powershell
cargo test -p wmux-core command:: -- --nocapture
cargo test -p wmux-cli -- --nocapture
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

Expected: all existing command behavior and all new lexer/parser tests pass.

- [ ] **Step 7: Commit Task 1 directly to main**

```powershell
git add wmux-clean/crates/wmux-core/src/command.rs wmux-clean/crates/wmux-core/src/command wmux-clean/crates/wmux-core/src/lib.rs wmux-clean/crates/wmux-cli/src/lib.rs
git commit -m "feat(core): add bounded shared command parser"
```

---

### Task 2: Add the Central Typed Target Resolver

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/target.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/parser.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`

**Interfaces:**
- Consumes: `ServerState`, stable IDs, client attachment state, `Session::previous_winlink`, and `Window::previous_pane`.
- Produces: parsed `TargetSpec`, typed `TargetKind`, `ResolveContext`, `ResolvedTarget`, `TargetError`, and `TargetResolver::resolve` used by every executor.

- [ ] **Step 1: Write failing target parsing and resolution tests**

Create tests through the public resolver seam:

```rust
#[test]
fn resolves_qualified_current_last_relative_and_stable_targets() {
    let (state, client, first, second) = target_fixture();
    let resolver = TargetResolver::new(&state);
    let context = ResolveContext::for_client(&state, client).unwrap();

    assert_eq!(resolver.resolve(&context, TargetKind::Pane, &TargetSpec::parse("{current}").unwrap()).unwrap().pane, Some(second.pane));
    assert_eq!(resolver.resolve(&context, TargetKind::Window, &TargetSpec::parse("-1").unwrap()).unwrap().winlink, Some(first.winlink));
    assert_eq!(resolver.resolve(&context, TargetKind::Pane, &TargetSpec::parse(&format!("%{}", first.pane.raw())).unwrap()).unwrap().pane, Some(first.pane));
    assert_eq!(resolver.resolve(&context, TargetKind::Pane, &TargetSpec::parse("work:0.0").unwrap()).unwrap().pane, Some(first.pane));
}

#[test]
fn ambiguity_and_absence_are_distinct() {
    let (state, client) = ambiguous_session_fixture(["work", "worker"]);
    let resolver = TargetResolver::new(&state);
    let context = ResolveContext::for_client(&state, client).unwrap();
    assert!(matches!(resolver.resolve(&context, TargetKind::Session, &TargetSpec::parse("wor").unwrap()), Err(TargetError::Ambiguous { .. })));
    assert!(matches!(resolver.resolve(&context, TargetKind::Pane, &TargetSpec::parse("missing:0.0").unwrap()), Err(TargetError::NotFound { .. })));
}
```

Add focused cases for client ID, `$session`, `@window`, `%pane`, exact-name precedence, unique prefix, numeric winlink index, `+N` wrap, `-N` wrap, `{last}` for session/winlink/pane, empty current target, invalid pane suffix on a session request, and a destroyed stable ID.

- [ ] **Step 2: Run target tests and verify RED**

```powershell
cargo test -p wmux-core target::tests -- --nocapture
```

Expected: compilation failure because `TargetSpec` and `TargetResolver` do not exist.

- [ ] **Step 3: Implement the resolver interface and client history**

Define in `target.rs`:

```rust
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TargetKind { Client, Session, Window, Pane }

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TargetSpec { raw: String }

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResolveContext {
    pub client: ClientId,
    pub current_session: Option<SessionId>,
    pub current_winlink: Option<WinlinkId>,
    pub current_window: Option<WindowId>,
    pub current_pane: Option<PaneId>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResolvedTarget {
    pub client: ClientId,
    pub session: Option<SessionId>,
    pub winlink: Option<WinlinkId>,
    pub window: Option<WindowId>,
    pub pane: Option<PaneId>,
}

pub struct TargetResolver<'a> { state: &'a ServerState }

impl<'a> TargetResolver<'a> {
    pub fn new(state: &'a ServerState) -> Self;
    pub fn resolve(&self, context: &ResolveContext, kind: TargetKind, spec: &TargetSpec) -> Result<ResolvedTarget, TargetError>;
}
```

Add `previous_session: Option<SessionId>` to core `Client`. Update it only when an attached client successfully changes sessions. Resolution must traverse session winlinks, never choose a global window without preserving the matching `WinlinkId`.

- [ ] **Step 4: Parse target arguments once and remove `find_session` execution lookups**

Change string target fields in `Command` to `Option<TargetSpec>` and translate next/previous/last selectors into target specs during parsing. Each affected executor must construct `ResolveContext::for_client`, call `TargetResolver`, and consume the returned stable IDs. Remove `ServerState::find_session` after `rg` proves no remaining command-specific lookup uses it.

- [ ] **Step 5: Run focused, state, and workspace verification**

```powershell
cargo test -p wmux-core target::tests -- --nocapture
cargo test -p wmux-core command:: -- --nocapture
cargo test -p wmux-core state::tests -- --nocapture
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
rg -n "find_session|sessions\.values\(\).*find" wmux-clean/crates/wmux-core/src/command wmux-clean/crates/wmux-server/src
git diff --check
```

Expected: target tests pass and the final `rg` prints no ad hoc target lookup in command/server code.

- [ ] **Step 6: Commit Task 2 directly to main**

```powershell
git add wmux-clean/crates/wmux-core/src/target.rs wmux-clean/crates/wmux-core/src/lib.rs wmux-clean/crates/wmux-core/src/state.rs wmux-clean/crates/wmux-core/src/command
git commit -m "feat(core): centralize command target resolution"
```

---

### Task 3: Upgrade Protocol v6 with Semantic Key Events

**Files:**
- Modify: `wmux-clean/crates/wmux-protocol/src/lib.rs`
- Modify: `wmux-clean/docs/ipc-protocol.md`
- Modify: `wmux-clean/docs/hybrid-rendering.md`
- Modify: `wmux-clean/fuzz/fuzz_targets/protocol_frame.rs`
- Create: `wmux-clean/fuzz/corpus/protocol_frame/key-v6`

**Interfaces:**
- Consumes: protocol v5 `Message::Key(Vec<u8>)` and existing zero-reallocation owned payload encoding.
- Produces: v6 `WireKeyCode`, `WireKeyModifiers`, `WireKeyEvent`, `Message::Key(WireKeyEvent)`, magic `WMX6`, and strict decoder validation.

- [ ] **Step 1: Write failing protocol round-trip and rejection tests**

```rust
#[test]
fn semantic_key_roundtrip_preserves_identity_and_raw_bytes() {
    let message = Message::Key(WireKeyEvent {
        code: WireKeyCode::Char('λ'),
        modifiers: WireKeyModifiers::ALT | WireKeyModifiers::CONTROL,
        raw: "λ".as_bytes().to_vec(),
    });
    assert_eq!(decode_message(&encode_frame(&message)).unwrap(), message);
}

#[test]
fn invalid_semantic_keys_are_rejected() {
    assert!(decode_key_payload(&payload_with_scalar(0x11_0000)).is_err());
    assert!(decode_key_payload(&payload_with_modifiers(0x80)).is_err());
    assert!(decode_key_payload(&payload_with_unknown_tag(0xff)).is_err());
}
```

Update the documentation drift test to expect version 6 and `WMX6`; run it before modifying constants.

- [ ] **Step 2: Run protocol tests and verify RED**

```powershell
cargo test -p wmux-protocol -- --nocapture
```

Expected: failures because wire key types do not exist and the documentation still records v5.

- [ ] **Step 3: Implement the compact key payload**

Define protocol-owned wire types without depending on `wmux-core`:

```rust
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WireKeyCode {
    Char(char), Left, Right, Up, Down, Home, End, PageUp, PageDown,
    Backspace, Delete, Insert, Enter, Tab, BackTab, Escape, Function(u8),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct WireKeyModifiers(u8);

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct WireKeyEvent {
    pub code: WireKeyCode,
    pub modifiers: WireKeyModifiers,
    pub raw: Vec<u8>,
}
```

Encode a one-byte tag, one-byte modifiers, four-byte little-endian key value, then raw bytes. Validate function keys are 1 through 24, modifier bits are known, character scalars satisfy `char::from_u32`, and fixed-key values are zero. Move `raw` into `EncodedPayload::Owned` once; do not clone it.

Set:

```rust
pub const VERSION: u32 = 6;
pub const MAGIC: [u8; 4] = *b"WMX6";
pub const MAX_FRAME: usize = 16 * 1024 * 1024;
```

- [ ] **Step 4: Update protocol documentation, fuzz seed, and corpus decoder path**

Record the exact payload layout in `docs/ipc-protocol.md`, update every version mention in `hybrid-rendering.md`, extend `protocol_frame` to decode v6 key frames, and add a checked-in `key-v6` seed produced by `encode_frame` in the protocol test helper.

- [ ] **Step 5: Run protocol, fuzz-build, and workspace verification**

```powershell
cargo test -p wmux-protocol -- --nocapture
cargo check --manifest-path fuzz/Cargo.toml --bins
cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

- [ ] **Step 6: Commit Task 3 directly to main**

```powershell
git add wmux-clean/crates/wmux-protocol/src/lib.rs wmux-clean/docs/ipc-protocol.md wmux-clean/docs/hybrid-rendering.md wmux-clean/fuzz
git commit -m "feat(protocol): carry semantic keys in version 6"
```

---

### Task 4: Add Packed Keys, Compact Tables, and the Pure Core Router

**Files:**
- Create: `wmux-clean/crates/wmux-core/src/keys.rs`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-core/src/event.rs`

**Interfaces:**
- Consumes: `CommandList`, core `Client`, semantic paste, and existing copy-mode state indication.
- Produces: `BareKey`, packed `KeyModifiers`, `KeyCode`, `KeyEvent`, `KeyBinding`, `KeyTable`, `KeyTables`, `InputMode`, `InputRoute`, and `route_key`.

- [ ] **Step 1: Write failing key normalization, table, and routing tests**

```rust
#[test]
fn uppercase_characters_normalize_to_lowercase_plus_shift() {
    assert_eq!(KeyCode::character('A', KeyModifiers::NONE), KeyCode::character('a', KeyModifiers::SHIFT));
}

#[test]
fn prefix_state_is_per_client_and_unbound_bytes_move_unchanged() {
    let (mut state, first, second) = attached_clients_fixture();
    let prefix = KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]);
    assert_eq!(route_key(&mut state, first, InputMode::Normal, prefix, 100), InputRoute::Consumed);
    assert_eq!(state.clients[&first].key_table, KeyTableName::PREFIX);
    assert_eq!(state.clients[&second].key_table, KeyTableName::ROOT);

    let raw = vec![0xf0, 0x9f, 0x99, 0x82];
    let route = route_key(&mut state, second, InputMode::Normal, KeyEvent::new(KeyCode::character('🙂', KeyModifiers::NONE), raw), 100);
    assert_eq!(route, InputRoute::PaneBytes(vec![0xf0, 0x9f, 0x99, 0x82]));
}

#[test]
fn repeatable_binding_keeps_prefix_and_nonrepeatable_key_retries_root() {
    let (mut state, client) = key_fixture();
    enter_prefix(&mut state, client, 100);
    assert!(matches!(route_named(&mut state, client, "resize-right", 110), InputRoute::Commands(_)));
    assert_eq!(state.clients[&client].repeat_deadline_ms, Some(610));
    assert!(matches!(route_named(&mut state, client, "literal-a", 120), InputRoute::PaneBytes(_)));
    assert_eq!(state.clients[&client].key_table, KeyTableName::ROOT);
}
```

Add tests for sorted lookup order, bind replacement, unbind, root binding precedence, copy-mode table precedence, missing copy binding returning `CopyModeKey`, expired repeat, `send-prefix` binding, paste not entering `route_key`, confirmation yes/no/Escape, and client removal dropping all transient input state.

- [ ] **Step 2: Run key tests and verify RED**

```powershell
cargo test -p wmux-core keys::tests -- --nocapture
```

Expected: compilation failure because the key interfaces do not exist.

- [ ] **Step 3: Implement packed keys and sorted tables**

Use a `u64` packed representation for `KeyCode`; reserve the low bits for the bare-key tag/value and the high four bits for modifiers. Expose constructors and parsing/display functions, but keep the packed field private.

```rust
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyBinding {
    pub key: KeyCode,
    pub repeatable: bool,
    pub commands: CommandList,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyTable {
    name: KeyTableName,
    bindings: Vec<KeyBinding>,
}

impl KeyTable {
    pub fn get(&self, key: KeyCode) -> Option<&KeyBinding>;
    pub fn bind(&mut self, binding: KeyBinding);
    pub fn unbind(&mut self, key: KeyCode) -> bool;
    pub fn clear(&mut self);
}
```

Implement lookup with `binary_search_by_key`; implement replacement/insertion at the returned index. `KeyTables::tmux_defaults()` must parse default binding command lists once during `ServerState::new` and fail fast only on compile-time default mistakes.

- [ ] **Step 4: Implement client state and the pure router**

Add to `Client`:

```rust
pub key_table: KeyTableName,
pub prefix_deadline_ms: Option<u64>,
pub repeat_deadline_ms: Option<u64>,
pub last_repeatable_key: Option<KeyCode>,
pub confirmation: Option<ConfirmationState>,
```

Add `pub key_tables: KeyTables` to `ServerState` and initialize it with
`KeyTables::tmux_defaults()` in `ServerState::new`.

Define:

```rust
pub enum InputMode { Normal, CopyMode }

pub enum InputRoute {
    PaneBytes(Vec<u8>),
    Commands(CommandList),
    CopyModeKey(KeyEvent),
    Consumed,
}

pub fn route_key(
    state: &mut ServerState,
    client: ClientId,
    mode: InputMode,
    event: KeyEvent,
    now_ms: u64,
) -> InputRoute;
```

Follow the exact precedence and 500 ms repeat semantics from the spec. Avoid cloning `event.raw`; move it only into `PaneBytes` or `CopyModeKey`.

- [ ] **Step 5: Run focused and workspace verification**

```powershell
cargo test -p wmux-core keys::tests -- --nocapture
cargo test -p wmux-core event::tests -- --nocapture
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

- [ ] **Step 6: Commit Task 4 directly to main**

```powershell
git add wmux-clean/crates/wmux-core/src/keys.rs wmux-clean/crates/wmux-core/src/lib.rs wmux-clean/crates/wmux-core/src/state.rs wmux-clean/crates/wmux-core/src/event.rs
git commit -m "feat(core): add server-owned key tables"
```

---

### Task 5: Normalize Windows Keys and Remove Client Binding Policy

**Files:**
- Modify: `wmux-clean/crates/wmux-windows/src/console.rs`
- Modify: `wmux-clean/crates/wmux-client/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-client/src/main.rs`

**Interfaces:**
- Consumes: crossterm `KeyEvent`, v6 `WireKeyEvent`, and core key normalization rules.
- Produces: `ConsoleInput::Key(WireKeyEvent)` and a client attach loop with no prefix state or binding translation.

- [ ] **Step 1: Write failing Windows normalization tests**

```rust
#[test]
fn normalized_key_keeps_semantic_identity_and_application_bytes() {
    assert_eq!(
        encode_key_event(key(KeyCode::Char('B'), KeyModifiers::CONTROL)).unwrap(),
        WireKeyEvent {
            code: WireKeyCode::Char('b'),
            modifiers: WireKeyModifiers::CONTROL,
            raw: vec![0x02],
        }
    );
}

#[test]
fn multibyte_and_navigation_keys_are_one_semantic_event() {
    let unicode = encode_key_event(key(KeyCode::Char('λ'), KeyModifiers::ALT)).unwrap();
    assert_eq!(unicode.code, WireKeyCode::Char('λ'));
    assert_eq!(unicode.raw, [0x1b, 0xce, 0xbb]);
    let up = encode_key_event(key(KeyCode::Up, KeyModifiers::NONE)).unwrap();
    assert_eq!(up.code, WireKeyCode::Up);
    assert_eq!(up.raw, b"\x1b[A");
}
```

Add cases for Shift normalization, Super, BackTab, F1/F12, Ctrl-Space, Ctrl-Backspace, Enter, Escape, and the existing clipboard-paste shortcut.

- [ ] **Step 2: Run Windows/client tests and verify RED**

```powershell
cargo test -p wmux-windows console::tests -- --nocapture
cargo test -p wmux -- --nocapture
```

Expected: type failures because console keys are still byte vectors.

- [ ] **Step 3: Implement the Windows wire-key adapter**

Change `ConsoleInput::Key(Vec<u8>)` to `ConsoleInput::Key(WireKeyEvent)`. Reuse the current byte encoder to produce `raw`, map crossterm key codes to wire tags, normalize ASCII uppercase into lowercase plus Shift, and preserve every original modifier supported by the wire type.

- [ ] **Step 4: Delete all mux policy from the client attach loop**

Remove `prefix`, `handle_key`, and `prefix_command_sequence`. The attach loop becomes:

```rust
ConsoleInput::Key(event) => {
    send_async_message(writer, Message::Key(event)).await?;
}
ConsoleInput::Paste(text) => {
    send_paste(writer, paste_bytes(&text)).await?;
}
```

Delete the client prefix translation tests and replace them with an assertion that the client source contains no `prefix_command_sequence` or `Ctrl-b` command mapping. Do not add `wmux-core` as a client dependency; the client uses protocol wire types only.

- [ ] **Step 5: Run focused, policy-audit, and workspace verification**

```powershell
cargo test -p wmux-windows console::tests -- --nocapture
cargo test -p wmux -- --nocapture
rg -n "prefix_command_sequence|let mut prefix|select-window -t|select-pane -[LRUD]" wmux-clean/crates/wmux-client/src
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

Expected: tests pass and `rg` prints no client-side mux binding policy.

- [ ] **Step 6: Commit Task 5 directly to main**

```powershell
git add wmux-clean/crates/wmux-windows/src/console.rs wmux-clean/crates/wmux-client/Cargo.toml wmux-clean/crates/wmux-client/src/main.rs
git commit -m "feat(client): send normalized keys without mux policy"
```

---

### Task 6: Make Command Lists Fair and Execution Effect-Driven

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`

**Interfaces:**
- Consumes: current immediate push/pop queue, `CommandList`, `ResolveContext`, existing server special cases, and platform-pane adapters.
- Produces: fair per-client `CommandQueue`, `CommandSource`, `QueuedCommand`, `CommandCompletion`, `CommandOutcome`, and ordered `CommandEffect` application.

- [ ] **Step 1: Write failing queue fairness and isolation tests**

```rust
#[test]
fn command_lists_are_round_robin_across_clients_and_ordered_within_each_client() {
    let mut queue = CommandQueue::default();
    let a = ClientId::new(1);
    let b = ClientId::new(2);
    queue.push_list(a, list(&["new-window -n a1", "new-window -n a2"]), CommandSource::ClientRequest).unwrap();
    queue.push_list(b, list(&["new-window -n b1", "new-window -n b2"]), CommandSource::ClientRequest).unwrap();
    assert_eq!(pop_names(&mut queue), ["a1", "b1", "a2", "b2"]);
}

#[test]
fn one_failed_list_does_not_remove_another_clients_work() {
    let mut queue = two_client_queue();
    let failed = queue.pop().unwrap();
    queue.finish(failed, Err("no target".into()));
    assert_eq!(queue.pop().unwrap().client, ClientId::new(2));
}
```

Add tests for monotonic sequence IDs, list length enforcement, binding-origin lists requiring no reply, client disconnect dropping only that client's pending lists, and one command popped per client turn.

- [ ] **Step 2: Run queue/server tests and verify RED**

```powershell
cargo test -p wmux-core command::tests::command_lists_are_round_robin -- --nocapture
cargo test -p wmux-server command_queue -- --nocapture
```

Expected: failures because the queue handles only one immediately popped command.

- [ ] **Step 3: Implement the fair queue interface**

```rust
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum CommandSource { ClientRequest, KeyBinding }

pub struct QueuedCommand {
    pub invocation: u64,
    pub sequence: u64,
    pub client: ClientId,
    pub command: Command,
    pub source: CommandSource,
    pub final_in_list: bool,
}

impl CommandQueue {
    pub fn push_list(&mut self, client: ClientId, list: CommandList, source: CommandSource) -> Result<u64, CommandParseError>;
    pub fn pop(&mut self) -> Option<QueuedCommand>;
    pub fn finish(&mut self, command: QueuedCommand, result: Result<String, String>) -> Option<CommandCompletion>;
    pub fn remove_client(&mut self, client: ClientId) -> Vec<u64>;
}
```

Store a `BTreeMap<ClientId, VecDeque<PendingInvocation>>` plus a `VecDeque<ClientId>` ready ring. Requeue a client only after yielding one command. On failure, discard only the rest of that invocation. Aggregate nonempty command messages with a single newline and emit completion only at list end or error.

- [ ] **Step 4: Replace special-case execution with semantic effects**

Define:

```rust
pub struct CommandOutcome {
    pub ok: bool,
    pub message: String,
    pub effects: smallvec::SmallVec<[CommandEffect; 2]>,
}

pub enum CommandEffect {
    EnsurePane { pane: PaneId },
    PaneInput { pane: PaneId, bytes: Vec<u8> },
    EnterCopyMode { client: ClientId },
    RefreshClient { client: ClientId },
    Confirm { client: ClientId, prompt: String, commands: CommandList },
    DetachClient { client: ClientId },
    Shutdown { requester: ClientId },
}
```

Make `execute(&mut ServerState, &QueuedCommand) -> CommandOutcome` mutate only core state. Move copy-mode, detach, refresh, shutdown, pane-spawn, and pane-input mechanics into one `ServerOwner::apply_command_effect` match. Apply effects before calling `queue.finish` and before the next command from that invocation.

- [ ] **Step 5: Drive the queue from the owner loop with a fixed budget**

Add `COMMANDS_PER_TURN: usize = 64`. IPC and key events enqueue lists. Each owner-loop turn pops and executes at most 64 commands, preserving opportunities for PTY output, timers, and control events between budgets. Send one response from `CommandCompletion` for client requests; binding lists are silent unless their command explicitly reports an error.

- [ ] **Step 6: Run focused, lifecycle, and workspace verification**

```powershell
cargo test -p wmux-core command:: -- --nocapture
cargo test -p wmux-server -- --nocapture
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

- [ ] **Step 7: Commit Task 6 directly to main**

```powershell
git add wmux-clean/crates/wmux-core/src/command wmux-clean/crates/wmux-core/src/state.rs wmux-clean/crates/wmux-server/src/lib.rs
git commit -m "refactor(server): queue command lists fairly"
```

---

### Task 7: Route Semantic Keys in the Owner Loop

**Files:**
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-core/src/event.rs`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`

**Interfaces:**
- Consumes: `Message::Key(WireKeyEvent)`, `KeyEvent`, `route_key`, fair command queue, and current copy-mode handler.
- Produces: one server input path that routes keys, enqueues bindings, forwards raw bytes, and preserves paste/mouse precedence.

- [ ] **Step 1: Write failing owner-loop routing tests**

```rust
#[test]
fn ctrl_b_then_c_is_executed_by_server_key_tables() {
    let mut owner = attached_owner();
    owner.handle_wire_key(client(), wire_ctrl('b', vec![0x02])).unwrap();
    owner.handle_wire_key(client(), wire_char('c', b"c".to_vec())).unwrap();
    owner.drain_command_budget().unwrap();
    assert_eq!(owner.runtime.state.sessions.len(), 1);
    assert_eq!(owner.runtime.state.windows.len(), 2);
    assert!(owner.platform_input(client()).is_empty());
}

#[test]
fn unbound_key_and_paste_reach_the_pane_once_without_binding_policy() {
    let mut owner = attached_owner();
    owner.handle_wire_key(client(), wire_char('λ', "λ".as_bytes().to_vec())).unwrap();
    owner.handle_event(ServerEvent::ClientInput { client: client(), input: ClientInput::Paste(b"abc".to_vec()) }).unwrap();
    assert_eq!(owner.take_platform_input(), ["λ".as_bytes(), b"abc"]);
}
```

Add tests for two clients with independent prefix state, copy-mode fallback, repeatable resize bindings, expired repeat, input scheduler timestamps, mouse precedence, and disconnect removing pending key commands.

- [ ] **Step 2: Run server routing tests and verify RED**

```powershell
cargo test -p wmux-server server_owned_key -- --nocapture
```

Expected: failures because wire keys still become raw `ClientInput::Bytes`.

- [ ] **Step 3: Add explicit key events to the core owner-event contract**

Change `ServerEvent` to include:

```rust
ClientKey {
    client: ClientId,
    event: KeyEvent,
},
```

Keep `ClientInput` for raw/paste effects only. Convert `WireKeyEvent` to core `KeyEvent` in the server protocol adapter and reject conversion errors as `InvalidCommand`-class client errors without stopping the daemon.

- [ ] **Step 4: Integrate `route_key` without cloning bytes**

Supply monotonic milliseconds from a server-owner start `Instant`. Route using `InputMode::CopyMode` when the client view has an active copy mode. Match `InputRoute` exactly once: write moved bytes, enqueue the shared command list as `KeyBinding`, call existing copy handler with moved raw bytes, or consume.

- [ ] **Step 5: Extend portable conformance for semantic input**

Add deterministic cases proving Ctrl-b prefix handling, unbound UTF-8 passthrough, two-client prefix isolation, bracketed paste bypass, and binding-origin queue order. Include these results in the aggregate conformance fingerprint.

- [ ] **Step 6: Run focused, conformance, and workspace verification**

```powershell
cargo test -p wmux-server server_owned_key -- --nocapture
cargo test -p wmux-conformance -- --nocapture
cargo run -p wmux-conformance --release
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

- [ ] **Step 7: Commit Task 7 directly to main**

```powershell
git add wmux-clean/crates/wmux-server/src/lib.rs wmux-clean/crates/wmux-core/src/event.rs wmux-clean/crates/wmux-conformance/src/lib.rs
git commit -m "feat(server): route keys through server-owned tables"
```

---

### Task 8: Implement Binding Mutation and Listing Commands

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/parser.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Modify: `wmux-clean/crates/wmux-core/src/keys.rs`
- Modify: `wmux-clean/crates/wmux-cli/src/lib.rs`

**Interfaces:**
- Consumes: parsed nested `CommandList`, mutable `ServerState::key_tables`, `KeyCode::parse`, and deterministic table iteration.
- Produces: `bind-key`, `unbind-key`, and `list-keys` through the serialized queue.

- [ ] **Step 1: Write failing parse and execution tests**

```rust
#[test]
fn bind_unbind_and_list_keys_use_parsed_command_lists() {
    let bind = parse_command_text("bind-key -r -T prefix C-j 'select-pane -D'").unwrap();
    assert!(matches!(&bind[0], Command::BindKey { table, key, repeatable: true, commands } if table.as_str() == "prefix" && *key == KeyCode::ctrl('j') && commands.len() == 1));

    let mut state = ServerState::new();
    execute_list(&mut state, client(), bind).unwrap();
    let listed = execute_text(&mut state, client(), "list-keys -T prefix").unwrap();
    assert!(listed.contains("bind-key -r -T prefix C-j select-pane -D"));
    execute_text(&mut state, client(), "unbind-key -T prefix C-j").unwrap();
    assert!(!execute_text(&mut state, client(), "list-keys -T prefix").unwrap().contains("C-j"));
}
```

Add tests for alias `bind`, default prefix table, `-n` root, explicit copy-mode table, replacement without duplicate entries, `unbind -a` clearing only the selected/default table, unknown keys, invalid table names, empty nested commands, deterministic table/key order, and parseable `list-keys` output.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
cargo test -p wmux-core command::tests::bind -- --nocapture
cargo test -p wmux-core keys::tests::bind -- --nocapture
```

Expected: unknown command errors for the new surface.

- [ ] **Step 3: Add command variants and exact argument validation**

```rust
BindKey {
    table: KeyTableName,
    key: KeyCode,
    repeatable: bool,
    commands: CommandList,
},
UnbindKey {
    table: KeyTableName,
    key: Option<KeyCode>,
    all: bool,
},
ListKeys {
    table: Option<KeyTableName>,
},
```

Parse `bind-key [-nr] [-T table] key command [argument ...]`, `unbind-key [-an] [-T table] [key]`, and `list-keys [-T table]`. Enforce mutual exclusion of `-n` and `-T`; require `-a` when unbind has no key; reject a key when `-a` is present.

- [ ] **Step 4: Execute mutation and deterministic listing through state**

Mutate only `state.key_tables`. Format list output with canonical key names and `quote_argument` from the command parser, ordered by `KeyTableName` then packed `KeyCode`. The output of each line must parse back into an equivalent `BindKey` command.

- [ ] **Step 5: Run focused, CLI, and workspace verification**

```powershell
cargo test -p wmux-core command:: -- --nocapture
cargo test -p wmux-core keys:: -- --nocapture
cargo test -p wmux-cli -- --nocapture
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

- [ ] **Step 6: Commit Task 8 directly to main**

```powershell
git add wmux-clean/crates/wmux-core/src/command wmux-clean/crates/wmux-core/src/keys.rs wmux-clean/crates/wmux-cli/src/lib.rs
git commit -m "feat(core): add key binding commands"
```

---

### Task 9: Implement Send, Switch, Refresh, and Confirmation Commands

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/command/mod.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/parser.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/execute.rs`
- Modify: `wmux-clean/crates/wmux-core/src/keys.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-server/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-cli/src/lib.rs`

**Interfaces:**
- Consumes: `TargetResolver`, `CommandEffect`, key encoding, per-client confirmation state, and client-scoped render baselines.
- Produces: `send-keys`, `send-prefix`, `switch-client`, `refresh-client`, `confirm-before`, and confirmed destructive defaults.

- [ ] **Step 1: Write failing command behavior tests**

```rust
#[test]
fn send_keys_and_send_prefix_emit_exact_targeted_bytes() {
    let mut state = attached_state();
    let keys = execute_text(&mut state, client(), "send-keys -t :0.0 -l 'λ x'").unwrap();
    assert_eq!(keys.effects, [CommandEffect::PaneInput { pane: pane(), bytes: "λ x".as_bytes().to_vec() }]);
    let prefix = execute_text(&mut state, client(), "send-prefix -t :0.0").unwrap();
    assert_eq!(prefix.effects, [CommandEffect::PaneInput { pane: pane(), bytes: vec![0x02] }]);
}

#[test]
fn confirmation_enqueues_only_after_affirmative_input() {
    let mut state = attached_state();
    execute_text(&mut state, client(), "confirm-before -p 'kill pane? (y/n)' kill-pane").unwrap();
    assert!(state.clients[&client()].confirmation.is_some());
    assert_eq!(route_char(&mut state, client(), 'n'), InputRoute::Consumed);
    assert!(state.panes.contains_key(&pane()));
    execute_text(&mut state, client(), "confirm-before kill-pane").unwrap();
    assert!(matches!(route_char(&mut state, client(), 'y'), InputRoute::Commands(_)));
}
```

Add tests for named keys, `-l`, bounded `-N` from 1 through 10,000, invalid repeat counts, last/next/previous/exact session switching, `previous_session`, refresh invalidating only one client's baseline, Enter accepting confirmation, Escape rejecting it, destructive default bindings containing `ConfirmBefore`, and prompt clipping without pane-cell mutation.

- [ ] **Step 2: Run focused tests and verify RED**

```powershell
cargo test -p wmux-core command::tests::send -- --nocapture
cargo test -p wmux-core command::tests::confirmation -- --nocapture
cargo test -p wmux-server confirmation -- --nocapture
```

Expected: unknown command errors and missing confirmation overlay behavior.

- [ ] **Step 3: Add and parse the command variants**

```rust
SendKeys { target: Option<TargetSpec>, keys: Vec<SendKey>, literal: bool, repeat: u16 },
SendPrefix { target: Option<TargetSpec> },
SwitchClient { target: SessionSelector },
RefreshClient,
ConfirmBefore { prompt: String, commands: CommandList },
```

Use exact aliases `send` and `confirm`. Cap send repeat at 10,000, total encoded bytes at 1 MiB, and prompt UTF-8 length at 4 KiB. Parse named keys through `KeyCode::parse`; `-l` concatenates every remaining argument in argv order without inserting bytes, matching tmux. Users preserve spaces by quoting them into an individual argument.

- [ ] **Step 4: Implement effects and state transitions**

Resolve pane/session targets through `TargetResolver`. Encode named keys with the core static application-byte mapping and keep literal text unchanged. `SwitchClient` must call the same state attach path that records `previous_session`. `RefreshClient` emits only its client effect. `ConfirmBefore` stores the prompt and shared command list in core client state.

- [ ] **Step 5: Compose the functional confirmation overlay**

Extend the existing per-client viewport override path used for copy prompts. Clone only the visible last scene line, replace it with default-style prompt cells, hide the pane cursor, and keep the authoritative pane grid unchanged. A confirmation change requests an immediate full client render; Task 10 owns later styling.

- [ ] **Step 6: Replace destructive defaults and test live command flow**

Compile default prefix bindings as:

```text
bind-key -T prefix x confirm-before -p 'kill-pane? (y/n)' kill-pane
bind-key -T prefix & confirm-before -p 'kill-window? (y/n)' kill-window
```

Use protocol-level server tests to press Ctrl-b x, observe the prompt scene, reject once, accept once, and verify only the accepted path destroys the pane.

- [ ] **Step 7: Run focused and workspace verification**

```powershell
cargo test -p wmux-core command:: -- --nocapture
cargo test -p wmux-core keys:: -- --nocapture
cargo test -p wmux-server -- --nocapture
cargo test -p wmux-cli -- --nocapture
cargo test --workspace
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
git diff --check
```

- [ ] **Step 8: Commit Task 9 directly to main**

```powershell
git add wmux-clean/crates/wmux-core/src/command wmux-clean/crates/wmux-core/src/keys.rs wmux-clean/crates/wmux-core/src/state.rs wmux-clean/crates/wmux-server/src/lib.rs wmux-clean/crates/wmux-cli/src/lib.rs
git commit -m "feat(core): complete phase 4 command surface"
```

---

### Task 10: Add Malformed-Input, Performance, Conformance, and Exit Evidence

**Files:**
- Create: `wmux-clean/fuzz/fuzz_targets/command_text.rs`
- Create: `wmux-clean/fuzz/corpus/command_text/quoted-chain`
- Create: `wmux-clean/fuzz/corpus/command_text/malformed-escape`
- Modify: `wmux-clean/fuzz/Cargo.toml`
- Modify: `wmux-clean/fuzz/README.md`
- Modify: `wmux-clean/crates/wmux-core/src/command/lexer.rs`
- Modify: `wmux-clean/crates/wmux-core/src/command/parser.rs`
- Modify: `wmux-clean/crates/wmux-core/src/keys.rs`
- Modify: `wmux-clean/crates/wmux-protocol/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-bench/src/main.rs`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`
- Modify: `wmux-clean/docs/cross-os-conformance.md`
- Create: `wmux-clean/docs/command-key-model.md`

**Interfaces:**
- Consumes: complete Phase 4 parser, resolver, key router, queue, v6 protocol, and existing benchmark/conformance gates.
- Produces: deterministic malformed-input coverage, command fuzz target, four routing/parser benchmark workloads, exact performance thresholds, updated conformance fingerprint, and Phase 4 verification record.

- [ ] **Step 1: Read the test-quality rules before adding property tests**

Read completely:

```text
C:\Users\shres\.codex\plugins\cache\openai-curated-remote\superpowers\6.3.0\skills\test-driven-development\writing-good-tests.md
```

Name the production behavior each new test would catch before writing it.

- [ ] **Step 2: Write failing deterministic malformed-input property tests**

Use a fixed xorshift generator so tests require no random dependency:

```rust
#[test]
fn arbitrary_bounded_command_bytes_never_panic_or_exceed_limits() {
    let mut seed = 0x6d75_782d_7068_6173_u64;
    for len in 0..=4096 {
        let mut bytes = Vec::with_capacity(len);
        for _ in 0..len {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            bytes.push(seed as u8);
        }
        let text = String::from_utf8_lossy(&bytes);
        let result = std::panic::catch_unwind(|| parse_command_text(&text));
        assert!(result.is_ok(), "parser panicked for length {len}");
    }
}
```

Add equivalent bounded loops for `TargetSpec::parse`, `KeyCode::parse`, and v6 key payload decode. Assert errors have messages under 4 KiB and no invalid input mutates `ServerState` or `KeyTables`.

- [ ] **Step 3: Run property tests and verify RED where limits are incomplete**

```powershell
cargo test -p wmux-core arbitrary_bounded -- --nocapture
cargo test -p wmux-protocol invalid_semantic -- --nocapture
```

Expected: at least one focused assertion fails until all error/limit paths are bounded; if all pass immediately, add a mutation-snapshot assertion proving an invalid two-command list executes neither command and rerun to obtain the required RED.

- [ ] **Step 4: Implement missing bounds and add the command fuzz target**

The fuzz target must cap input to 1 MiB, use `String::from_utf8_lossy`, call `parse_command_text`, and, on success, iterate every parsed command without executing it. Register it as `command_text` in `fuzz/Cargo.toml`; document 60-second smoke and extended commands in `fuzz/README.md`.

- [ ] **Step 5: Write failing benchmark-registration and allocation tests**

```rust
#[test]
fn phase_4_hot_path_workloads_are_required() {
    for required in ["key-unbound", "key-prefix-binding", "command-queue", "command-text"] {
        assert!(SCENARIOS.contains(&required), "missing {required}");
    }
}
```

In the workload, recycle the `Vec<u8>` returned by `InputRoute::PaneBytes` into the next `KeyEvent` so setup allocates once. Assert the measured allocation delta is zero for one million routed unbound keys and one million prefix-binding dispatches.

- [ ] **Step 6: Implement the four release workloads and exact gates**

Full-suite operations:

```text
key-unbound:        10,000,000 routes, >= 15,000,000 operations/second, 0 measured allocations
key-prefix-binding: 5,000,000 prefix+binding pairs, >= 5,000,000 pairs/second, 0 measured allocations
command-queue:      1,000,000 pre-parsed commands across 8 clients, >= 2,000,000 commands/second
command-text:       250,000 four-command lists, >= 200,000 lists/second
```

Each workload must checksum routed bytes, command variants, client order, and final queue depth so the optimizer cannot remove work. Add all four scenarios to `SCENARIOS`, invocation, required-gate checks, JSON/human reporting, and unit registration tests.

- [ ] **Step 7: Run focused release measurements and optimize before lowering any gate**

```powershell
cargo run -p wmux-bench --release -- --suite full --scenario key-unbound
cargo run -p wmux-bench --release -- --suite full --scenario key-prefix-binding
cargo run -p wmux-bench --release -- --suite full --scenario command-queue
cargo run -p wmux-bench --release -- --suite full --scenario command-text
```

Expected: every stated threshold passes. If one fails, profile allocations, clones, lookup layout, or queue rotation and optimize the implementation. Do not lower a threshold without explicit user approval.

- [ ] **Step 8: Record reference comparison honestly**

Run equivalent existing key-routing microbenchmarks from the local tmux and zellij trees if either repository exposes one without writing reference code. Record command, revision, workload, and result in `docs/command-key-model.md`. If no equivalent benchmark exists, record that exact fact and restrict performance claims to wmux's measured thresholds and architectural comparison.

- [ ] **Step 9: Extend conformance and documentation**

Document grammar, target syntax, key names, default tables, queue fairness, protocol v6, confirmation behavior, and benchmark evidence in `command-key-model.md`. Extend conformance with parser, target, prefix, repeat, binding mutation, send-keys, switch-client, and confirmation cases. Record the new fingerprint and Phase 4 test count in `cross-os-conformance.md`.

- [ ] **Step 10: Run the complete Phase 4 exit gate**

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo check --manifest-path fuzz/Cargo.toml --bins
cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings
cargo run -p wmux-conformance --release
cargo run -p wmux-bench --release -- --suite full --gate
git diff --check
rg -n "std::os::windows|windows_sys|std::os::unix|libc::" wmux-clean/crates/wmux-core wmux-clean/crates/wmux-protocol wmux-clean/crates/wmux-cli
rg -n "prefix_command_sequence|let mut prefix" wmux-clean/crates/wmux-client/src
```

Expected: all commands pass; both final audits print no forbidden native imports or client binding policy.

- [ ] **Step 11: Update roadmap evidence and commit Task 10 directly to main**

Mark Task 4 checkboxes complete in `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md` only after the exit gate passes. Record exact commands, test count, conformance fingerprint, benchmark results, and clean tracked status in `docs/command-key-model.md`.

```powershell
git add docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md wmux-clean/fuzz wmux-clean/crates/wmux-core/src/command wmux-clean/crates/wmux-core/src/keys.rs wmux-clean/crates/wmux-protocol/src/lib.rs wmux-clean/crates/wmux-bench/src/main.rs wmux-clean/crates/wmux-conformance/src/lib.rs wmux-clean/docs/command-key-model.md wmux-clean/docs/cross-os-conformance.md
git commit -m "docs: record phase 4 command architecture evidence"
git status --short
```

Expected tracked status: clean. The pre-existing untracked root `.agents/` directory may remain and must be reported separately.

## Phase 4 Exit Gate

Phase 4 is complete only when every command source uses `CommandList`, every executor uses `TargetResolver`, normalized key plus raw bytes cross protocol v6, clients contain no mux binding policy, all required commands run through the fair queue, destructive defaults require confirmation, malformed input is bounded, all new release thresholds pass without weakening old gates, and every verification command in Task 10 is green on `main`.
