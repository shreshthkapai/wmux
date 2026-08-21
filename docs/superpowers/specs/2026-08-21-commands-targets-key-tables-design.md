# Phase 4 Commands, Targets, and Key Tables Design

**Date:** 2026-08-21

**Status:** Approved for planning

**Roadmap task:** Task 4 of 11 in `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md`

## Goal

Move command parsing, target resolution, prefix handling, and key-binding policy
into server-owned, OS-neutral core modules. Every command source must produce the
same parsed `CommandList`, every target must pass through one `TargetResolver`,
and attached clients must send normalized input rather than interpret mux
bindings.

Phase 4 must also make the ordinary input path exceptionally small: after IPC
decode, an unbound key moves to the active pane without text parsing,
re-encoding, an extra heap allocation, a lock, or a spawned task. The design
targets lower routing overhead than zellij's general action pipeline and avoids
adding work to tmux's already compact server-side binding model. Comparative
performance claims will be made only from measured release-build evidence.

## Scope

Phase 4 includes:

- IPC protocol version 6 with normalized keys plus their original terminal
  bytes.
- A bounded command lexer and parser shared by command strings and CLI argument
  vectors.
- Parsed, reusable `CommandList` values.
- Central target parsing and resolution for clients, sessions, winlinks,
  windows, and panes.
- Server-owned root, prefix, and copy-mode key tables.
- Per-client key-table, prefix, repeat, and confirmation state.
- `bind-key`, `unbind-key`, `list-keys`, `send-keys`, `send-prefix`,
  `switch-client`, `refresh-client`, and a functional `confirm-before` path.
- Tmux-style confirmation defaults for destructive pane and window bindings.
- Fair, bounded command-list scheduling inside the single state-owning server
  loop.
- Focused release-mode routing and parsing performance gates.

Phase 4 does not include config-file loading, hooks, user option inheritance,
visual status design, menus, popups, themes, Unix backends, or the final UI/UX
layer. Those remain in their roadmap phases.

## Researched Compatibility Model

The design follows these local first-class references:

- tmux `cmd-parse.y` and `cmd.c`: one lexer/parser, quoted empty arguments,
  escaped text, semicolon command chains, comments, aliases, and unique command
  prefixes.
- tmux `cmd-find.c`: typed target resolution from a current client context into
  session, winlink, window, and pane objects.
- tmux `key-bindings.c`, `server-client.c`, `cmd-bind-key.c`, and
  `cmd-send-keys.c`: server-resident named key tables, per-client active-table
  state, prefix precedence, repeatable bindings, pre-parsed command lists, and
  command-queue dispatch.
- zellij `zellij-client/src/input_handler.rs`,
  `zellij-utils/src/data.rs`, `zellij-utils/src/input/keybinds.rs`, and
  `zellij-server/src/route.rs`: clients transmit a normalized key identity and
  original bytes; server-side policy chooses a binding or forwards the bytes.

Tmux's command and key semantics are the compatibility baseline. Zellij's
normalized-key transport is adopted because it keeps platform key decoding in
the disposable client while preserving exact bytes for applications. Wmux does
not copy zellij's broad action-routing machinery into the hot path.

## Architecture

### Module seams

`wmux-core` gains four deep modules with small external interfaces:

```text
command::lexer   text -> bounded tokens with source spans
command::parser  tokens/argv -> CommandList
target           TargetSpec + ResolveContext + ServerState -> ResolvedTarget
keys             KeyEvent + client key state + KeyTables -> InputRoute
```

`command::execute` is the only module that applies parsed commands to
`ServerState`. It returns semantic `CommandEffect` values for mechanics the
server adapter must perform, such as pane input, copy-mode entry, refresh,
detach, confirmation, or shutdown. Core never opens IPC, writes a PTY, reads a
clock, or renders directly.

The server owner loop remains the sole mutator. It converts wire messages to
core input, asks `keys` for a route, enqueues any resulting command list, invokes
`command::execute`, and applies returned effects through existing platform and
client-view adapters.

### Command representation

The public command interface is:

```rust
pub struct CommandList(Arc<[Command]>);

pub fn parse_command_text(input: &str) -> Result<CommandList, CommandParseError>;
pub fn parse_command_argv(argv: &[String]) -> Result<CommandList, CommandParseError>;
```

Both entry points converge on the same command-name table, argument parser,
`TargetSpec` parser, validation rules, and `Command` variants. The argv entry
point creates already-separated word tokens rather than joining arguments into
text, so spaces and empty arguments survive exactly. CLI policy may inspect
shared command metadata, but command validity and mux behavior stay in core.

`CommandList` is reference counted because key bindings reuse it. Dispatching a
binding clones only the `Arc`; it never reparses command text.

The Phase 4 text grammar supports:

- spaces, tabs, CRLF, and newlines as separators;
- single and double quotes, including empty quoted arguments;
- backslash escapes for a following character plus `\a`, `\b`, `\e`, `\f`,
  `\n`, `\r`, `\s`, `\t`, `\v`, `\uNNNN`, and `\UNNNNNNNN`;
- unquoted semicolons as command separators;
- `#` comments when `#` begins a token outside quotes;
- exact aliases and unambiguous canonical-name prefixes;
- byte spans and line/column locations for syntax errors.

Environment expansion, tilde expansion, formats, conditionals, and nested
brace command blocks remain out of scope until the configuration and format
phases. A binding receives its command from the remaining parsed argv as a
`CommandList`; quoted command strings pass through the same text parser.

Parser work is bounded by exact constants: 1 MiB input, 4,096 tokens, 256
commands per list, and 64 KiB per token. Crossing a limit returns a syntax error
before any command is enqueued. A list is parsed atomically; no prefix of an
invalid list executes.

### Target model

Target strings parse once into `TargetSpec`. Resolution happens immediately
before each command executes so later commands in a list observe mutations made
by earlier commands. Once resolved, an executor receives stable IDs and does not
repeat string lookup.

```rust
pub struct ResolveContext {
    pub client: ClientId,
    pub current_session: Option<SessionId>,
    pub current_winlink: Option<WinlinkId>,
    pub current_window: Option<WindowId>,
    pub current_pane: Option<PaneId>,
}

pub struct ResolvedTarget {
    pub client: ClientId,
    pub session: Option<SessionId>,
    pub winlink: Option<WinlinkId>,
    pub window: Option<WindowId>,
    pub pane: Option<PaneId>,
}
```

The supported beta target surface is tmux-shaped:

- omitted, empty, `@`, or `{current}` selects the invoking client's current
  context;
- `$N`, `@N`, and `%N` select stable session, window, and pane IDs;
- session names and window names prefer exact matches, then accept only a
  unique prefix;
- `session:window` and `session:window.pane` qualify nested targets;
- numeric windows select winlink display indexes in the resolved session;
- `+N` and `-N` select relative winlinks with wraparound;
- `!` or `{last}` selects the client's previous session, the session's previous
  winlink, or the window's previous pane according to the requested type;
- pane numbers select pane order within the resolved window;
- client targets accept the stable numeric client ID and `{current}`.

Ambiguous and absent matches are distinct typed errors. Commands declare the
target type they require, so a pane-only suffix cannot silently satisfy a
session command. Winlinks are preserved in `ResolvedTarget`; window selection
never loses the session-specific link identity.

### Semantic keys and protocol v6

Core represents a key as a compact sortable value:

```rust
pub struct KeyCode {
    pub key: BareKey,
    pub modifiers: KeyModifiers,
}

pub struct KeyEvent {
    pub code: KeyCode,
    pub raw: Vec<u8>,
}
```

`BareKey` covers Unicode characters, navigation keys, editing keys, function
keys, Enter, Tab, Backspace, Escape, and Space. Modifiers are a compact bitset
for Shift, Alt, Control, and Super. ASCII uppercase normalizes to lowercase plus
Shift, matching zellij's equality model and avoiding duplicate bindings.

Protocol v6 encodes the semantic key in a fixed-width header followed by the
original byte payload. The protocol crate owns only its wire representation;
client and server adapters convert to the core type. Multibyte UTF-8 and escape
sequences remain one key frame, never one frame per byte. Paste remains a
separate semantic message and bypasses key bindings.

The Windows console adapter converts crossterm events directly to normalized
wire keys while retaining its existing application bytes. Later Unix clients
will implement the same adapter without changing core.

### Key tables and input routing

`KeyTables` owns named tables. Each table uses a sorted compact binding vector
with binary-search lookup. Reads are hot and mutation is cold, so this layout is
deterministic and cache-friendly without a randomized hash or a per-binding
allocation.

```rust
pub struct KeyBinding {
    pub key: KeyCode,
    pub repeatable: bool,
    pub commands: CommandList,
}

pub enum InputRoute {
    PaneBytes(Vec<u8>),
    Commands(CommandList),
    CopyModeKey(KeyEvent),
    Consumed,
}
```

Every core `Client` stores only semantic input state: active table, previous
session, prefix deadline, repeat deadline, last repeatable key, and an optional
confirmation. Monotonic milliseconds are supplied by the server loop; core
does not read a clock. Phase 4 uses tmux-compatible defaults of `C-b`, a 500 ms
repeat window, root/prefix/copy-mode tables, and the existing binding surface.
Later functional options may replace those constants without moving policy back
to clients.

Routing precedence is:

1. an active confirmation consumes `y`, `n`, Enter, or Escape;
2. active copy mode selects the copy-mode table;
3. the configured prefix switches a normal client to the prefix table;
4. a binding produces its already-parsed `CommandList`;
5. a missing copy-mode binding reaches the existing copy-mode handler;
6. a missing normal binding moves the original bytes to the active pane.

Paste never enters this path. Existing mouse precedence remains copy-mode mux
handling, application mouse mode, then server scrollback.

Repeatable bindings retain the prefix table until their supplied deadline.
Nonrepeatable keys leave repeat state and retry against the root table, matching
tmux. Input state is per client while binding tables are server-wide, so one
client's prefix or repeat state cannot affect another client.

### Command queue and effects

The queue stores bounded command-list invocations rather than immediately
pushing and popping one command. Each invocation carries its client,
monotonic sequence, `CommandList`, current index, and evolving resolve context.
Per-client pending queues are serviced round-robin, one command at a time,
inside the owner loop. This preserves per-client order and the global
single-mutator invariant without letting a 256-command list monopolize other
clients.

An error stops the remainder of that invocation and produces one deterministic
reply. It does not remove or reorder another client's work. Binding-originated
lists use the same queue but do not require a control reply.

Executors return effects instead of reaching through module seams:

```rust
pub enum CommandEffect {
    PaneInput { pane: PaneId, bytes: Vec<u8> },
    EnterCopyMode { client: ClientId },
    RefreshClient { client: ClientId },
    Confirm { client: ClientId, prompt: String, commands: CommandList },
    DetachClient { client: ClientId },
    Shutdown { requester: ClientId },
}
```

The server applies effects in command order before executing the next command
from the same invocation. Existing pane creation, resize, destruction, render,
and shutdown mechanics remain server adapter responsibilities.

### Phase 4 commands and defaults

The following commands join the same command-name table and parser:

- `bind-key`/`bind`: bind a parsed command list in prefix, root (`-n`), or an
  explicit `-T` table; `-r` marks repeatable.
- `unbind-key`/`unbind`: remove one key from prefix, root (`-n`), or an explicit
  `-T` table; `-a` clears the selected table and still defaults to prefix when
  neither `-n` nor `-T` is present.
- `list-keys`/`lsk`: emit deterministic, parseable bindings ordered by table and
  key.
- `send-keys`/`send`: resolve one pane and emit named keys or literal text;
  `-l` forces literal text and `-N` repeats with a strict bound.
- `send-prefix`: emit the configured prefix key to a resolved pane.
- `switch-client`: change the invoking client to an exact, next, previous, or
  last session through `TargetResolver`.
- `refresh-client`: invalidate only the invoking client's render baseline.
- `confirm-before`/`confirm`: install a plain one-line confirmation and enqueue
  its parsed command list only after affirmative input.

The default prefix binding for `x` becomes `confirm-before kill-pane`, and `&`
becomes `confirm-before kill-window`. The prompt is a temporary client-owned
overlay composed during rendering; it does not mutate pane cells or select a UI
theme. Task 10 will replace or style this functional surface.

## Performance Design and Gates

The key hot path has these enforced properties after IPC decode:

- one packed-key comparison path and at most two binary searches;
- zero command lexing or parsing;
- zero raw-byte clones before the platform pane write;
- zero locks and zero spawned tasks in core/server routing;
- one IPC frame per normalized key event, never per byte;
- shared `Arc` command lists for binding dispatch;
- no redraw unless a command or mode actually changes client-visible state.

`wmux-bench` gains release workloads for unbound-key routing, prefix binding
dispatch, pre-parsed command queue execution, and command-text parsing. The
gate records throughput and allocation counts where the harness can observe
them. Thresholds are set from the first reviewed release measurement with
headroom for CI variance, then become non-regression gates. Existing full-suite
thresholds must continue to pass.

A small documented comparison run will exercise equivalent normalized-key
lookup and passthrough scenarios in the local zellij and tmux reference trees
where their build/test harnesses permit. Results will be labeled by workload;
wmux will not claim end-to-end superiority from a synthetic microbenchmark.

## Error Handling and Safety

- Lexer and parser errors carry stable spans and never panic on arbitrary UTF-8
  command text.
- Wire key decoding rejects unknown key tags, invalid Unicode scalars, invalid
  modifier bits, and trailing data.
- Key names, table names, repeat counts, command-list sizes, and confirmation
  prompts have explicit bounds.
- `send-keys` writes only after target resolution and complete argument
  validation.
- Target resolution never returns a native handle or process identifier.
- Disconnected clients lose pending input state and command replies without
  affecting sessions or other client queues.
- Destructive default bindings require explicit confirmation.

## Testing Strategy

Every behavior is implemented red-green-refactor. Focused tests cover:

- lexer goldens for quoting, escaping, empty arguments, comments, semicolon
  chains, Unicode, limits, and precise error spans;
- command aliases, unique prefixes, argv preservation, atomic lists, and all
  new command arguments;
- session/window/winlink/pane/client targets, current/last/relative targets,
  ambiguity, absence, qualification, and stable IDs;
- wire-key round trips and invalid protocol v6 payloads;
- Windows console normalization for characters, modifiers, arrows, function
  keys, UTF-8, and exact raw bytes;
- root/prefix/copy tables, repeat expiry, send-prefix, literal passthrough,
  paste bypass, mouse precedence, confirmation, and per-client isolation;
- bind/unbind/list/send/switch/refresh execution through the serialized queue;
- round-robin queue fairness, bounded lists, malformed-list atomicity, and
  client disconnect during queued work;
- no-allocation/no-clone hot-path assertions where observable;
- full workspace, conformance, fuzz compilation, release performance, format,
  and clippy gates.

## Acceptance Criteria

Phase 4 is complete when:

1. every command source converges on one parser and produces `CommandList`;
2. every command target is resolved through `TargetResolver` into stable IDs;
3. no attached client contains mux binding or prefix policy;
4. normalized keys plus original bytes cross protocol v6;
5. root, prefix, and copy-mode bindings are server-owned and client-isolated;
6. all required Phase 4 commands execute through the serialized fair queue;
7. destructive default bindings use functional confirmation;
8. malformed commands, keys, and targets cannot panic or partially execute;
9. the measured key-routing path satisfies the new performance gate and all
   earlier performance thresholds remain green;
10. formatting, clippy with warnings denied, all workspace tests,
    conformance, fuzz build checks, and a clean tracked worktree pass on
    `main`.
