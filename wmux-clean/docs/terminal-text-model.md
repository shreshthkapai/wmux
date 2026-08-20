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
