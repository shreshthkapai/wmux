# Terminal Text Model

wmux stores the terminal's authoritative text as displayed cells, not as a
lossy stream of scalar values. A printable cell owns one bounded extended
grapheme, its display width, its application style, and (for width-two text) a
separate continuation cell in the following grid column.

## Researched Compatibility Model

The local tmux reference uses `struct utf8_data` in `tmux.h` with a 32-byte
`UTF8_SIZE`, stores that complete value in a `grid_cell`, and keeps wide-cell
padding explicit. `screen-write.c::screen_write_combine` attaches combining
characters, variation selectors, zero-width joiners, emoji components, and
composable Hangul Jamo to the preceding cell. `utf8-combined.c` defines the
special sequence decisions. The sparse/extended-cell split in `grid.c` keeps
the common ASCII case compact.

The local Zellij reference uses `TerminalCharacter { character: char, width,
styles }` in `zellij-server/src/panes/terminal_character.rs`. Its
`Grid::add_character` in `zellij-server/src/panes/grid.rs` explicitly drops
width-zero input and links that behavior to the known grapheme segmentation
issue. wmux retains Zellij's useful shared-style and variable-row ideas, but
does not copy that lossy text behavior.

## wmux Representation

`CellText` is one machine word. A tagged scalar encoding stores the common
one-scalar value inline; a tagged `Arc<String>` raw pointer is created only
when another scalar joins the cell. Manual clone/drop implementations preserve
the `Arc` strong-count contract, and dedicated tests cover shared ownership and
independent mutation after cloning. The allocation lets scrollback, frozen
scenes, and per-client baselines share uncommon combined text across line-level
copy-on-write snapshots.

The canonical style ID occupies 57 bits. Cell width and continuation state use
three of its seven otherwise-unused high bits, keeping the complete `Cell`
within the existing 16-byte memory budget even after grapheme support.

One cell accepts at most 32 UTF-8 bytes, matching tmux. An extension beyond the
limit is ignored atomically: the previous text, width, continuation state, and
cursor remain unchanged.

Grapheme boundaries come from `unicode-segmentation` 1.13.3 extended UAX #29
rules. Widths come from non-CJK `unicode-width` 0.2.2 rules for complete strings,
including emoji ZWJ, modifier, and presentation sequences. Stored terminal
widths are limited to zero, one, or two columns. A width-zero scalar at column
zero has no base cell and is discarded, matching tmux's left-edge behavior.

## Style and Rendering Rules

New scalars joined to a cell keep the base cell's application style. wmux does
not normalize, compose, recolor, or otherwise rewrite application text.
Terminal-default foreground and background remain `Color::Default` in core;
only the attached terminal resolves those defaults. Later mux chrome must
remain a separate compositing layer.

Renderers emit the complete `CellText` bytes for visible base cells and never
emit continuation cells. Copy mode and reflow operate on the same complete
value, so detach/reattach reconstructs the current scene from server-owned grid
state rather than replaying pane output.

## Limits and Verification

The pinned `vte` parser bounds CSI parameters at 32 and intermediates at two.
wmux declares its parser as `vte::Parser<1024>` and constructs it through the
fixed-capacity feature path, making the 1 KiB OSC allocation explicit. OSC 0
and 2 update an authoritative screen title capped at 512 UTF-8 bytes without
splitting a scalar. DCS/SOS/PM/APC payloads remain discard-only. Malformed or
oversized input must return to printable ground state without panic.

The Phase 2 gate runs focused Unicode tests, the complete workspace suite,
deterministic cross-OS conformance, protocol/terminal fuzz-target compilation,
and the unchanged release performance thresholds. Sanitizer-backed cargo-fuzz
execution uses nightly Rust on a supported Unix-like host because cargo-fuzz
does not support Windows.

## Phase 2 Verification Evidence

Verified on Windows on 2026-08-20 with Rust 1.96.0. The relevant locked
dependencies are `unicode-segmentation` 1.13.3, `unicode-width` 0.2.2, `vte`
0.14.1, `smallvec` 1.15.2, and fuzz-only `libfuzzer-sys` 0.4.13.

The fresh exit-gate results were:

- `cargo fmt --all -- --check`: passed after applying the formatter's two
  conformance-layout changes.
- `cargo clippy --workspace --all-targets -- -D warnings`: passed.
- `cargo test --workspace`: 163 passed, 0 failed, 0 ignored.
- `cargo metadata --manifest-path fuzz/Cargo.toml --no-deps --format-version 1`:
  exactly `protocol_frame` and `terminal_bytes` were listed.
- `cargo check --manifest-path fuzz/Cargo.toml --bins`: both fuzz harnesses
  compiled on stable Windows.
- No sanitizer-backed fuzz run is claimed from Windows; the checked-in README
  records the supported nightly Unix commands.

Two consecutive release conformance runs produced the same fingerprints:

| Case | Fingerprint |
|---|---:|
| `vt-replay-grid` | `a4b6730791ae60e9` |
| `detach-reattach-persistence` | `3e7ff92dccf4cc09` |
| `multiple-client-consistency` | `05e713cca6822bb7` |
| `resize-reflow` | `09848825260ccebf` |
| `malformed-input-resilience` | `9e0a00486d9b8448` |
| `key-paste-behavior` | `63a3aa224a4d7943` |
| `mouse-mode-routing` | `cfb320142e88231b` |
| `unicode-terminal-text` | `903fdf4289fd38bb` |
| `terminal-modes-and-title` | `27d45dd045405a17` |
| `bounded-control-recovery` | `75180fe6bb88ab1b` |
| **Suite** | **`77b632078fd0ab8b`** |

The unchanged full release performance rejection gate passed:

| Scenario | Total ms | p50 us | p95 us | MiB/s | Alloc MiB | Peak MiB | Queue |
|---|---:|---:|---:|---:|---:|---:|---:|
| `parser-codex` | 16.775 | 0.000 | 0.000 | 118.95 | 2.63 | 1.35 | 0 |
| `parser-claude` | 19.189 | 0.000 | 0.000 | 104.80 | 2.64 | 1.35 | 0 |
| `frame-codex` | 27.746 | 55.500 | 99.900 | 71.91 | 44.15 | 1.42 | 0 |
| `frame-claude` | 28.755 | 56.000 | 107.100 | 69.94 | 44.16 | 1.42 | 0 |
| `hybrid-frame-codex` | 25.590 | 55.400 | 90.200 | 77.97 | 43.73 | 1.40 | 0 |
| `hybrid-frame-claude` | 26.317 | 56.200 | 91.200 | 76.42 | 43.74 | 1.40 | 0 |
| `scene-frame-codex` | 77.925 | 162.500 | 304.100 | 25.61 | 170.49 | 1.76 | 0 |
| `idle-input-render` | 0.747 | 1.400 | 3.800 | 0.51 | 1.38 | 0.04 | 0 |
| `damage-proportional` | 0.015 | 1.200 | 1.200 | 0.07 | 0.01 | 0.00 | 0 |
| `large-paste` | 0.308 | 0.000 | 0.700 | 207590.01 | 0.00 | 0.00 | 0 |
| `history-resize-100k` | 39.228 | 23.900 | 25.500 | 43.76 | 8.92 | 0.02 | 0 |
| `split-storm` | 13.208 | 28.200 | 50.500 | 0.00 | 25.06 | 0.12 | 0 |
| `detach-backlog` | 64.413 | 0.000 | 0.000 | 124.20 | 101.58 | 1.53 | 8192 |
| `multiple-clients` | 70.370 | 151.400 | 255.100 | 28.58 | 53.93 | 1.43 | 8 |
