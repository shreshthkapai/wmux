# Terminal Text Model

wmux stores the terminal's authoritative text as displayed cells, not as a
lossy stream of scalar values. A printable cell owns one bounded extended
grapheme, its display width, its application style, and (for width-two text) a
separate continuation cell in the following grid column whenever that column
exists.

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

When a width-two cell is temporarily wider than a one-column row, wmux retains
the logical base cell without an in-bounds continuation. This follows tmux's
edge-cell preservation and Zellij's reflow rule that the first logical
character may exceed the current row width. Reflow or non-reflow growth restores
the continuation cell, so resize never destroys terminal text.

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

A width-two cell that does not completely fit the remaining physical row is
temporarily rendered as one styled blank. Pane composition applies the same
clipping at local pane edges, so logical preservation cannot wrap a client row
or overwrite a neighboring pane. The complete grapheme becomes visible again
after growth provides both columns.

## Limits and Verification

The pinned `vte` parser bounds CSI parameters at 32 and intermediates at two.
wmux declares its parser as `vte::Parser<1024>` and constructs it through the
fixed-capacity feature path, making the 1 KiB OSC allocation explicit. Because
`vte` exposes at most 16 semicolon-delimited OSC parameters, a parallel bounded
OSC scanner preserves the raw OSC 0/2 title payload and operation ordering.
Titles are capped at 512 UTF-8 bytes without splitting a scalar.
DCS/SOS/PM/APC payloads remain discard-only. Malformed or oversized input must
return to printable ground state without panic.

The Phase 2 gate runs focused Unicode tests, the complete workspace suite,
deterministic cross-OS conformance, protocol/terminal fuzz-target compilation,
and the unchanged release performance thresholds. Sanitizer-backed cargo-fuzz
execution uses nightly Rust on a supported Unix-like host because cargo-fuzz
does not support Windows.

## Phase 2 Verification Evidence

Verified on Windows on 2026-08-20 with Rust 1.96.0. The relevant locked
dependencies are `unicode-segmentation` 1.13.3, `unicode-width` 0.2.2, `vte`
0.14.1, `smallvec` 1.15.2, and fuzz-only `libfuzzer-sys` 0.4.13.

The fresh post-review exit-gate results were:

- `cargo fmt --all -- --check`: passed.
- `cargo clippy --workspace --all-targets -- -D warnings`: passed.
- `cargo test --workspace`: 179 passed, 0 failed, 0 ignored.
- `cargo metadata --manifest-path fuzz/Cargo.toml --no-deps --format-version 1`:
  exactly `protocol_frame` and `terminal_bytes` were listed.
- `cargo check --manifest-path fuzz/Cargo.toml --bins`: both fuzz harnesses
  compiled on stable Windows.
- `cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings`: passed.
- No sanitizer-backed fuzz run is claimed from Windows; the checked-in README
  records the supported nightly Unix commands.

The review regressions now cover pending-wrap ZWJ/modifier/flag extensions,
exact and over-limit cell text, one-column reflow and non-reflow restoration,
Unicode copy search/word movement, OSC titles beyond `vte`'s parameter limit,
UTF-8 continuation bytes in the C1 range, and safe physical rendering of
partially visible wide cells at terminal and pane edges. Protocol fuzzing also
normalizes every non-empty input through payload decoding and includes valid
variable and fixed-shape payload seeds.

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
| `parser-codex` | 14.032 | 0.000 | 0.000 | 142.20 | 2.63 | 1.35 | 0 |
| `parser-claude` | 13.950 | 0.000 | 0.000 | 144.15 | 2.64 | 1.35 | 0 |
| `frame-codex` | 22.862 | 53.200 | 73.000 | 87.28 | 44.15 | 1.42 | 0 |
| `frame-claude` | 22.665 | 53.400 | 63.500 | 88.73 | 44.16 | 1.42 | 0 |
| `hybrid-frame-codex` | 22.426 | 52.900 | 67.300 | 88.97 | 43.73 | 1.40 | 0 |
| `hybrid-frame-claude` | 25.740 | 55.500 | 91.200 | 78.13 | 43.74 | 1.40 | 0 |
| `scene-frame-codex` | 63.408 | 152.200 | 183.000 | 31.47 | 170.49 | 1.76 | 0 |
| `idle-input-render` | 0.758 | 1.400 | 3.800 | 0.50 | 1.38 | 0.04 | 0 |
| `damage-proportional` | 0.011 | 1.000 | 1.000 | 0.09 | 0.01 | 0.00 | 0 |
| `large-paste` | 0.228 | 0.000 | 0.400 | 280948.20 | 0.00 | 0.00 | 0 |
| `history-resize-100k` | 15.580 | 21.500 | 22.600 | 110.18 | 8.92 | 0.02 | 0 |
| `split-storm` | 11.516 | 25.000 | 43.800 | 0.00 | 25.06 | 0.12 | 0 |
| `detach-backlog` | 60.953 | 0.000 | 0.000 | 131.25 | 101.58 | 1.53 | 8192 |
| `multiple-clients` | 56.946 | 135.700 | 161.800 | 35.31 | 53.93 | 1.43 | 8 |
