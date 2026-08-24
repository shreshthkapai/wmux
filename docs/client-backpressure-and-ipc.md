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
for that client yet. Once the writer reports the completed byte count, all
accumulated damage is rendered as one current frame. Other clients continue
against their own baselines and generation cursors.

Control replies are allowed into the same byte-accounted outbox. A client that
cannot accept a critical reply within the bound is disconnected. Pane processes
and sessions remain alive.

The named-pipe read and write halves run as separate async futures. A stalled
write therefore cannot block input dispatch from that client, the state owner,
PTY processing, or another client.

## IPC ownership

Protocol version 3 uses the outer frame length as the sole payload length.
Variable payloads do not carry a redundant nested length prefix.

Live async writes consume a `Message` into an `EncodedFrame`. `Output`, `Key`,
`Input`, and `Paste` retain their existing `Vec<u8>` allocation and are emitted
as header and payload slices with vectored writes. Live decoding transfers the
frame payload allocation directly into the resulting message.

Synchronous transports use stack storage for fixed payloads and vectored
writes for borrowed variable payloads. `encode_frame_into` is available to
callers that need contiguous frames and retains caller-owned capacity between
frames.

Disabled terminal tracing performs no hex or escaped-text construction in hot
key and paste paths.
