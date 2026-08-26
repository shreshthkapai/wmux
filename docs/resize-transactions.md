# Resize Transactions

Layout changes are committed by the server state owner as one transaction.
The core computes the final pane rectangles once, compares them with the
authoritative pane rectangles, and records only panes whose rows or columns
changed.

For each final rectangle:

1. An identical rectangle is a no-op.
2. A position-only change updates pane geometry but does not resize the grid or
   PTY.
3. A dimension change updates the geometry, resizes the authoritative screen,
   and records one `PaneResize` intent.
4. Repeated changes before server synchronization coalesce by pane. The intent
   retains the original dimensions and the latest final dimensions.
5. If a pane returns to its original dimensions, its pending resize is removed.

The server first creates any missing platform panes at their final dimensions,
then consumes the resize intents. Existing ConPTYs are resized only when their
tracked dimensions differ from the transaction's final dimensions. This avoids
a second layout computation and avoids reflow, damage generation, ConPTY calls,
and repaint holds for unchanged panes.

`Screen::resize` independently rejects unchanged dimensions. This is a final
invariant guard, not the mechanism used to discover the transaction delta.

Primary-screen width changes use the lazy scrollback contract documented in
`lazy-scrollback-reflow.md`. Completed canonical history is not part of the
resize transaction; only active viewport content and an unfinished logical
line at its upper boundary are reflowed.

Layout is resolved first and pane resize immediately returns when dimensions
are unchanged. Pane geometry remains separate from PTY resize dispatch, and
the explicit delta avoids an all-pane PTY resize pass.

The transaction contains stable pane IDs and core rectangles only. ConPTY/PTY
handles and platform-specific size APIs remain in the platform coordination
layer.

## Publication transaction

PTY resize and client publication are separate phases. Before resizing a PTY,
the server retains a compact pane frame containing only visible copy-on-write
lines plus cursor and terminal-mode metadata. It does not clone the pane's
`Screen`, parser, scrollback, alternate grid, or damage journal.

The structural redraw is diffed against each client's retained baseline. A
valid baseline therefore preserves unchanged terminal regions; only attach,
physical client-size changes, or an invalid baseline require a full terminal
paint. Changed-pane generations are not consumed while that pane has a resize
publication hold.

Output produced in response to the PTY resize continues to update the
authoritative server grid, but clients keep seeing the retained pane frame.
The hold is released atomically when either:

1. the application completes DEC synchronized output (`DECRST 2026`), or
2. unsynchronized output reaches the measured quiet deadline.

A bounded safety deadline prevents a broken application from freezing a pane
forever. Releasing a hold marks the authoritative pane fully damaged, so every
client independently transitions from its retained frame to the latest stable
grid. Client-capable terminals wrap that transition in synchronized output.
No intermediate application clear/blank frame is published.

The unsynchronized quiet interval is four milliseconds. A client sends its
physical terminal size before its attach command, so the initial authoritative
layout is built at the real dimensions instead of rendering a default-sized
scene and immediately replacing it.
