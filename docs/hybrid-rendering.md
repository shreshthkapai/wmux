# Hybrid Rendering And Frame Scheduling

wmux separates structural composition from ordinary pane output and maintains
an independent output baseline for each attached client.

## Structural scenes

Each attached client caches a `StructuralScene` containing the active window,
client dimensions, active pane, pane rectangles, compact border spans, and the
composed status row. The cache remains valid across ordinary pane output. A
changed window, layout, zoom, active pane, title, status text, or client size
produces a different structure and forces an authoritative scene render.

Attach, window switch, resize, layout changes, journal loss, and an uncertain
client baseline use the authoritative path. It composes the current server
grid and never replays pane output.

An authoritative structural render still diffs against a valid client
baseline. Layout changes do not repaint unaffected physical terminal regions.
Only a missing, invalid, or differently sized baseline requires a full paint.

## Default terminal UI

The final physical terminal row is server-owned UI whenever the client has at
least two rows. Pane layout and platform PTY sizes use the remaining rows, so
the status bar never covers application content and scroll regions cannot
damage it. A one-row client keeps that row pane-owned instead of creating a
zero-height PTY.

The default status row is intentionally timeless and compact:

```text
 wmux · demo    0:shell  [1:server]    pane 0 · npm dev
```

It shows the session, ordered window list, current window, active pane index,
and authoritative OSC pane title with the window name as fallback. Side
sections are bounded to one third of the client width; when the window list no
longer fits, the current window wins. Control characters from names or titles
are replaced before cell composition. The window list stays centered against
the client width while pane focus changes; it only moves when a side section
would otherwise overlap it. The default has no clock, animation, or timer that
can create idle redraw work.

Decoration never selects a font or fixed color. Border and status cells keep
`Color::Default` for both foreground and background, allowing the host
terminal theme to remain authoritative. The status row uses reverse video and
the selected window uses a bold cutout. Inactive pane separators use connected
single box-drawing glyphs; the edge adjacent to the active pane uses the
matching heavy glyph and bold attribute. Nested layouts are reduced to proper
corners, tees, and crossings before compact row spans are cached.

For an exact two-pane layout, a vertical separator belongs to the first pane on
its top half and the
second pane on its bottom half; a horizontal separator belongs to the first
pane on its left half and the second pane on its right half. Only the active
pane's owned half uses the heavy glyph. Moving focus therefore moves the thick
half of the same separator instead of making the entire divider permanently
heavy.

The server-owned status and active-adjacent border selection remain part of
wmux's scene and client-baseline model. Confirmation and editing prompts
temporarily replace the status row. Confirmations hide the physical cursor;
editing prompts place and show the real terminal cursor at the grapheme-aware
input position.
Editing prompts update through client-scoped scene diffs, so typing does not
mutate the pane grid or require a full-scene repaint.

## Theme Frames

The same scene composer accepts a resolved UI frame for inactive borders,
active-adjacent borders, status segments, and editing prompts. Presets, theme
files, one-shot provider output, and explicit configuration are resolved before
rendering. Reload commits a complete validated theme generation atomically;
failed candidates never partially repaint a client or mutate pane state. See
the [UI theme and animation guide](ui-themes.md) for the public schema.

Animation timing is client-scoped and deadline-driven. A client retains its
own theme generation, start time, rendered-frame index, next boundary, and
terminal baseline. Blocked clients schedule no animation wakeups. When one
becomes writable after several missed boundaries, selection advances directly
to the current deterministic frame and emits at most one update instead of
replaying obsolete frames. One-shot playback stops scheduling after its final
frame; static themes never add a deadline.

## Direct pane damage

With a valid structure and client baseline, retained pane generations select
only changed pane rows. Those rows are copied from the authoritative pane grid
into the client baseline and translated to VT updates directly. Full-width
single-pane scroll damage may use terminal scroll-region operations when the
client advertises support. Split panes use row updates because vertical-only
scroll margins cannot safely protect neighboring columns. The pane rectangles
exclude the server UI row, so direct damage cannot overwrite the status bar.

Full-width pane rows share the authoritative immutable `Line` backing with the
candidate client baseline. Partial-width panes still compose only their owned
rectangle. Numeric cursor, erase, color, and style parameters are serialized
directly into the frame buffer without temporary formatting allocations.

Rendering is transactional. A candidate baseline is committed only after the
client's bounded outbound queue accepts the frame. That frame then holds the
client's one-slot physical-presentation gate until its exact sequence is
acknowledged. Later pane damage continues updating authoritative grids but
cannot build an obsolete successor transaction. When the gate opens, all
accumulated generations become one current diff. Other clients continue
independently.

Changed rows are serialized coherently from left to right. A row completes its
text, style changes, and destructive erase before rendering moves to another
row; erases are never collected into a transaction-wide tail that can expose a
mixed scene. Cursor position, shape, and visibility are finalized once after
all row work. Deterministic mixed-update replay covers clears, overwrites,
styles, wide Unicode, combining marks, cursor changes, and bracketed-paste
state against the authoritative grid.

Cursor state is also transactional. On hosts without synchronized output, pane
painting and destructive erases run with the physical cursor hidden. On hosts
that commit a frame atomically, wmux avoids redundant hide/show transitions and
emits only a real visibility change. Position, DECSCUSR shape, and final
visibility remain post-render state. An application visibility change is never
published before the content update it accompanies.

Applications without an explicit synchronized-output transaction may split a
single hide, repaint, and show sequence across multiple PTY reads. A visible to
hidden transition therefore holds that pane against each client's previous
complete baseline for at most 8 ms. Restoring the cursor releases the completed
frame immediately; reaching the deadline publishes an intentionally hidden
cursor. Parsing and input continue throughout the hold, and another pane's
redraw cannot expose the intermediate cursor state.

## Adaptive scheduling

There is no fixed repaint tick.

- The first update after 12 ms of inactivity is immediately eligible.
- Sustained output coalesces for 4-8 ms.
- Input gives the originating client a 50 ms priority window. Output in that
  window is published after a 1 ms redraw-cycle deferral, coalescing split PTY
  reads from one TUI action.
- Structural changes and synchronized-output commits are immediate.
- Clients with an unacknowledged physical frame do not create expired-deadline
  spin loops; accumulated damage is rendered after the matching `OutputAck`.

Application synchronized output (`DECSET 2026`) is held until reset or the
existing safety timeout. Generations emitted while held are not consumed by a
client. The completed application frame is then published atomically.

PTY resize output uses the same publication discipline. A compact retained
pane frame remains visible until synchronized-output completion or a measured
quiet deadline; held pane generations are not consumed. This avoids cloning a
whole `Screen` and prevents transient clear frames from reaching clients.

## Terminal capabilities

IPC protocol version 9 carries terminal capability bits in `Hello` and
`HelloOk`, sequences every non-empty render, and acknowledges only a completed
host-terminal write. The server emits complete unframed render transactions.
The client owns the physical host terminal and wraps each accepted transaction
in one locked synchronized-output write when support was advertised.
Specialized scroll operations remain server-selected per client. The Windows
client recognizes Windows Terminal and known `TERM_PROGRAM` hosts;
`WMUX_SYNCHRONIZED_OUTPUT=1` or `0` provides an explicit override for terminals
whose environment is not identifiable.
