# Physical Presentation And Input Robustness Design

Date: 2026-08-27
Status: Approved for implementation planning

## Summary

wmux will make the physical terminal presentation boundary explicit. The
server will allow at most one unacknowledged render transaction per attached
client, and the client will acknowledge that transaction only after the host
terminal backend has written and flushed it successfully.

This closes a gap in the current backpressure contract. Today, the server
considers output drained as soon as the IPC writer accepts it. The client has a
second queue between IPC and the physical terminal, so multiple obsolete
render transactions can still accumulate there. A busy full-screen program can
therefore display stale intermediate cursor and cell states even though the
server-owned grid is correct and rendering each individual frame is fast.

The change preserves wmux's server-owned virtual terminal architecture:

```text
PTY output bytes
  -> server-owned parser and grid
  -> current client scene
  -> one sequenced render transaction
  -> physical terminal write and flush
  -> presentation acknowledgement
  -> next diff from the acknowledged baseline
```

PTY output and semantic input are never dropped. When output arrives while a
render is in flight, wmux continues parsing into the authoritative grid and
coalesces presentation work. Once the acknowledgement arrives, the server
renders the newest coherent state rather than replaying obsolete intermediate
frames.

The same work will tighten changed-row serialization, final cursor state,
rapid scrolling, mouse selection and clipboard coverage, and exact handling of
printable punctuation such as a leading `&`.

## User-visible problems

The design addresses these related symptoms:

- text in animated or streaming terminal applications can appear to wobble;
- cursors can flicker, briefly appear at stale locations, or expose a transient
  application cursor state;
- rapid scroll input can feel janky even when grid rendering benchmarks are
  fast;
- clear-and-redraw sequences can briefly expose stale cells;
- mouse drag selection or clipboard delivery can differ across host terminals;
- printable punctuation at the start of an input line must never be mistaken
  for a wmux command unless the prefix was explicitly entered first.

These symptoms share presentation and input-boundary risks, but they do not
change the core ownership model. The server grid remains authoritative, the
client remains disposable, and platform crates continue to own only native
terminal mechanics.

## Evidence and current contract gap

Each attached client currently has two relevant queues:

```text
server state owner
  -> bounded server outbound channel
  -> IPC writer
  -> IPC transport
  -> bounded client inbound channel
  -> physical terminal backend
```

`OutboundDrained` is emitted after the IPC writer completes. The state owner
then permits another render when its byte counter reaches zero. This proves
that the server-side channel drained, but it does not prove that the client
wrote the prior render transaction to the host terminal. The client inbound
channel can consequently contain several complete but obsolete renders.

The existing CPU benchmarks show that parsing, scene construction, damage
rendering, and multi-client rendering are already below their release gates.
The missing benchmark dimension is a deliberately slow physical presentation
sink. This design adds that dimension rather than attempting to fix the issue
by lowering frame rate or adding a larger arbitrary delay.

## Goals

- Permit at most one unacknowledged physical render per attached client.
- Continue parsing all PTY output while a client is presenting a frame.
- Coalesce accumulated visual damage into the newest authoritative scene.
- Keep key, paste, mouse, resize, and control input independent of render
  acknowledgements.
- Make each changed row coherent when a terminal presents part of a transaction
  before the complete write is visible.
- Emit one final cursor position, shape, and visibility state per transaction.
- Preserve exact, ordered application mouse events and exact server-history
  navigation semantics.
- Make normal mouse drag selection and clipboard delivery work through both
  native terminal adapters.
- Preserve a leading `&` byte through key normalization, IPC, root-table
  routing, paste handling, and PTY delivery.
- Preserve or improve the existing rendering and input performance gates.
- Keep all core and protocol semantics OS-neutral.

## Non-goals

- Moving the terminal grid or diff renderer into the client.
- Replaying raw PTY history to repair a client.
- Dropping PTY bytes, semantic input events, or application mouse events.
- Assuming that the host terminal supports synchronized-output mode.
- Adding a product-specific coding-agent compatibility path.
- Defining shell grammar. A shell may interpret a leading `&` specially after
  wmux has delivered the byte correctly.
- Making another multiplexer's implementation or key syntax wmux's contract.

## Selected architecture

### Sequenced render transactions

IPC will advance to protocol version 9. `Output(Vec<u8>)` becomes a sequenced
render transaction, and a new acknowledgement travels from the client to the
server:

```rust
Message::Output {
    sequence: u64,
    bytes: Vec<u8>,
}

Message::OutputAck {
    sequence: u64,
}
```

The sequence is scoped to one IPC connection. The server allocates it in
strictly increasing order and records it before enqueueing the output. A client
sends exactly one matching acknowledgement only after
`TerminalBackend::write_render_transaction` returns success. A failed terminal
write ends the disposable client connection and does not affect sessions or
pane processes.

An acknowledgement with no in-flight transaction, a duplicate sequence, or a
different sequence is a protocol violation. The server disconnects that client
rather than guessing which physical baseline it has.

Protocol version 9 is an intentional clean break. The existing handshake
already rejects incompatible versions, so neither side needs a legacy render
mode.

### Client presentation state

Each attached `ClientView` gains:

```rust
next_output_sequence: u64
in_flight_output: Option<u64>
```

`queued_bytes` continues to account for bounded IPC memory and critical
messages. It no longer represents physical presentation completion.

A client is eligible to render only when all of these are true:

```text
attached
not blocked by the bounded IPC outbox
no render transaction is awaiting acknowledgement
render or theme deadline is due
```

When a non-empty render is accepted by the outbox, it becomes the sole
in-flight transaction. The candidate render baseline and consumed pane
generations become the expected physical state. No later diff is generated
from that baseline until the matching acknowledgement arrives. If the client
disconnects or the physical write fails, its client-scoped baseline is simply
discarded.

An empty render needs no output message or acknowledgement because it changes
no physical terminal state.

### Coalescing while presentation is busy

The server event loop continues to process PTY output and every input event
while a render is in flight. Pane parsers, screens, scrollback, modes, damage,
and generations advance normally. Render requests remain pending in the
client's scheduler.

The owner's wakeup calculation excludes expired render and animation deadlines
for clients with an in-flight transaction. This prevents a slow client from
causing a busy loop. When its acknowledgement arrives, the owner immediately
re-evaluates pending work and renders the newest scene. Missed decorative
animation frames are skipped by selecting the frame appropriate for the current
time.

This is visual coalescing only:

- PTY bytes are all parsed in order;
- keys and paste bytes are all delivered in order;
- mouse events are all routed in order;
- resize transactions retain their existing semantic ordering;
- control-mode records retain their existing lossless contract.

### Streaming behavior

For sustained command output or animated full-screen applications, the
authoritative pane grid can advance through many generations while one physical
transaction is pending. After acknowledgement, wmux computes one diff from the
acknowledged baseline to the latest grid.

The server never sends a sequence of obsolete intermediate snapshots merely to
catch the terminal up. This bounds presentation backlog without reducing PTY
throughput or losing terminal state.

### Scrolling behavior

Scroll input remains semantic and ordered. Presentation coalescing must not
coalesce scroll events themselves.

Routing remains mode-dependent:

```text
application mouse tracking active
  -> encode every wheel event for the pane PTY

alternate-scroll navigation active
  -> encode every navigation input for the pane PTY

server-owned history view active
  -> update that client's scroll offset for every event
```

Multiple history-offset changes may produce one latest visual transaction if a
prior frame is still being presented. The final offset and selected content
must be exact. Returning to live output on typed or pasted input remains
client-scoped.

### Coherent changed-row serialization

The diff renderer currently collects destructive erases separately and appends
them after visible cell paints across the frame. Although the final grid is
correct, a host terminal that presents the byte stream incrementally can expose
old cells or duplicated text between those operations.

The renderer will serialize each changed row as a coherent left-to-right unit:

1. move to the first changed column;
2. emit style and printable runs in display order;
3. clear the remaining changed tail at the point where it becomes invalid;
4. complete the row before moving to another row.

Wide cells, combining marks, wrapped rows, selective erase rules, and protected
cells retain their existing terminal-model semantics. The optimization may
choose a larger coherent row span when that produces a safer or smaller
transaction, but it must never expose a final state different from the
server-owned grid.

### Cursor finalization

A render transaction has one explicit final cursor state:

```text
content and structural mutations
  -> final cursor position
  -> final cursor style
  -> final cursor visibility
```

Temporary cursor suppression used while painting is internal to that one
transaction. It must not leak into a later transaction, and redundant
visibility or style toggles must be omitted when the renderer's client-scoped
physical baseline proves they are unnecessary.

Application-side cursor hide/show bursts retain the bounded repaint hold, but
the presentation gate ensures that obsolete held and unheld snapshots cannot
queue behind one another. Synchronized-output wrapping remains enabled only
when the client advertised real support.

### Mouse selection and clipboard

Mouse routing remains server-owned when wmux mouse mode is enabled:

- a plain left press begins a possible selection;
- a drag converts it into copy mode and renders the selection;
- release copies the selected text, updates the wmux paste buffer, sends one
  clipboard message, and exits the transient selection;
- a modifier override selects wmux behavior when an application has enabled
  mouse tracking;
- otherwise application mouse reports preserve press, drag, release, wheel,
  coordinates, button identity, and modifiers exactly.

Tests will cover native Windows mouse normalization and SGR mouse input used by
Unix terminal hosts. Clipboard tests distinguish selection rendering from
physical clipboard delivery so either layer can fail with a useful diagnosis.
The user's private theme configuration does not participate in mouse routing.

### Printable punctuation and leading `&`

`&` is ordinary pane input in the root key table. It invokes the default
kill-window command only after the configured prefix has placed that client in
the prefix table.

Both input routes must preserve exact bytes:

```text
typed Shift+symbol
  -> semantic character '&' plus raw byte 0x26
  -> protocol key event
  -> root-table passthrough
  -> pane PTY byte 0x26

pasted '& command'
  -> semantic paste payload beginning with 0x26
  -> optional bracketed-paste wrapper
  -> pane PTY bytes beginning with 0x26 inside the wrapper
```

Platform normalization may report the Shift modifier used to produce ASCII
punctuation. Key identity canonicalization may remove that redundant Shift bit
for binding lookup, but it must never alter or discard the produced raw byte.

Regression tests will cover Windows and Unix normalization, protocol
round-tripping, root and prefix table routing, plain paste, bracketed paste, and
server-to-PTY delivery. If these all preserve `0x26`, any later rejection or
special behavior belongs to the active shell's grammar rather than wmux.

## Performance contract

The implementation must preserve these properties:

- no allocation, task, IPC message, or terminal write per PTY byte;
- at most one unacknowledged render transaction per attached client;
- no fixed repaint tick and no idle wakeup from the presentation gate;
- no waiting for a terminal write on the server state-owner thread;
- independent progress for other clients when one physical terminal is slow;
- current absolute release gates for parser, renderer, scene, memory, and
  multi-client workloads continue to pass;
- repeated benchmark medians for existing hot paths must not regress by more
  than 5 percent beyond measured run-to-run noise;
- an end-to-end slow-sink benchmark must demonstrate bounded render backlog and
  prompt convergence to the latest grid;
- input latency tests must show that key, paste, mouse, and resize handling
  continues while a render acknowledgement is delayed.

## Failure handling

- Physical terminal write failure disconnects only that client.
- IPC write failure disconnects only that client.
- Missing acknowledgement keeps at most one render in flight and no unbounded
  render queue; normal connection teardown remains the recovery path.
- Invalid or duplicate acknowledgement disconnects the client as malformed
  protocol input.
- Sequence exhaustion is treated as a connection error rather than wrapping to
  an ambiguous identifier.
- A slow client never blocks the state owner, PTY readers, or other clients.
- A full critical-message outbox retains the existing client-disconnect policy.

## Verification matrix

### Protocol

- version 9 handshake and magic;
- output transaction encode/decode with sequence and maximum payload;
- acknowledgement encode/decode and fixed payload validation;
- malformed, duplicate, stale, and future acknowledgement rejection.

### Server and client lifecycle

- a second render cannot enqueue before the first physical acknowledgement;
- multiple PTY generations coalesce to one latest render after acknowledgement;
- theme animation skips expired intermediate frames;
- another client continues rendering while one client is delayed;
- terminal write success emits one matching acknowledgement;
- terminal write failure emits no acknowledgement and ends the client;
- input and clipboard messages continue to be serviced with a render in flight.

### Rendering

- row shrink, overwrite, clear, wide-cell, combining-mark, wrap, status-row,
  split-border, and alternate-screen diffs end at the authoritative grid;
- destructive erases occur in the coherent row transaction they belong to;
- each render has exactly one correct final cursor position, style, and
  visibility;
- rapid cursor hide/show application bursts do not queue stale cursor frames;
- randomized diff replay reaches the same terminal state as a full render.

### Scrolling and mouse

- sustained output while scrolled preserves the anchored history view;
- rapid wheel events reach the correct final history offset;
- every application mouse wheel event reaches the PTY in order;
- press, drag, release selection renders visibly and copies exact text;
- modifier override works while application mouse tracking is active;
- Windows native and Unix SGR input normalize to the same semantic events.

### Input punctuation

- typed leading `&` reaches the PTY exactly on Windows and Unix;
- pasted leading `&` reaches the PTY in plain and bracketed-paste modes;
- unprefixed `&` is never resolved through the prefix table;
- explicitly prefixed `&` retains the documented kill-window binding.

### Cross-platform and performance

- workspace formatting, linting, unit tests, conformance, stress, and release
  checks pass;
- Windows native lifecycle tests pass;
- Unix and WSL lifecycle tests pass;
- existing benchmark gates pass with recorded before/after results;
- the slow physical-sink workload proves bounded backlog and latest-state
  convergence.

## Documentation changes

Implementation will update:

- `docs/ipc-protocol.md` for version 9 messages and validation;
- `docs/client-backpressure-and-ipc.md` for the distinction between IPC drain
  and physical presentation acknowledgement;
- `docs/hybrid-rendering.md` for presentation gating and frame coalescing;
- `docs/terminal-batching-and-damage.md` for coherent row serialization and
  cursor finalization;
- `docs/scrollback-and-mouse.md` and `docs/windows-input.md` for verified native
  input behavior;
- performance documentation for the slow physical-sink workload and results.

## Acceptance

The work is complete when a deliberately slow host-terminal sink cannot build
an obsolete render backlog, streaming and rapid scrolling converge smoothly to
the latest authoritative scene, cursor state remains coherent, mouse selection
and clipboard tests pass through both platform models, a leading `&` reaches
the pane unchanged, and all existing semantic and performance gates remain
green.
