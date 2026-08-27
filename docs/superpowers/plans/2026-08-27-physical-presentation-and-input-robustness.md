# Physical Presentation And Input Robustness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make terminal presentation, streaming, rapid scrolling, cursor state, mouse copy, and printable input converge robustly to the server-owned scene without sacrificing throughput.

**Architecture:** Protocol version 9 adds sequenced render transactions and client presentation acknowledgements. The server keeps at most one render in flight per client while continuing to parse output and process input; the client presents renders on a persistent worker so terminal writes do not stall IPC input, then acknowledges successful presentation. The core diff renderer emits coherent row updates and finalizes cursor state once per transaction.

**Tech Stack:** Rust 1.96, Tokio, crossterm, wmux's framed IPC protocol, server-owned terminal grid and diff renderer, platform `TerminalBackend` adapters, deterministic benchmark and conformance harnesses.

**Spec:** `docs/superpowers/specs/2026-08-27-physical-presentation-and-input-robustness-design.md`

## Global Constraints

- Work directly on `main`; do not create a worktree or use subagents.
- Preserve all user-owned untracked and unrelated files.
- The server grid remains the source of truth and clients remain disposable.
- PTY output and semantic key, paste, mouse, resize, and control events are never dropped.
- At most one unacknowledged render transaction may exist per attached client.
- The server state owner must never await a client or physical terminal write.
- Core and protocol code remain OS-neutral; native mechanics stay in platform crates.
- No task, allocation, IPC frame, or terminal write may be created per PTY byte.
- Protocol version 9 is a clean break; incompatible handshakes remain rejected.
- Existing absolute release performance gates must pass.
- Existing repeated benchmark medians may not regress by more than 5 percent beyond measured run-to-run noise.
- Repository documentation must describe wmux's own contracts without competitor branding.

## File map

- Modify `crates/wmux-protocol/src/lib.rs`: protocol v9, sequenced output, presentation acknowledgement, zero-copy live output framing, validation tests.
- Create `crates/wmux-server/src/presentation.rs`: small per-client render presentation state machine and focused unit tests.
- Modify `crates/wmux-server/src/lib.rs`: route acknowledgements, gate renders, preserve scheduler work, coalesce accumulated generations, integration tests.
- Create `crates/wmux-client/src/presentation.rs`: persistent physical presentation worker and completion messages.
- Modify `crates/wmux-client/src/lib.rs`: classify sequenced output, drive the worker, acknowledge successful writes, remain responsive to input, integration tests.
- Modify `crates/wmux-core/src/render.rs`: coherent row serialization, cursor finalization assertions, replay/property-style regression tests.
- Modify `crates/wmux-unix/src/terminal.rs`: Unix printable-symbol and SGR mouse normalization tests; production only if a failing test identifies a defect.
- Modify `crates/wmux-windows/src/console.rs`: Windows printable-symbol and mouse normalization tests; production only if a failing test identifies a defect.
- Modify `crates/wmux-core/src/event.rs`, `crates/wmux-core/src/keys.rs`, and `crates/wmux-server/src/lib.rs`: exact leading-`&` route tests; production only if the tests expose a transform.
- Modify `crates/wmux-bench/src/main.rs`: protocol-v9 call sites and slow physical-sink/coalescing workload.
- Modify `docs/ipc-protocol.md`, `docs/client-backpressure-and-ipc.md`, `docs/hybrid-rendering.md`, `docs/terminal-batching-and-damage.md`, `docs/scrollback-and-mouse.md`, `docs/windows-input.md`, `docs/performance.md`, and `docs/performance-gates.md`: implemented contracts and measured evidence.

---

### Task 1: Protocol v9 sequenced presentation messages

**Files:**
- Modify: `crates/wmux-protocol/src/lib.rs`
- Modify: every compile-time `Message::Output` construction and match reported by `rg -n "Message::Output" crates`

**Interfaces:**
- Produces: `Message::Output { sequence: u64, bytes: Vec<u8> }`
- Produces: `Message::OutputAck { sequence: u64 }`
- Produces: `VERSION == 9`, `MAGIC == *b"WMX9"`
- Preserves: `EncodedFrame` split-header/split-payload live writes for render bytes.

- [ ] **Step 1: Write failing protocol round-trip and validation tests**

Add cases to `roundtrips_all_basics` and focused tests equivalent to:

```rust
let output = Message::Output {
    sequence: 42,
    bytes: b"frame".to_vec(),
};
assert_eq!(read_message(encode_frame(&output).as_slice()).unwrap(), Some(output));

let ack = Message::OutputAck { sequence: 42 };
assert_eq!(read_message(encode_frame(&ack).as_slice()).unwrap(), Some(ack));

assert_eq!(MAGIC, *b"WMX9");
assert!(decode_frame_payload_owned(18, vec![0; 7]).is_err());
assert!(decode_frame_payload_owned(7, vec![0; 7]).is_err());
```

Also assert that `EncodedFrame::from_message(Message::Output { ... })` keeps the render byte vector as its payload and encodes the eight-byte sequence prefix without concatenating a second complete frame buffer.

- [ ] **Step 2: Run the focused protocol tests and confirm RED**

Run: `cargo test -p wmux-protocol --lib`

Expected: compilation fails because the new variants and version do not exist.

- [ ] **Step 3: Implement version 9 framing**

Change the public protocol surface to:

```rust
pub const VERSION: u32 = 9;
pub const MAGIC: [u8; 4] = *b"WMX9";
const OUTPUT_PREFIX_LEN: usize = 8;

pub enum Message {
    // existing variants
    Output { sequence: u64, bytes: Vec<u8> },
    OutputAck { sequence: u64 },
}
```

Keep output tag `7`, allocate tag `18` to `OutputAck`, and encode output payload as `sequence.to_le_bytes()` followed by the existing byte vector. Special-case output in `EncodedFrame::from_message` and `write_message` so live writers use the fixed sequence prefix and owned render bytes as separate slices. Decode into the original allocation by reading the first eight bytes, shifting the remaining bytes to index zero with `copy_within`, and truncating by eight; do not allocate a second render-sized vector. Require exactly eight bytes for an acknowledgement.

Update exhaustive `Message` matches, `payload_len`, `wire_len`, trace-kind helpers, protocol fuzz expectations, and benchmark constructions. Use sequence `0` only in tests and pure encoding benchmarks that do not model a live connection.

- [ ] **Step 4: Run protocol and workspace compile tests and confirm GREEN**

Run: `cargo test -p wmux-protocol --lib`

Run: `cargo check --workspace --all-targets`

Expected: protocol tests pass and every output call site uses named fields.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-protocol/src/lib.rs crates
git commit -m "protocol: acknowledge physical render presentation"
```

### Task 2: Pure server-side presentation gate

**Files:**
- Create: `crates/wmux-server/src/presentation.rs`
- Modify: `crates/wmux-server/src/lib.rs`

**Interfaces:**
- Consumes: protocol output sequence `u64`.
- Produces:

```rust
pub(crate) struct PresentationGate {
    next_sequence: u64,
    in_flight: Option<u64>,
}

pub(crate) enum PresentationError {
    AlreadyInFlight,
    UnexpectedAck { expected: Option<u64>, actual: u64 },
    SequenceExhausted,
}

impl PresentationGate {
    pub(crate) fn new() -> Self;
    pub(crate) fn ready(&self) -> bool;
    pub(crate) fn begin(&mut self) -> Result<u64, PresentationError>;
    pub(crate) fn acknowledge(&mut self, sequence: u64) -> Result<(), PresentationError>;
    pub(crate) fn in_flight(&self) -> Option<u64>;
}
```

- [ ] **Step 1: Write failing state-machine tests**

Cover the exact transition table:

```rust
let mut gate = PresentationGate::new();
assert!(gate.ready());
assert_eq!(gate.begin().unwrap(), 0);
assert!(!gate.ready());
assert_eq!(gate.in_flight(), Some(0));
assert!(matches!(gate.begin(), Err(PresentationError::AlreadyInFlight)));
assert!(matches!(
    gate.acknowledge(1),
    Err(PresentationError::UnexpectedAck { expected: Some(0), actual: 1 })
));
gate.acknowledge(0).unwrap();
assert!(gate.ready());
assert_eq!(gate.begin().unwrap(), 1);
```

Add a test-only constructor at `u64::MAX` and verify `SequenceExhausted` without wrapping.

- [ ] **Step 2: Run the focused server test and confirm RED**

Run: `cargo test -p wmux-server presentation --lib`

Expected: compilation fails because the module and state machine do not exist.

- [ ] **Step 3: Implement the minimal presentation gate**

Implement the exact interface above with no timers, platform imports, async
code, or allocations. `begin` sets `in_flight` only after obtaining a valid
sequence. A rejected acknowledgement leaves the gate unchanged.

Add `mod presentation;` to the server crate and a `PresentationGate` field to
`ClientView`, initialized by `ClientView::registered`.

- [ ] **Step 4: Run the focused tests and confirm GREEN**

Run: `cargo test -p wmux-server presentation --lib`

Expected: every transition test passes.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-server/src/presentation.rs crates/wmux-server/src/lib.rs
git commit -m "server: model physical presentation state"
```

### Task 3: Client physical presentation worker and acknowledgement

**Files:**
- Create: `crates/wmux-client/src/presentation.rs`
- Modify: `crates/wmux-client/src/lib.rs`

**Interfaces:**
- Consumes: `Message::Output { sequence, bytes }`.
- Produces: `PresentationRequest { sequence, bytes, synchronized_output }`.
- Produces: `PresentationCompletion { sequence, result: PlatformResult<()> }`.
- Produces: `Message::OutputAck { sequence }` after and only after success.

- [ ] **Step 1: Write failing client tests**

Use a blocking fake `TerminalBackend` controlled by channels or a condition
variable. Cover all of these:

```text
successful terminal write -> exactly one matching OutputAck
terminal write failure -> no OutputAck and attach loop returns the error
second Output while one is pending -> InvalidData protocol error
key input while terminal write is blocked -> Key reaches IPC before presentation completes
paste input while terminal write is blocked -> Paste reaches IPC before presentation completes
```

The ordering test must read the client side of an in-memory Tokio duplex stream,
hold the fake terminal write, inject `TerminalInput::Key`, observe
`Message::Key`, release the terminal, and only then observe
`Message::OutputAck`.

- [ ] **Step 2: Run the focused client tests and confirm RED**

Run: `cargo test -p wmux presentation --lib`

Expected: compilation or assertions fail because output is written inline and no acknowledgement exists.

- [ ] **Step 3: Implement one persistent presentation worker per attached client**

Create a bounded request channel of capacity one and a bounded async completion
channel of capacity one. Spawn one named OS thread when attachment begins. Its
loop performs only:

```rust
while let Ok(request) = requests.recv() {
    let result = terminal.write_render_transaction(
        &request.bytes,
        request.synchronized_output,
    );
    if completions.blocking_send(PresentationCompletion {
        sequence: request.sequence,
        result,
    }).is_err() {
        break;
    }
}
```

The attach loop tracks `presentation_in_flight: Option<u64>`. On sequenced
output it rejects a second transaction, stores the sequence, and sends one
request. On matching successful completion it clears the state and sends
`OutputAck`. On mismatched completion it returns `InvalidData`. On terminal
failure it sends no acknowledgement and returns the platform error. The worker
must not own the IPC writer, so key, paste, mouse, and resize messages continue
through the existing async select loop while physical output is blocked.

- [ ] **Step 4: Run client tests and confirm GREEN**

Run: `cargo test -p wmux --lib`

Expected: acknowledgement ordering, failure isolation, and concurrent input tests pass.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-client/src/presentation.rs crates/wmux-client/src/lib.rs
git commit -m "client: acknowledge completed terminal frames"
```

### Task 4: Server render gating and latest-state coalescing

**Files:**
- Modify: `crates/wmux-server/src/lib.rs`
- Test: inline server owner tests in `crates/wmux-server/src/lib.rs`

**Interfaces:**
- Consumes: `PresentationGate` and `Message::OutputAck { sequence }`.
- Produces: `OwnerMessage::OutputPresented { client, sequence }`.
- Preserves: `OwnerMessage::OutboundDrained` solely for byte accounting.

- [ ] **Step 1: Write failing owner integration tests**

Add focused tests proving:

```rust
assert!(owner.render_due_clients(now));
let first = recv_output(&mut outbound_rx);
assert!(!owner.clients[&client].presentation.ready());

owner.handle_event(ServerEvent::PtyOutput { pane, bytes: b"new".to_vec() }).unwrap();
assert!(!owner.render_due_clients(now + Duration::from_millis(20)));
assert!(outbound_rx.try_recv().is_err());

owner.handle_owner_message(OwnerMessage::OutputPresented {
    client,
    sequence: first.sequence,
});
assert!(owner.render_due_clients(now + Duration::from_millis(20)));
let latest = recv_output(&mut outbound_rx);
assert_scene_replays_to_authoritative_grid(&latest.bytes, &owner, client);
```

Also cover invalid, duplicate, stale, and future acknowledgements disconnecting
only the offending client; another attached client rendering while the first is
unacknowledged; expired animation deadlines not causing `next_deadline` spin;
and empty diffs not creating an in-flight sequence.

- [ ] **Step 2: Run focused owner tests and confirm RED**

Run: `cargo test -p wmux-server physical_presentation --lib`

Expected: assertions fail because IPC drain still reopens rendering.

- [ ] **Step 3: Route acknowledgements and gate rendering**

Map `Message::OutputAck` to `OwnerMessage::OutputPresented`. Keep
`OutboundDrained` unchanged except that it no longer makes a client physically
renderable. Change every render eligibility and wakeup condition from the
implicit `queued_bytes == 0` presentation test to both explicit conditions:

```rust
view.queued_bytes == 0 && view.presentation.ready()
```

Immediately before enqueueing non-empty bytes, call `begin` and construct:

```rust
Message::Output { sequence, bytes }
```

If enqueue fails, restore the just-reserved gate state through a narrowly
scoped `cancel(sequence)` method that accepts only the current in-flight value,
then apply existing blocked/disconnect policy. On matching acknowledgement,
clear the gate and request an immediate render when the scheduler, theme
deadline, pane generations, scroll offsets, copy state, overlay state, or full
render flag indicates accumulated work.

Exclude clients with in-flight presentation from owner wakeup deadlines, while
continuing to accept and parse their events.

- [ ] **Step 4: Run server tests and confirm GREEN**

Run: `cargo test -p wmux-server --lib`

Expected: one render stays in flight, accumulated generations converge after acknowledgement, and other clients progress.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-server/src/presentation.rs crates/wmux-server/src/lib.rs
git commit -m "server: gate diffs on physical presentation"
```

### Task 5: Coherent row diffs and cursor finalization

**Files:**
- Modify: `crates/wmux-core/src/render.rs`

**Interfaces:**
- Preserves: `render_full_scene_with_capabilities` and `render_diff_scene_with_capabilities` public signatures.
- Replaces: `render_changed_span(..., paint, erase)` with `render_changed_row(..., out)`.

- [ ] **Step 1: Replace the old ordering assertion with failing coherence tests**

Change `diff_paints_visible_cells_before_destructive_erases` into an assertion
that each row's paint and erase complete before the next changed row begins:

```rust
let paint_first = diff.find("\x1b[1;1Hnew").unwrap();
let clear_first = diff.find("\x1b[1;4H\x1b[K").unwrap();
let paint_second = diff.find("\x1b[2;1Hnewer").unwrap();
assert!(paint_first < clear_first);
assert!(clear_first < paint_second);
```

Add tests for alternating blank/print runs, wide-cell boundaries, styled blank
tails, combining marks, status-line shrink, a `clear`-like whole-pane update,
and exact cursor transaction order:

```text
optional temporary hide
all content mutations
final cursor position
final cursor style when changed
final visibility restoration when required
```

Add a small test-only ANSI replay model or extend the existing replay helper to
apply every emitted diff to the previous scene and compare all cells and final
cursor state to the wanted scene across deterministic randomized mutations.

- [ ] **Step 2: Run focused renderer tests and confirm RED**

Run: `cargo test -p wmux-core render --lib`

Expected: the row-order test fails because erases are appended after all paints.

- [ ] **Step 3: Emit each changed row coherently**

Remove the frame-global erase vector. Implement:

```rust
fn render_changed_row(row: u16, previous: &Line, wanted: &Line, out: &mut Vec<u8>)
```

Walk the widened changed span from left to right. Emit each printable run or
blank erase immediately into `out`. Use erase-to-end only when every remaining
cell through `wanted.cols()` is a default blank; otherwise use exact erase
character counts. Preserve wide-cell span widening and style resets.

Keep cursor finalization after every row mutation. Emit no visibility or style
transition when the transaction made no content change and the client-scoped
state already matches. For non-synchronized content mutation, keep at most one
temporary hide and one final restoration. For synchronized output, rely on the
client wrapper and do not add a temporary visibility pair.

- [ ] **Step 4: Run renderer and core tests and confirm GREEN**

Run: `cargo test -p wmux-core --lib`

Expected: deterministic replay matches the authoritative scene and cursor for every case.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-core/src/render.rs
git commit -m "render: serialize coherent terminal rows"
```

### Task 6: Exact punctuation, scrolling, mouse selection, and clipboard regressions

**Files:**
- Modify: `crates/wmux-unix/src/terminal.rs`
- Modify: `crates/wmux-windows/src/console.rs`
- Modify: `crates/wmux-core/src/event.rs`
- Modify: `crates/wmux-core/src/keys.rs`
- Modify: `crates/wmux-server/src/lib.rs`
- Modify: `crates/wmux-client/src/lib.rs`

**Interfaces:**
- Preserves: platform `TerminalInput`, protocol key/paste/mouse messages, server `ClientInput`, and PTY byte delivery.
- Acceptance byte: ASCII ampersand `0x26`.

- [ ] **Step 1: Add failing-or-proving leading-`&` tests at every boundary**

Add exact assertions:

```rust
// Windows and Unix normalizers
assert_eq!(normalized.code, TerminalKeyCode::Char('&'));
assert_eq!(normalized.raw, b"&");

// Root routing
assert_eq!(
    route_key(&mut state, client, InputMode::Normal, ampersand, 0),
    InputRoute::PaneBytes(b"&".to_vec())
);

// Paste translation
assert_eq!(ClientInput::Paste(b"& run".to_vec()).into_pty_bytes(false), b"& run");
assert_eq!(
    ClientInput::Paste(b"& run".to_vec()).into_pty_bytes(true),
    b"\x1b[200~& run\x1b[201~"
);
```

At the server boundary, send both a wire `Key` and `Paste` beginning with `&`
and assert exact ordered entries in `runtime.test_inputs`. Separately retain the
existing prefix-plus-`&` kill-window binding test.

- [ ] **Step 2: Run focused punctuation tests and classify the result**

Run: `cargo test -p wmux-unix ampersand --lib`

Run: `cargo test -p wmux-windows ampersand --lib`

Run: `cargo test -p wmux-core ampersand --lib`

Run: `cargo test -p wmux-server ampersand --lib`

Expected from the audited code: tests pass without a production transform. If
a boundary fails, make the minimal change at that boundary only and rerun the
same test before proceeding.

- [ ] **Step 3: Add end-to-end mouse and scrolling regressions**

Cover:

```text
Unix SGR Down -> Drag -> Up retains button, coordinates, and modifiers
Windows Down -> Drag -> Up normalization has the same semantic sequence
plain server-owned drag renders a visible selection before release
release writes exact selected text to paste buffer and one Clipboard message
modifier override wins while application mouse tracking is enabled
twenty rapid history wheel events produce the exact clamped final offset
twenty application wheel events produce twenty ordered PTY reports
typed and pasted input return only the originating scrolled client to live view
```

Use deterministic event construction. Do not require a GUI, desktop clipboard,
or timing sleep in unit tests.

- [ ] **Step 4: Run platform, client, core, and server tests and confirm GREEN**

Run: `cargo test -p wmux-unix --lib`

Run: `cargo test -p wmux-windows --lib`

Run: `cargo test -p wmux --lib`

Run: `cargo test -p wmux-core --lib`

Run: `cargo test -p wmux-server --lib`

Expected: exact input bytes, event counts, coordinates, selection, and clipboard payloads pass on their supported targets.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-unix/src/terminal.rs crates/wmux-windows/src/console.rs crates/wmux-core/src/event.rs crates/wmux-core/src/keys.rs crates/wmux-server/src/lib.rs crates/wmux-client/src/lib.rs
git commit -m "test: lock down interactive input semantics"
```

### Task 7: Slow physical-sink workload and performance gates

**Files:**
- Modify: `crates/wmux-bench/src/main.rs`
- Modify: `docs/performance-gates.md`
- Modify: benchmark snapshots or CI scripts only if the repository already tracks them.

**Interfaces:**
- Produces benchmark scenario: `physical-presentation`.
- Produces counters: maximum outstanding frames, generated generations,
  presented frames, coalesced generations, final checksum, input progress.

- [ ] **Step 1: Write the failing benchmark self-test and gate**

Add a deterministic workload that simulates 1,000 authoritative scene
generations and a terminal that completes one presentation for every sixteen
generations. Assert:

```text
maximum outstanding frames == 1
presented frames < generated generations
coalesced generations > 0
final presented checksum == latest authoritative scene checksum
input progress == injected input event count
```

Add the scenario to required-report validation so omitting it fails the full
suite.

- [ ] **Step 2: Run the focused benchmark test and confirm RED**

Run: `cargo test -p wmux-bench --bin wmux-bench physical_presentation`

Expected: compilation or required-report validation fails because the workload does not exist.

- [ ] **Step 3: Implement the deterministic workload**

Reuse the real render state and sequenced protocol messages. Model the same
one-slot gate as the server and acknowledge only at deterministic generation
boundaries. Hash the bytes replayed by the simulated physical sink and compare
its final scene with a full render of the latest authoritative state. Record a
violation for backlog above one, lost input, or checksum mismatch.

Do not sleep. The benchmark measures computation and invariants, not wall-clock
terminal emulator scheduling.

- [ ] **Step 4: Run smoke and full release benchmark suites**

Run: `cargo run -p wmux-bench --release -- --suite smoke`

Run: `cargo run -p wmux-bench --release -- --suite full --enforce`

Expected: all absolute gates pass and `physical-presentation` reports zero violations with maximum queue depth one.

- [ ] **Step 5: Commit**

```powershell
git add -- crates/wmux-bench/src/main.rs docs/performance-gates.md
git commit -m "bench: gate physical presentation backlog"
```

### Task 8: Contract documentation and compatibility matrix

**Files:**
- Modify: `docs/ipc-protocol.md`
- Modify: `docs/client-backpressure-and-ipc.md`
- Modify: `docs/hybrid-rendering.md`
- Modify: `docs/terminal-batching-and-damage.md`
- Modify: `docs/scrollback-and-mouse.md`
- Modify: `docs/windows-input.md`
- Modify: `docs/performance.md`
- Modify: `docs/cross-os-conformance.md`
- Modify: `docs/compatibility-matrix.md`

**Interfaces:**
- Documents only behavior verified in Tasks 1 through 7.

- [ ] **Step 1: Update protocol and backpressure contracts**

Document protocol version 9 tags, payload layouts, per-connection sequence
scope, acknowledgement validation, IPC byte drain versus physical presentation,
one-frame gate, slow-client failure behavior, and latest-state coalescing.

- [ ] **Step 2: Update rendering and input contracts**

Document coherent row output, cursor finalization, synchronized-output
capability behavior, streaming convergence, exact scroll event routing,
selection and clipboard semantics, and unprefixed punctuation passthrough.

- [ ] **Step 3: Record benchmark evidence**

Run three full release suites, record the median before/after values for parser,
damage render, full scene, animated UI, multi-client, memory, and physical
presentation, and explain any variance over 5 percent with the raw repeated
runs. Do not claim platform performance from a platform that was not executed.

- [ ] **Step 4: Check documentation consistency**

Run: `rg -n "protocol version 8|WMX8|Output\(Vec|written into IPC.*present" docs crates`

Expected: no stale v8/current-contract description remains outside historical release notes or explicit migration context.

Run: `git diff --check`

Expected: no whitespace errors.

- [ ] **Step 5: Commit**

```powershell
git add -- docs
git commit -m "docs: document presentation and input guarantees"
```

### Task 9: Full verification and release-readiness evidence

**Files:**
- Modify only files required by a newly reproduced failure; use the same RED/GREEN cycle before each correction.

**Interfaces:**
- Consumes all deliverables above.
- Produces verified commit history on `main`; does not push or publish without an explicit user request.

- [ ] **Step 1: Run formatting and static checks**

Run: `cargo fmt --all -- --check`

Run: `cargo clippy --workspace --all-targets -- -D warnings`

Run: `git diff --check`

Expected: all commands exit zero.

- [ ] **Step 2: Run workspace and robustness tests**

Run: `cargo test --workspace`

Run the repository's deterministic conformance, stress, fuzz-build, and release-check commands exactly as documented by `docs/RELEASING.md` and the workspace `xtask` help.

Expected: all commands exit zero; malformed protocol and terminal inputs do not panic.

- [ ] **Step 3: Run native Windows lifecycle coverage**

Build the release binaries and run the repository's isolated Windows lifecycle
test target without stopping or replacing the user's currently running wmux
server. Use a unique test endpoint/session as the existing harness requires.

Expected: attach, input, resize, detach, reconnect, pane cleanup, and server shutdown pass.

- [ ] **Step 4: Run Unix/WSL coverage**

From WSL, run the portable crate tests and isolated Unix lifecycle/conformance
harness against the workspace. Do not mutate the user's installed WSL wmux or
its normal server endpoint.

Expected: protocol, PTY, SGR mouse, scroll, detach/reattach, and cleanup tests pass.

- [ ] **Step 5: Run repeated release benchmarks**

Run the full enforced suite three times. Compare medians with the recorded
pre-change baseline and verify every absolute gate, maximum outstanding frame
count, memory ceiling, and checksum.

Expected: zero gate failures and no unexplained hot-path regression above the global threshold.

- [ ] **Step 6: Inspect the final diff and commit any verification-only corrections**

Run: `git status --short`

Run: `git diff HEAD~1 --stat`

Run: `git log --oneline --decorate -12`

Expected: only intended tracked files changed; `.agents/`, `config.wmux`, and the user-owned glyph-named file remain untracked and untouched.

If verification required a correction, commit only its scoped files with a
message describing the actual defect. Otherwise create no empty commit.

## Self-review

- Spec coverage: protocol acknowledgement, physical worker, render gate,
  streaming, scrolling, row coherence, cursor state, mouse copy, clipboard,
  leading `&`, failure handling, cross-platform tests, documentation, and
  performance gates each map to a task above.
- Placeholder scan: the plan contains no deferred implementation or unnamed
  error-handling steps.
- Type consistency: protocol uses `Output { sequence, bytes }` and
  `OutputAck { sequence }` throughout; server state uses `PresentationGate`;
  client worker completion carries the same per-connection `u64` sequence.
