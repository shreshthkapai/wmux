# Terminal Text and Fuzzing Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve complete Unicode grapheme text and terminal-default styles through wmux's authoritative parser/grid/render/copy lifecycle, while making terminal and protocol parsing explicitly bounded and fuzzable.

**Architecture:** Add an OS-neutral `CellText` value that keeps the common single-scalar case inline and allocates shared owned storage only after another scalar joins the cell. `Screen` uses Unicode grapheme boundaries to decide whether a scalar extends the preceding cell and Unicode string-width rules to maintain continuation cells; every downstream consumer copies or emits `CellText` instead of projecting it back to one `char`. The existing `vte` state machine remains the byte parser, with its fixed limits made explicit, while wmux adds bounded title handling, recovery tests, deterministic conformance fixtures, and independent cargo-fuzz targets.

**Tech Stack:** Rust 2021, `vte` 0.14.1, `unicode-segmentation` 1.13.3, `unicode-width` 0.2.2, `smallvec` 1.15, `libfuzzer-sys` 0.4.13, Cargo fuzz.

**Spec:** `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md` Task 2, plus `AGENTS.md` and `docs/windows-first-cross-os-execution-plan.md`.

## Global Constraints

- Make product changes only under the canonical `wmux-clean/` workspace; the legacy root workspace remains migration input.
- Keep terminal parsing, cell storage, screen state, rendering semantics, conformance, and fuzz entry points OS-neutral.
- Follow the researched tmux model: a printable grid cell owns the complete bounded UTF-8 sequence for one displayed character, and wide cells retain an explicit continuation column.
- Improve on the researched Zellij behavior that currently drops zero-width scalars; do not copy that known grapheme-loss behavior.
- Cap one `CellText` at 32 UTF-8 bytes, matching tmux's `UTF8_SIZE`; an over-limit extension is ignored without panic or partial mutation.
- Use extended grapheme boundaries from UAX #29 and non-CJK `UnicodeWidthStr::width`; clamp stored terminal widths to 0, 1, or 2 columns.
- Keep `Color::Default` unresolved in core. SGR 39/49 restore it, and renderers emit no palette choice for it.
- Do not introduce themes, palettes, decorative chrome, borders, icons, gradients, motion, or UI configuration.
- Preserve chunked parsing, printable ASCII fast paths, copy-on-write lines, client-scoped render baselines, and batched terminal output.
- Follow TDD for every behavior change: focused failing test, observed expected failure, minimal implementation, focused pass, broader pass, commit.
- Cargo-fuzz execution requires nightly LLVM sanitizer support on Unix-like x86-64/aarch64 hosts; Windows verification compiles/tests the ordinary crates and validates fuzz manifests/targets without claiming a native fuzz run.

---

### Task 1: Introduce the Bounded `CellText` Value and Unicode Tables

**Files:**
- Modify: `wmux-clean/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-core/Cargo.toml`
- Modify: `wmux-clean/crates/wmux-core/src/lib.rs`
- Create: `wmux-clean/crates/wmux-core/src/text.rs`
- Create: `wmux-clean/docs/terminal-text-model.md`
- Modify: `wmux-clean/Cargo.lock`

**Interfaces:**
- Consumes: Rust `char`, existing `smallvec`, tmux's 32-byte UTF-8 cell limit, and UAX #29/UAX #11 table crates.
- Produces: `CellText`, `MAX_CELL_TEXT_BYTES`, `scalar_width(char) -> u8`, `extends_grapheme(&CellText, char) -> bool`, `CellText::try_append(char) -> bool`, `CellText::display_width() -> u8`, `CellText::push_to(&mut String)`, and `CellText::write_utf8(&mut Vec<u8>)`.

- [x] **Step 1: Add failing `CellText` tests**

  Create `text.rs` with tests that name the exact regressions: a scalar must need no owned overflow, `e + U+0301` must remain one text value, `U+2764 + U+FE0F` must have width 2, `woman + ZWJ + laptop` must remain one extended grapheme, two regional indicators must form one flag while a third starts another grapheme, and a 33-byte extension must be rejected without changing the existing text.

```rust
#[test]
fn scalar_uses_no_owned_overflow_until_a_combining_mark_arrives() {
    let mut text = CellText::from('e');
    assert!(!text.has_owned_overflow());
    assert!(text.try_append('\u{301}'));
    assert!(text.has_owned_overflow());
    assert_eq!(text.to_string(), "e\u{301}");
}

#[test]
fn emoji_sequences_use_string_width_not_scalar_width_sum() {
    let mut text = CellText::from('\u{1f469}');
    assert!(text.try_append('\u{200d}'));
    assert!(text.try_append('\u{1f4bb}'));
    assert_eq!(text.to_string(), "\u{1f469}\u{200d}\u{1f4bb}");
    assert_eq!(text.display_width(), 2);
}

#[test]
fn cell_text_rejects_extensions_past_tmux_compatible_limit_atomically() {
    let mut text = CellText::from('a');
    for _ in 0..15 {
        assert!(text.try_append('\u{301}'));
    }
    let before = text.clone();
    assert!(!text.try_append('\u{301}'));
    assert_eq!(text, before);
}
```

- [x] **Step 2: Run the focused tests and observe RED**

  Run from `wmux-clean/`:

```powershell
cargo test -p wmux-core text::tests -- --nocapture
```

  Expected: compilation fails because the planned `CellText` API and Unicode dependencies do not exist.

- [x] **Step 3: Add pinned Unicode dependencies and minimal implementation**

  Add exact workspace dependencies and consume them only in `wmux-core`:

```toml
unicode-segmentation = "=1.13.3"
unicode-width = { version = "=0.2.2", default-features = false }
```

  Implement this public shape; the tagged pointer-sized representation makes cloned scrollback/render snapshots share combined strings without growing each grid cell.

```rust
pub const MAX_CELL_TEXT_BYTES: usize = 32;

#[repr(transparent)]
pub struct CellText(NonZeroUsize);

impl CellText {
    pub fn first_char(&self) -> char;
    pub fn has_owned_overflow(&self) -> bool;
    pub fn byte_len(&self) -> usize;
    pub fn ends_with(&self, ch: char) -> bool;
    pub fn try_append(&mut self, ch: char) -> bool;
    pub fn display_width(&self) -> u8;
    pub fn push_to(&self, out: &mut String);
    pub fn write_utf8(&self, out: &mut Vec<u8>);
}

impl fmt::Display for CellText;

pub fn scalar_width(ch: char) -> u8;
pub fn extends_grapheme(text: &CellText, next: char) -> bool;
```

  Store a scalar inline and tag an `Arc<String>` raw pointer only for combined text, with clone/drop ownership tests and documented safety invariants. Build candidate graphemes in `SmallVec<[u8; 32]>`, validate them with `str::from_utf8`, use `UnicodeSegmentation::graphemes(candidate, true)`, and derive widths through `UnicodeWidthChar`/`UnicodeWidthStr`. Do not normalize or rewrite application text.

- [x] **Step 4: Run focused tests and observe GREEN**

```powershell
cargo test -p wmux-core text::tests -- --nocapture
```

  Expected: every new text-model test passes with no warnings.

- [x] **Step 5: Record the researched model**

  In `terminal-text-model.md`, record the exact local references (`tmux/tmux.h`, `tmux/utf8-combined.c`, `tmux/screen-write.c`, Zellij `terminal_character.rs` and `grid.rs`), the selected 32-byte limit, dependency versions/Unicode rules, zero-width-at-column-zero behavior, style inheritance, width-change behavior, default-colour rule, and fuzz/benchmark gates.

- [x] **Step 6: Commit the text primitive**

```powershell
git add wmux-clean/Cargo.toml wmux-clean/Cargo.lock wmux-clean/crates/wmux-core/Cargo.toml wmux-clean/crates/wmux-core/src/lib.rs wmux-clean/crates/wmux-core/src/text.rs wmux-clean/docs/terminal-text-model.md
git commit -m "feat(core): add bounded Unicode cell text"
```

---

### Task 2: Make Grid and Screen Grapheme-Aware

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/grid.rs`
- Modify: `wmux-clean/crates/wmux-core/src/screen.rs`
- Modify: `wmux-clean/crates/wmux-core/src/terminal.rs`

**Interfaces:**
- Consumes: Task 1 `CellText`, `extends_grapheme`, and width functions.
- Produces: `Cell::text() -> &CellText`, `Cell::printable_text(CellText, u8, Style)`, `Line::set_text(u16, CellText, u8, Style)`, and screen behavior that appends a grapheme extension to the preceding base cell while preserving wide-cell invariants.

- [x] **Step 1: Add failing terminal/grid tests for grapheme storage**

  Add literal, hand-derived expectations for combining marks in one feed and across feeds, split UTF-8, variation selectors, emoji ZWJ sequences, modifiers, flags, zero-width input at column zero, and overwriting either half of a wide cell.

```rust
#[test]
fn combining_marks_survive_split_terminal_feeds_in_one_cell() {
    let mut engine = TerminalEngine::new();
    let mut screen = Screen::new(8, 2);
    engine.feed(&mut screen, b"e");
    engine.feed(&mut screen, "\u{301}".as_bytes());
    let line = screen.grid().line(0).unwrap();
    assert_eq!(line.text(), "e\u{301}");
    assert_eq!(line.width_at(0), 1);
    assert_eq!(screen.cursor(), (0, 1));
}

#[test]
fn emoji_zwj_sequence_occupies_one_wide_cell() {
    let screen = run("\u{1f469}\u{200d}\u{1f4bb}x".as_bytes());
    let line = screen.grid().line(0).unwrap();
    assert_eq!(line.text(), "\u{1f469}\u{200d}\u{1f4bb}x");
    assert_eq!(line.width_at(0), 2);
    assert_eq!(line.width_at(1), 0);
    assert_eq!(line.width_at(2), 1);
}

#[test]
fn zero_width_scalar_at_column_zero_is_ignored_like_tmux() {
    let screen = run("\u{301}a".as_bytes());
    assert_eq!(screen.grid().line(0).unwrap().text(), "a");
    assert_eq!(screen.cursor(), (0, 1));
}
```

- [x] **Step 2: Run tests and observe RED**

```powershell
cargo test -p wmux-core terminal::tests -- --nocapture
```

  Expected: assertions fail because zero-width input is dropped and emoji components occupy separate cells.

- [x] **Step 3: Replace `Cell.ch` with `CellText` and preserve convenience APIs**

  Keep `Cell::printable(char, ...)`, `Line::set(char, ...)`, and `Grid::set(char, ...)` as scalar conveniences for layout/test callers. Add text-aware variants, make blank/continuation statics use the const scalar constructor, compare complete text in equality, and expose `Cell::ch()` only as a compatibility accessor for the first scalar. Pack width and continuation state into unused high bits of the canonical style word so `Cell` stays within its 16-byte memory budget. Add a line helper that atomically replaces a base plus continuation cells when a combined sequence changes width.

- [x] **Step 4: Implement previous-cell grapheme extension in `Screen::put_char`**

  Before normal placement of a non-ASCII printable scalar, locate the preceding base cell (respecting `pending_wrap` and wide continuations), test the candidate with `extends_grapheme`, enforce the 32-byte cap, recompute whole-string width, and update cursor/continuation state only after the line mutation succeeds. Width-zero input with no valid preceding base is ignored. The ASCII run fast path remains unchanged.

- [x] **Step 5: Run focused and core tests**

```powershell
cargo test -p wmux-core terminal::tests -- --nocapture
cargo test -p wmux-core grid::tests -- --nocapture
cargo test -p wmux-core screen::tests -- --nocapture
cargo test -p wmux-core
```

  Expected: all grapheme, wide-cell, resize, scrollback, and existing terminal tests pass.

- [x] **Step 6: Commit grapheme-aware authoritative state**

```powershell
git add docs/superpowers/plans/2026-08-20-terminal-text-and-fuzzing.md wmux-clean/crates/wmux-core/src/grid.rs wmux-clean/crates/wmux-core/src/screen.rs wmux-clean/crates/wmux-core/src/terminal.rs wmux-clean/crates/wmux-core/src/text.rs wmux-clean/docs/terminal-text-model.md
git commit -m "feat(core): preserve graphemes in pane grids"
```

---

### Task 3: Preserve Complete Text Through Reflow, Copy, Rendering, and Default Colours

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/grid.rs`
- Modify: `wmux-clean/crates/wmux-core/src/copy_mode.rs`
- Modify: `wmux-clean/crates/wmux-core/src/render.rs`
- Modify: `wmux-clean/crates/wmux-core/src/screen.rs`
- Modify: `wmux-clean/crates/wmux-core/src/terminal.rs`
- Modify: `wmux-clean/crates/wmux-core/src/state.rs`
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`
- Modify: `wmux-clean/crates/wmux-bench/src/legacy_terminal.rs`

**Interfaces:**
- Consumes: grapheme-aware `Cell`/`Line` state from Task 2.
- Produces: full-text `Line::text`, reflow, copy extraction, scene equality/diff output, byte-based conformance hashing, and explicit `Color::Default` semantics.

- [x] **Step 1: Add failing lifecycle and renderer tests**

  Add tests proving the same literal sequence survives wrapped reflow, history materialization, copy-mode selection, full render, a client diff after changing a combined cell, and a `ServerState` detach/reattach. Each expected string must be a literal rather than produced by a wmux helper.

```rust
#[test]
fn selection_copies_combined_text_without_splitting_grid_columns() {
    let mut line = Line::blank(8);
    let mut text = CellText::from('e');
    assert!(text.try_append('\u{301}'));
    line.set_text(0, text, 1, Style::default());
    line.set(1, 'x', 1, Style::default());
    let lines = vec![line];
    let mut mode = CopyMode::new(PaneId::new(1), 0, 0, 1, 1);
    mode.handle_key(b" ", &lines, 1);
    mode.handle_key(b"l", &lines, 1);
    assert_eq!(mode.selected_text(&lines), "e\u{301}x");
}

#[test]
fn full_and_diff_render_emit_complete_grapheme_bytes() {
    let mut engine = TerminalEngine::new();
    let mut screen = Screen::new(8, 2);
    let mut state = RenderState::new(8, 2);
    engine.feed(&mut screen, "e\u{301}".as_bytes());
    assert!(String::from_utf8(render_diff(&screen, &mut state)).unwrap().contains("e\u{301}"));
    engine.feed(&mut screen, b"\r");
    engine.feed(&mut screen, "a\u{301}".as_bytes());
    assert!(String::from_utf8(render_diff(&screen, &mut state)).unwrap().contains("a\u{301}"));
}
```

- [x] **Step 2: Run tests and observe RED**

```powershell
cargo test -p wmux-core copy_mode::tests::selection_copies_combined_text_without_splitting_grid_columns -- --nocapture
cargo test -p wmux-core render::tests::full_and_diff_render_emit_complete_grapheme_bytes -- --nocapture
```

  Expected: text after the first scalar is missing from copy/render output.

- [x] **Step 3: Replace every lossy `cell.ch()` projection**

  Make `Line::text` and copy selection call `CellText::push_to`; make renderer output call `CellText::write_utf8` unless hidden; make reflow place cloned `CellText`; compare complete text in border collision logic; and hash every UTF-8 byte plus width/continuation/style in conformance. Retain `ch()` only where layout code intentionally deals in one ASCII border glyph.

- [x] **Step 4: Represent terminal defaults explicitly**

  Change the colour model to:

```rust
pub enum Color {
    Default,
    Indexed(u8),
    Rgb(u8, u8, u8),
}
```

  Make `Style::default()` use `Color::Default` for both foreground and background, make SGR 39/49 restore `Color::Default`, preserve `Default` in style interning, and have `push_color` emit no palette sequence for it after the renderer's SGR reset. Update existing literal assertions from `None`/`Some` to direct `Color` variants.

- [x] **Step 5: Run focused tests and the portable consumers**

```powershell
cargo test -p wmux-core
cargo test -p wmux-conformance -p wmux-server -p wmux-bench
```

  Expected: all tests pass; old ASCII and style behaviors remain unchanged while complete grapheme bytes survive every consumer.

- [x] **Step 6: Commit complete text propagation**

```powershell
git add docs/superpowers/plans/2026-08-20-terminal-text-and-fuzzing.md wmux-clean/crates/wmux-core/src/grid.rs wmux-clean/crates/wmux-core/src/copy_mode.rs wmux-clean/crates/wmux-core/src/render.rs wmux-clean/crates/wmux-core/src/screen.rs wmux-clean/crates/wmux-core/src/terminal.rs wmux-clean/crates/wmux-core/src/state.rs wmux-clean/crates/wmux-conformance/src/lib.rs wmux-clean/crates/wmux-bench/src/legacy_terminal.rs
git commit -m "feat(core): retain cell text across rendering and copy"
```

---

### Task 4: Make Parser Bounds and OSC Titles Explicit

**Files:**
- Modify: `wmux-clean/crates/wmux-core/src/screen.rs`
- Modify: `wmux-clean/crates/wmux-core/src/terminal.rs`
- Modify: `wmux-clean/docs/terminal-text-model.md`

**Interfaces:**
- Consumes: `vte::Parser` fixed CSI/intermediate/OSC storage and the existing `TerminalBatch` operation queue.
- Produces: explicit `MAX_OSC_BYTES = 1024`, `MAX_TITLE_BYTES = 512`, bounded `Screen::title()`, `TerminalOperation::SetTitle(String)`, and recovery guarantees for oversized CSI, OSC, DCS, SOS, PM, and APC input.

- [ ] **Step 1: Add failing title and malformed-control tests**

```rust
#[test]
fn osc_zero_and_two_update_a_bounded_authoritative_title() {
    let mut engine = TerminalEngine::new();
    let mut screen = Screen::new(20, 2);
    engine.feed(&mut screen, b"\x1b]0;first\x07");
    assert_eq!(screen.title(), "first");
    let long = format!("\x1b]2;{}\x1b\\", "x".repeat(MAX_TITLE_BYTES + 40));
    engine.feed(&mut screen, long.as_bytes());
    assert_eq!(screen.title().len(), MAX_TITLE_BYTES);
}

#[test]
fn oversized_control_strings_recover_to_printable_ground_state() {
    let mut bytes = Vec::new();
    bytes.extend_from_slice(b"\x1bPq");
    bytes.extend(std::iter::repeat_n(b'x', 16 * 1024));
    bytes.extend_from_slice(b"\x1b\\after");
    let screen = run(&bytes);
    assert_eq!(screen.grid().line(0).unwrap().text(), "after");
}
```

  Add equivalent recovery cases for an over-parameterized CSI, oversized OSC, and the SOS/PM/APC string-control introducers.

- [ ] **Step 2: Run tests and observe RED**

```powershell
cargo test -p wmux-core terminal::tests -- --nocapture
```

  Expected: the title test fails because OSC is currently ignored; recovery tests then define the accepted parser behavior.

- [ ] **Step 3: Implement bounded title dispatch and explicit parser type**

  Store `title: String` in `Screen`, truncate only at UTF-8 scalar boundaries to 512 bytes, and add an OSC handler for commands 0 and 2. Declare `TerminalEngine.parser` as `vte::Parser<MAX_OSC_BYTES>` so the 1 KiB parser allocation cannot silently change with a dependency default. Keep DCS and other string controls discard-only: `Perform::put` must not accumulate bytes.

- [ ] **Step 4: Run focused and malformed-input tests**

```powershell
cargo test -p wmux-core terminal::tests -- --nocapture
```

  Expected: title updates and every malformed/oversized sequence recover without panic or unbounded storage.

- [ ] **Step 5: Commit parser bounds**

```powershell
git add wmux-clean/crates/wmux-core/src/screen.rs wmux-clean/crates/wmux-core/src/terminal.rs wmux-clean/docs/terminal-text-model.md
git commit -m "feat(core): bound terminal control strings"
```

---

### Task 5: Expand Portable Conformance and Add Fuzz Targets

**Files:**
- Modify: `wmux-clean/crates/wmux-conformance/src/lib.rs`
- Create: `wmux-clean/fuzz/Cargo.toml`
- Create: `wmux-clean/fuzz/fuzz_targets/protocol_frame.rs`
- Create: `wmux-clean/fuzz/fuzz_targets/terminal_bytes.rs`
- Create: `wmux-clean/fuzz/corpus/protocol_frame/bad-magic`
- Create: `wmux-clean/fuzz/corpus/protocol_frame/short-frame`
- Create: `wmux-clean/fuzz/corpus/terminal_bytes/multilingual-prompt`
- Create: `wmux-clean/fuzz/corpus/terminal_bytes/malformed-controls`
- Create: `wmux-clean/fuzz/README.md`
- Create: `wmux-clean/fuzz/Cargo.lock`

**Interfaces:**
- Consumes: public core/protocol parser entry points and the Task 1-4 invariants.
- Produces: deterministic `unicode-terminal-text`, `terminal-modes-and-title`, and `bounded-control-recovery` conformance cases plus `protocol_frame` and `terminal_bytes` libFuzzer binaries.

- [ ] **Step 1: Add failing conformance cases**

  Extend `run_portable_suite` with cases whose literal fixtures cover multilingual prompts, decomposed accents, variation selectors, ZWJ emoji, flags, wide text, alternate-screen transitions, synchronized output, OSC titles, split chunks, and malformed control strings. Assert semantic invariants inside each case before hashing, then leave the old aggregate fingerprint unchanged for the first RED run.

- [ ] **Step 2: Run conformance and observe RED**

```powershell
cargo test -p wmux-conformance -- --nocapture
```

  Expected: `portable_semantic_suite_passes` reports the newly computed fingerprint instead of the old `feb48e6303354e80` value.

- [ ] **Step 3: Hash complete text and accept the intentional fingerprint**

  Hash `CellText` UTF-8 bytes with a byte-length prefix, then width, continuation, and exact style including `Color::Default`. Update `EXPECTED_PORTABLE_FINGERPRINT` only after all per-case semantic assertions pass twice and produce identical results.

- [ ] **Step 4: Create the independent cargo-fuzz package and targets**

  Use this manifest shape so fuzz tooling does not enter the production workspace dependency graph:

```toml
[package]
name = "wmux-fuzz"
version = "0.0.0"
publish = false
edition = "2021"

[package.metadata]
cargo-fuzz = true

[dependencies]
libfuzzer-sys = "=0.4.13"
wmux-core = { path = "../crates/wmux-core" }
wmux-protocol = { path = "../crates/wmux-protocol" }

[[bin]]
name = "protocol_frame"
path = "fuzz_targets/protocol_frame.rs"
test = false
doc = false
bench = false

[[bin]]
name = "terminal_bytes"
path = "fuzz_targets/terminal_bytes.rs"
test = false
doc = false
bench = false

[workspace]
members = ["."]
```

  `protocol_frame` copies at most the fixed header, calls `decode_frame_header`, and calls `decode_frame_payload` only when the declared payload length exactly matches the remaining input. `terminal_bytes` derives dimensions in bounded ranges, feeds the input in varying bounded chunks, resizes once, materializes copy lines, and runs full rendering. Neither target asserts a particular arbitrary-input screen; crashes, panics, and sanitizer findings are failures.

- [ ] **Step 5: Check in small semantic seed corpora and instructions**

  Keep corpus inputs immutable and small. The terminal corpus contains real multilingual/emoji text and malformed control-like data derived from the conformance fixtures; the protocol corpus contains bad magic and truncated headers. Document supported nightly Unix commands:

```bash
cargo +nightly fuzz run terminal_bytes -- -max_total_time=60
cargo +nightly fuzz run protocol_frame -- -max_total_time=60
```

- [ ] **Step 6: Verify targets and conformance**

```powershell
cargo test -p wmux-conformance -- --nocapture
cargo metadata --manifest-path fuzz/Cargo.toml --no-deps
cargo check --manifest-path fuzz/Cargo.toml --bins
```

  Expected: conformance passes deterministically, metadata lists exactly two fuzz binaries, and both targets compile on the current toolchain. Do not claim a sanitizer-backed fuzz run on Windows.

- [ ] **Step 7: Commit conformance and fuzzing**

```powershell
git add wmux-clean/crates/wmux-conformance/src/lib.rs wmux-clean/fuzz
git commit -m "test: add terminal and protocol fuzz coverage"
```

---

### Task 6: Run the Phase 2 Exit Gate and Record Evidence

**Files:**
- Modify: `wmux-clean/docs/terminal-text-model.md`
- Modify: `docs/superpowers/plans/2026-08-20-terminal-text-and-fuzzing.md`

**Interfaces:**
- Consumes: all Task 1-5 commits and the Phase 1 baseline.
- Produces: formatting/lint/test/conformance/fuzz-build/performance evidence and a clean Phase 2 branch ready for review.

- [ ] **Step 1: Run formatting and lint**

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
```

  Expected: both commands exit 0 with no warnings or formatting diff.

- [ ] **Step 2: Run the complete workspace suite**

```powershell
cargo test --workspace
```

  Expected: every pre-existing and Phase 2 test passes; no test is ignored to obtain a green result.

- [ ] **Step 3: Run deterministic conformance twice**

```powershell
cargo run -p wmux-conformance --release
cargo run -p wmux-conformance --release
```

  Expected: both runs emit the same individual and aggregate fingerprints and exit 0.

- [ ] **Step 4: Validate fuzz targets from the stable Windows host**

```powershell
cargo metadata --manifest-path fuzz/Cargo.toml --no-deps
cargo check --manifest-path fuzz/Cargo.toml --bins
```

  Expected: exactly two binaries are described and both compile. Record that sanitizer-backed execution remains a nightly Unix CI/developer command.

- [ ] **Step 5: Enforce the release performance rejection gate**

```powershell
cargo run -p wmux-bench --release -- --suite full --gate
```

  Expected: exit 0, parser throughput remains at least 40 MiB/s for both fixtures, and every existing memory/latency/queue gate passes. If the gate fails materially because of cell storage, reject or redesign the representation rather than raising thresholds.

- [ ] **Step 6: Update evidence and plan checkboxes**

  Record dependency versions, final test count, conformance fingerprints, fuzz-target validation, performance results, and the explicit Windows fuzz-execution limitation in `terminal-text-model.md`. Mark only commands actually observed passing as complete.

- [ ] **Step 7: Inspect branch integrity**

```powershell
git diff --check main...HEAD
git status --short
git log --oneline main..HEAD
```

  Expected: no whitespace errors, no unintended generated artifacts, only Phase 2 files/changes, and coherent incremental commits.

- [ ] **Step 8: Commit final evidence if documentation changed**

```powershell
git add wmux-clean/docs/terminal-text-model.md docs/superpowers/plans/2026-08-20-terminal-text-and-fuzzing.md
git commit -m "docs: record phase 2 terminal correctness evidence"
```

## Exit Gate

Phase 2 is complete only when base plus combining data round-trips through parser, grid, scrollback, copy mode, resize/reflow, full and diff rendering, detach, and reattach; default colours remain unresolved; malformed control strings and frames remain bounded; the two fuzz targets compile with checked-in seeds; all workspace/conformance gates pass; and the unchanged performance thresholds pass in release mode.
