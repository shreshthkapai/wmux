# Phase 7 Shared Multiplexer Semantics Design

## Goal

Phase 7 completes wmux's nonvisual automation and configuration backbone on
Windows, Linux, and macOS. It adds typed inherited options, deterministic
formats, command-based configuration, server-owned paste buffers, hooks,
bounded asynchronous jobs, and versioned control-mode records while preserving
the single server state owner and the Phase 5 platform seam.

UI/UX, status-line layout, decorative chrome, palettes, icons, animation, and
visual option names remain out of scope. Pane application cells and the host
terminal theme remain authoritative.

## Researched Model

The design follows tmux revision
`7b833d07d9f1b58343fc88d7de3c2e0bd9f9aa8c`:

- `options.c` stores typed values in parent-linked trees and resolves a local
  value before walking its parents;
- `format.c` expands values from a bounded, target-aware context rather than
  letting renderers query arbitrary state;
- `paste.c` keeps binary-safe named and automatic buffers in the server,
  ordered by creation, and evicts only automatic buffers at the configured
  limit;
- hooks are option-backed command lists inserted into the serialized command
  queue;
- `job.c` owns bounded asynchronous child execution outside pane state while
  command continuations remain in the command queue;
- `control.c` preserves record ordering and applies explicit high-water marks
  per client instead of allowing slow consumers to grow memory without bound.

It follows zellij revision
`82c4a24d701ecf9a48aa01bcc5c0bb3882747fe7` for parsed configuration with
diagnostic source locations, server-owned background-job scheduling, and
clipboard delivery through a client-side terminal adapter (OSC 52 where that
is the native terminal mechanism).

The native job adapter follows the same platform rules already used for pane
processes: Windows owns process handles and Job Objects; Unix owns process
groups, signals, descriptors, and reaping. No native identity crosses the
platform seam.

## Approaches Considered

### Deep core modules with semantic effects (selected)

Each semantic area is a focused core module with one small interface. Commands
mutate these modules only on the state-owner thread and return semantic effects
for filesystem, clipboard, process, and protocol work. Native job mechanics
sit behind one new platform interface. This preserves authority, makes each
module directly testable, and keeps platform code narrow.

### Put all behavior in `wmux-server`

This would minimize new types initially, but it would make the already-large
server runtime own parsing, inheritance, persistence, process policy, and wire
formatting. The deletion test fails: the complexity would spread across
command handling, connection handling, rendering, and native adapters.

### Model every feature as a protocol extension first

This would make remote automation flexible, but it would duplicate command
semantics between ordinary and control clients and expose unstable internal
types. Phase 7 instead keeps the command language authoritative and adds only
the records needed for ordered control replies, notifications, and pane
output.

## Architecture

Phase 7 adds six deep modules:

```text
wmux-core/options.rs  typed definitions, scoped values, inheritance
wmux-core/formats.rs  bounded target-aware expansion
wmux-core/paste.rs    binary-safe named/automatic buffer store
wmux-core/hooks.rs    inherited event -> command-list resolution
wmux-core/jobs.rs     stable job identity and bounded lifecycle state
wmux-core/control.rs  platform-neutral control records and subscriptions
```

`ServerState` owns every semantic store. `wmux-server` remains the orchestration
layer: it applies command effects, reads config/files, routes clipboard records,
submits native job requests, and publishes control records. Neither clients nor
platform adapters mutate core state.

The data flow remains serialized:

```text
CLI/config/key/hook/control input
  -> shared command parser
  -> CommandQueue
  -> ServerState mutation
  -> semantic CommandEffect
  -> platform/protocol action
  -> semantic completion event
  -> CommandQueue continuation
```

## Typed Options And Inheritance

`OptionStore` exposes `set`, `unset`, `get`, and `list` over:

```rust
pub enum OptionTarget {
    Server,
    Session(SessionId),
    Window(WindowId),
    Pane(PaneId),
    Client(ClientId),
}

pub enum OptionValue {
    Flag(bool),
    Number(i64),
    String(String),
}
```

Definitions provide name, allowed scope, type, default, and numeric/string
bounds. Unknown built-in names are rejected; user options beginning with `@`
are bounded strings. Values resolve locally first, then through these chains:

```text
pane -> window -> session -> server
window -> session -> server
client -> attached session -> server
session -> server
server
```

The store receives a small resolver describing object relationships rather
than retaining references or duplicating the server object graph. Destruction
removes object-local option maps. Initial nonvisual definitions are
`buffer-limit`, `history-limit`, `remain-on-exit`, `repeat-time`,
`set-clipboard`, and `exit-empty`. Existing hardcoded values are migrated only
where Phase 7 behavior consumes them; visual defaults are not introduced.

Commands are `set-option`/`set-window-option` and
`show-options`/`show-window-options`, with tmux-compatible aliases and target
flags plus a wmux client scope. All target resolution uses `TargetResolver`.

## Deterministic Formats

`FormatEngine::expand` accepts immutable `ServerState`, `FormatContext`, and an
input string. It supports literal text, `##`, `#{name}`, and
`#{?condition,then,else}`. Values include stable server/session/window/pane/
client identifiers and names, pane geometry, option values, and bounded user
variables. Unknown variables expand to the empty string, matching tmux's
practical command-format behavior; malformed syntax returns a source-offset
error.

Expansion is limited to 32 nested expressions, 1 MiB input, and 1 MiB output.
It performs no I/O, process spawning, time lookup, or mutable global access, so
identical state and context always produce identical bytes. `display-message`
and list-command format flags use this one engine.

## Functional Configuration

`wmux-config` continues to own config discovery and bootstrap settings needed
before core construction (`agent_compat`, `agent_ui`, and `pane.env.*`). Every
other nonempty line is retained as command source text with its original line
number. `wmux-server` parses that text with `parse_command_text` and enqueues it
as `CommandSource::Config`; it does not maintain a second command grammar.

`source-file` is a semantic command effect. The server reads at most 1 MiB,
parses through the shared command parser, and inserts the resulting commands
immediately after the active command. Includes are limited to 16 levels and
canonical-path cycles are rejected. Startup configuration errors name the file,
line, and parser diagnostic and do not partially execute an invalid file.

Existing generated key/value config remains valid. Visual keys remain rejected
until the Phase 9 UI/UX hold point.

## Paste Buffers And Clipboard

`PasteBufferStore` owns `Arc<[u8]>` payloads, names, automatic/named status,
and monotonically increasing order. Empty automatic buffers are ignored.
Automatic buffers use `bufferN` names and evict the oldest automatic buffer at
`buffer-limit`; named buffers are never evicted by that limit. Names, counts,
individual payloads, and aggregate memory are bounded.

Commands are `set-buffer`, `load-buffer`, `save-buffer`, `show-buffer`,
`list-buffers`, `delete-buffer`, and `paste-buffer`. File effects are executed
by the server with explicit size checks. Copy mode first stores an automatic
buffer, then asks the attached client to update the platform clipboard when
`set-clipboard` permits it. Clipboard writes continue through
`TerminalBackend::write_clipboard_text`; the core never imports a desktop or
terminal clipboard mechanism.

Pasting does not submit a giant platform write. The owner stores a bounded
pending paste and advances it in 64 KiB chunks between event/command turns.
Bracketed-paste wrappers are selected once from the authoritative pane screen
mode, and chunk boundaries never alter payload bytes.

## Hooks And Notifications

`HookStore` uses the same `OptionTarget` inheritance model and stores bounded
`CommandList` arrays for a fixed nonvisual `HookEvent` enum. `set-hook` and
`show-hooks` are the public commands. Initial events cover client attach/
detach, session/window/pane create/remove/select, buffer change/delete, and job
completion.

After a successful mutation the owner resolves hooks from the most-specific
target and inserts them after the triggering command. Hook sources carry a
depth; depth 16 is the hard recursion limit. A hook error fails that hook
invocation and is reported to control clients, but it does not roll back the
already-completed event or corrupt another client's queue. Stable target order
and registration order define execution order.

The same semantic events feed control notifications. Hooks and control records
therefore cannot disagree about what happened.

## Bounded Jobs And Command Continuations

The platform contract gains a native-free job seam:

```rust
pub struct PlatformJobId(u64);
pub struct SpawnJob { id: PlatformJobId, command: String, cwd: Option<PathBuf>, environment: Vec<_> }
pub enum JobRequest { Spawn(SpawnJob), Terminate { id: PlatformJobId } }
pub enum JobEvent { Output { id, bytes }, Exited { id, exit_code }, Closed { id } }
pub trait JobBackend { submit(...); try_next_event(...); }
```

`ServerPlatform::create_job_backend` constructs the adapter beside the PTY
backend. Unix executes `/bin/sh -c` in a new process group and reaps it;
Windows executes `%COMSPEC% /D /S /C` in a kill-on-close Job Object. Output is
read in chunks through a bounded 64-entry event channel. Each job retains at
most 1 MiB of output and the server permits at most 64 running jobs.

`run-shell` can run in the background or hold its command-queue item until the
job closes. `if-shell` chooses one already-parsed command list by exit status.
The queue supports inserting commands after an active item, so continuations
run before the original invocation completes and ordering remains
deterministic. Server shutdown force-terminates all native jobs.

## Versioned Control Mode

Protocol version 7 adds `EnterControl`, `ControlCommand`, and `ControlRecord`
messages. Ordinary clients retain their current behavior. A control client
enters mode after the normal authenticated handshake and sends commands with a
monotonic client sequence. The server responds with structured begin/output/
end/error records and publishes lifecycle, buffer, job, and pane-output
records.

`wmux -C` is a line-oriented adapter: stdin lines become `ControlCommand`
messages and records are rendered as stable tmux-like `%begin`, `%output`,
`%notification`, `%end`, and `%error` lines. Arbitrary bytes are octal-escaped;
there is no lossy UTF-8 conversion.

Each control client has an ordered bounded queue sharing the existing 64-frame
and 4 MiB client limits. Pane output is sliced to 64 KiB records. A slow control
client transitions to a paused subscription with one explicit pause record;
it does not block the owner, other clients, pane parsing, or command replies.
`refresh-client` resumes from authoritative current state; raw output that was
explicitly skipped while paused is not silently claimed as delivered.

## Error Handling And Limits

- Option names: 256 bytes; user string values: 64 KiB.
- Format/config/command input and output: 1 MiB.
- Paste buffer: 16 MiB each, 64 MiB aggregate, automatic count from a bounded
  `buffer-limit` of 1 through 1,000.
- Hook registrations: 256 total; recursion depth: 16.
- Jobs: 64 concurrent, 1 MiB captured output each, 16 KiB native read chunks.
- Control records: 64 KiB pane chunks under the existing 4 MiB client budget.

Malformed input returns bounded diagnostics. Protocol messages exceeding the
existing 16 MiB frame limit are rejected before allocation. No malformed
config, format, hook, job output, control record, or buffer payload may panic
the server.

## Testing And Verification

Implementation is test-first in four independently reviewable slices:

1. options, formats, and functional configuration;
2. paste buffers, files, clipboard routing, and throttled pane writes;
3. hooks, native jobs, and queue continuations;
4. protocol-v7 control mode and bounded subscriptions.

Each slice runs focused unit/integration tests and the Windows workspace suite
before its direct-to-`main` commit. The final gate runs formatting, clippy,
all Windows tests, Linux native/shared tests, both macOS target checks,
portable conformance on Windows/Linux, real detached CLI smoke, protocol
documentation checks, and the release performance gate. The portable semantic
fingerprint may change only by adding explicit Phase 7 cases and must match
across platforms.

## Exit Gate

Phase 7 is complete only when:

1. option inheritance, format expansion, config source execution, buffers,
   hooks, jobs, and control records are server-owned and deterministic;
2. slow jobs, large pastes, and slow control clients remain bounded and cannot
   stall other clients;
3. Windows and Linux native execution plus macOS compile/CI coverage use the
   same shared semantics without native types in core;
4. protocol v7 documentation and compatibility diagnostics match the code;
5. no UI/UX or pane-theme policy has been introduced; and
6. all tested changes and evidence are committed directly to `main`.
