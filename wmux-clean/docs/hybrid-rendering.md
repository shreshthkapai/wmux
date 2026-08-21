# Hybrid Rendering And Frame Scheduling

wmux follows tmux's cached redraw-scene model and zellij's client-output
buffering model. Structural composition and ordinary pane output are separate
render paths.

## Structural scenes

Each attached client caches a `StructuralScene` containing the active window,
client dimensions, active pane, pane rectangles, and compact border spans. The
cache remains valid across ordinary pane output. A changed window, layout,
zoom, active pane, or client size produces a different structure and forces an
authoritative scene render.

Attach, window switch, resize, layout changes, journal loss, and an uncertain
client baseline use the authoritative path. It composes the current server
grid and never replays pane output.

An authoritative structural render still diffs against a valid client
baseline. Layout changes do not repaint unaffected physical terminal regions.
Only a missing, invalid, or differently sized baseline requires a full paint.

## Direct pane damage

With a valid structure and client baseline, retained pane generations select
only changed pane rows. Those rows are copied from the authoritative pane grid
into the client baseline and translated to VT updates directly. Full-width
single-pane scroll damage may use terminal scroll-region operations when the
client advertises support. Split panes use row updates because vertical-only
scroll margins cannot safely protect neighboring columns.

Full-width pane rows share the authoritative immutable `Line` backing with the
candidate client baseline. Partial-width panes still compose only their owned
rectangle. Numeric cursor, erase, color, and style parameters are serialized
directly into the frame buffer, matching tmux's buffered capability-write model
without temporary formatting allocations.

Rendering is transactional. A candidate baseline is committed only after the
client's bounded outbound queue accepts the frame. A blocked client therefore
keeps its last known baseline and consumed generations while later pane damage
coalesces. Other clients continue independently.

Cursor state is also transactional. Pane painting and destructive erases run
with the physical cursor hidden. Position, DECSCUSR shape, and final visibility
are emitted together as post-render state, matching tmux's final mode update
and zellij's post-VTE cursor instructions. An application visibility change is
never published before the content update it accompanies.

## Adaptive scheduling

There is no fixed repaint tick.

- The first update after 12 ms of inactivity is immediately eligible.
- Sustained output coalesces for 4-8 ms.
- Input gives the originating client a 50 ms priority window. Output in that
  window is published after a 1 ms redraw-cycle deferral, matching tmux's
  event-loop redraw timer and coalescing split PTY reads from one TUI action.
- Structural changes and synchronized-output commits are immediate.
- Blocked clients do not create expired-deadline spin loops; accumulated
  damage is rendered when `ClientWritable` arrives.

Application synchronized output (`DECSET 2026`) is held until reset or the
existing safety timeout. Generations emitted while held are not consumed by a
client. The completed application frame is then published atomically.

PTY resize output uses the same publication discipline. A compact retained
pane frame remains visible until synchronized-output completion or a measured
quiet deadline; held pane generations are not consumed. This avoids cloning a
whole `Screen` and prevents transient clear frames from reaching clients.

## Terminal capabilities

IPC protocol version 6 carries terminal capability bits in `Hello` and
`HelloOk`. The server emits complete unframed render transactions. The client
owns the physical host terminal and wraps each accepted transaction in one
locked synchronized-output write when support was advertised, following
zellij's client-output boundary. Specialized scroll operations remain
server-selected per client. The Windows client recognizes Windows Terminal and
known `TERM_PROGRAM` hosts; `WMUX_SYNCHRONIZED_OUTPUT=1` or `0` provides an
explicit override for terminals whose environment is not identifiable.

## References

- tmux `screen-redraw.c`: cached structural scenes, generation invalidation,
  and pane-targeted redraws.
- tmux `tty.c` and `input.c`: capability-gated terminal operations and
  synchronized output.
- zellij `zellij-server/src/screen.rs` and `zellij-server/src/output/mod.rs`:
  debounced rendering, per-client output, and output buffers.
- zellij `zellij-client/src/stdin_ansi_parser.rs`: synchronized-output
  capability handling.
