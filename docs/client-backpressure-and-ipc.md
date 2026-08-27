# Client Backpressure And Low-Copy IPC

wmux isolates every attached client from the state-owner runtime and from all
other clients. Each client has an independent sender and redraw baseline, with
byte accounting enforced at the server boundary.

## Per-client outbox

Each client has an independent bounded channel and byte counter. Ordinary
queued output is limited to 4 MiB and 64 messages. A single authoritative
frame may exceed the ordinary budget when the queue is empty, but can never
exceed the protocol's 16 MiB frame limit.

The state owner never awaits a client write. If a render is already in flight,
later pane generations remain in the authoritative grids and are not rendered
for that client yet. IPC-writer drain only releases the corresponding byte
budget. Once the client acknowledges successful physical presentation, all
accumulated damage is rendered as one current frame. Other clients continue
against their own baselines and generation cursors.

Clients due in the same owner turn share one immutable structural snapshot and
pane-generation map when their session, physical size, and selected theme frame
match. Terminal diff baselines, scroll offsets, copy state, capabilities, and
outboxes remain client-owned. The cache exists only for that render turn, so a
later state mutation cannot reuse stale structure.

Control replies are allowed into the same byte-accounted outbox. A client that
cannot accept a critical reply within the bound is disconnected. Pane processes
and sessions remain alive.

The transport read and write halves run independently. A stalled IPC write
therefore cannot block input dispatch from that client, the state owner, PTY
processing, or another client.

## Physical presentation boundary

Protocol version 9 sequences `Output` transactions and requires an exact
`OutputAck`. Each server-side client has a one-slot presentation gate. Queue
acceptance reserves that slot and advances the candidate diff baseline, but no
successor diff may use it until the matching physical acknowledgement arrives.
The next visible transition therefore cannot be derived from that candidate
baseline until the physical terminal has reached it.

The attached client sends each render to a persistent capacity-one worker that
owns the blocking terminal write. The async attach loop remains free to forward
key, paste, mouse, and resize messages. It acknowledges only a successful
complete transaction. A terminal-write failure ends the disposable attachment
without acknowledging or affecting persistent pane processes.

PTY reads, parsing, grid mutation, command execution, and semantic input do not
pause behind the gate. Scheduler deadlines exclude clients with a frame in
flight, avoiding expired-deadline spin. When the gate opens, intermediate
generations collapse into one diff to the latest server-owned scene; terminal
events and PTY bytes themselves are never coalesced or discarded.

## IPC ownership

Protocol version 9 uses the outer frame length as the sole payload length.
Variable payloads do not carry a redundant nested length prefix. `Output`
adds only its fixed eight-byte sequence prefix, and `OutputAck` is one fixed
eight-byte payload.

Live async writes consume a `Message` into an `EncodedFrame`. `Output`, `Key`,
`Input`, and `Paste` retain their existing `Vec<u8>` allocation and are emitted
as header/prefix/payload slices with vectored writes. Output decoding removes
the sequence prefix in place and transfers the original allocation into the
resulting message instead of making a second render-sized allocation.

Synchronous transports use stack storage for fixed payloads and vectored
writes for borrowed variable payloads. `encode_frame_into` is available to
callers that need contiguous frames and retains caller-owned capacity between
frames.

Disabled terminal tracing performs no hex or escaped-text construction in hot
key and paste paths.
