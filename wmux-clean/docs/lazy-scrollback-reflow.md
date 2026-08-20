# Lazy Scrollback Reflow

Step 13 removes completed scrollback from the pane-resize critical path.

## Representation

The primary grid has three regions:

```text
canonical history -> wrapped viewport -> wrapped rows below the viewport
```

Completed historical content is stored as immutable logical lines. A logical
line owns compact styled cells without continuation cells or trailing default
cells. Physical rows that were joined by terminal autowrap remain one logical
line.

The history container is a deque. Appending and evicting at the configured
history limit are therefore constant-time with respect to total history size.
Completed cell buffers are never mutated. The only mutable history entry is an
unfinished tail whose continuation is still crossing into the viewport.

## Resize Contract

A width resize does not walk completed history. It reflows only:

1. the unfinished logical history tail, when the viewport starts in the middle
   of that logical line;
2. the visible viewport;
3. rows retained below the viewport after a height contraction.

The old viewport top and cursor are converted to logical offsets before this
bounded reflow and mapped back afterward. Rows preceding the mapped viewport
top return to canonical history. This follows zellij's canonical
`lines_above`/viewport boundary model while retaining tmux-compatible wrapped
line and cursor semantics.

Alternate-screen resize remains absolute and does not reflow, as before.

## Lazy Materialization

Consumers that need old physical rows, such as copy mode and search, call
`Grid::history_lines_at_width`. The grid wraps canonical history on first use
for that width and retains the two most recent width caches. Ordinary output,
rendering, split, and resize paths never materialize old history.

Any history mutation invalidates width caches. Cache state is derived and is
excluded from grid equality; cloned cache rows share immutable line storage.

## Reference Model

- tmux's grid keeps wrap metadata authoritative and maps cursor positions
  through reflow rather than replaying terminal output.
- zellij stores `lines_above` canonically and combines only the logical line
  crossing the viewport boundary when its viewport width changes.
- wmux combines these rules with its compact copy-on-write `Line` storage and
  client-scoped renderer baselines.

## Verification

Tests cover immutable completed history, viewport-crossing wrapped lines,
cursor anchoring, and width-cache reuse. The existing terminal replay and
resize fixtures verify that visible-grid behavior remains unchanged.

The full release `history-resize-100k` workload is the regression benchmark.
Resize latency must scale with active viewport content, not completed history.
