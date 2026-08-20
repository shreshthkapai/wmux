# Terminal Batching And Damage

wmux parses each fair-scheduler PTY chunk into an OS-neutral semantic batch,
applies that batch to the authoritative pane screen, and commits one pane
generation. Parsing and client rendering do not share mutable dirty flags.

## Parser Model

The parser uses the Alacritty `vte` 0.14 state machine, the same mature parser
family used by zellij. Its slice API searches ground-state input for escapes,
decodes UTF-8 incrementally across calls, bounds CSI parameters to 32 fixed
slots, and bounds OSC storage. Unknown CSI, OSC, DCS, and escape sequences are
parsed to a valid terminal boundary and ignored when wmux has no semantic
handler; malformed bytes cannot desynchronize the surrounding stream.

`vte::Perform` callbacks are collected before screen mutation. Adjacent print
callbacks share one text arena and become one `PrintRun`. The screen applies
ASCII runs directly to a line segment and falls back to Unicode-width-aware
cell writes for non-ASCII text. Other callbacks become semantic operations for
cursor movement, clears, scrolling, insertion/deletion, modes, styles, and
saved cursor state.

`TerminalEngine` owns and reuses this batch workspace. Clearing a batch retains
the text and operation capacities established by bounded PTY chunks, so
sustained TUI output does not allocate a new parser workspace for every read.
Parser state remains incremental across batches, including fragmented UTF-8
and control sequences.

This follows tmux's separation between input parsing and collected
`screen-write` items while using zellij's proven Rust VT parser instead of a
wmux-specific escape state machine.

## Generations And Journal

Every non-empty applied batch advances the pane screen generation exactly
once. Every changed visible line receives that same generation. Primary and
alternate grids track changed rows separately, so a batch that switches
screens cannot lose line provenance.

Each screen retains 512 generation batches. Journal operations are compact
descriptions such as:

```text
PrintRun
ClearRange
ScrollRegion
InsertDelete
CursorMove
ModeChange
Full
```

Adjacent print, cursor, and clear operations are coalesced. A pathological
batch with more than 256 damage operations collapses to `Full`; the grid still
contains the exact result. Once the 512-batch journal rolls over, only clients
older than the retained generation range require a full redraw.

## Client Consumption

Each attached client stores its own consumed generation per visible pane. A
generation is consumed only after that client's output queue accepts the
rendered frame. A blocked client keeps its old baseline while other clients
continue rendering. No renderer clears pane state globally.

The existing client-scoped cell baseline remains the final correctness check
for terminal output diffing. Generations avoid unnecessary scene work and
detect journal gaps; they do not replace the authoritative grid.

## Correctness Gates

The benchmark crate retains the previous parser model under `cfg(test)` only.
Every deterministic Codex and Claude replay frame is applied to both parsers,
then all visible cells, cursor state, and relevant modes are compared. Core
tests cover split UTF-8, malformed and oversized CSI, wide cells, alternate
screens, synchronized output, journal rollover, and generation assignment.
Server tests verify independent generation consumption by two clients.

## References

- tmux `input.c`, `screen-write.c`, `grid.c`, and `tty.c`
- zellij `zellij-server/src/panes/terminal_pane.rs` and
  `zellij-server/src/panes/grid.rs`
- Alacritty `vte` 0.14 parser (`Apache-2.0 OR MIT`)
