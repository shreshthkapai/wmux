# Command, Target, and Key Model

Phase 4 moves multiplexer policy into OS-neutral, server-owned core. Clients
normalize terminal input and retain its original bytes, but they do not parse
commands, interpret the prefix, resolve targets, or mutate mux state. The
serialized server owner remains the only state mutator.

## Command grammar

`parse_command_text` and `parse_command_argv` converge on the same command-name
table, argument validators, target parser, and immutable `CommandList` type.
The argv path preserves already-separated arguments; it never reconstructs
shell text.

The text grammar supports whitespace and line separators, single and double
quotes (including empty arguments), unquoted semicolon command chains, and
comments introduced by `#` at the beginning of a token. Backslash escapes
include an escaped following character, `\a`, `\b`, `\e`, `\f`, `\n`, `\r`,
`\s`, `\t`, `\v`, `\uNNNN`, and `\UNNNNNNNN`. Exact aliases and unambiguous
command-name prefixes use the same lookup table. Syntax errors retain byte,
line, and column information.

Parsing is bounded to 1 MiB of input, 4,096 tokens, 256 commands, and 64 KiB
per token. Diagnostics are bounded, and a list is validated atomically before
anything enters the command queue. Environment/tilde expansion, formats,
conditionals, and brace blocks remain intentionally deferred to their roadmap
phases.

## Targets

Commands parse targets once as `TargetSpec` and resolve them immediately before
execution through `TargetResolver`. This lets later commands observe earlier
state changes while ensuring executors operate on stable IDs rather than names,
indexes, or platform handles.

The supported target forms are:

- omitted, empty, `@`, or `{current}` for the invoking client's context;
- `$N`, `@N`, and `%N` for stable session, window, and pane IDs;
- exact session/window names, followed by unique-prefix matching;
- `session:window` and `session:window.pane` qualified paths;
- numeric winlink indexes and pane positions;
- wrapping `+N` and `-N` relative winlinks;
- `!` or `{last}` for the relevant previous session, winlink, or pane;
- a numeric client ID or `{current}` for client targets.

Resolution returns distinct invalid, absent, and ambiguous errors. A target is
resolved for the type declared by its command, so a pane-qualified string
cannot silently satisfy a session command. Resolved window targets preserve
their session-specific winlink identity.

## Semantic keys and protocol v6

`KeyCode` contains a `BareKey` and compact Shift, Alt, Control, and Super bits.
It covers Unicode characters, navigation and editing keys, Enter, Tab,
Backspace, Escape, Space, and F1 through F24. Names use tmux-shaped forms such
as `C-b`, `M-Left`, `S-Tab`, `Enter`, and `F12`. ASCII uppercase normalizes to
lowercase plus Shift.

IPC version 6 (`WMX6`) carries a fixed semantic key header followed by the
original terminal bytes. A multibyte character or escape sequence is one key
message. Unbound keys move their existing byte vector to the active pane;
paste remains a separate semantic message and bypasses key tables.

For a character event whose raw payload is exactly one printable ASCII glyph,
the server uses that produced glyph as the binding identity and discards
redundant Shift and Control state. Alt, Super, multibyte characters, control
bytes, and escape sequences retain their semantic identity. The original raw
payload is never rewritten, so unbound passthrough remains byte-exact. This
matches tmux's legacy printable-key behavior while tolerating layout-dependent
Windows console modifier reporting.

## Tables, routing, and confirmation

The server owns sorted compact binding tables with binary-search lookup.
Phase 4 provides `root`, `prefix`, and `copy-mode` tables, a `C-b` prefix, and
a 500 ms repeat window. Like tmux with its default `prefix-timeout` of zero,
the prefix table waits indefinitely for the next key; the repeat window applies
only after a repeatable binding. `bind-key`, `unbind-key`, and `list-keys`
mutate or inspect those server-owned tables. Binding command lists are shared
by `Arc`, so dispatch does not reparse command text.

Routing order is confirmation, copy mode, prefix transition, active-table
binding, then unbound passthrough. Repeatable bindings retain prefix state until
their deadline. Client input state is independent, while tables are shared by
the server. Paste and existing mouse-mode precedence remain outside ordinary
key-table routing.

The default `C-b x`, `C-b &`, and `C-b X` bindings use `confirm-before` for
pane, window, and current-session destruction respectively. Lowercase `x`
therefore keeps tmux's pane behavior, while uppercase `X` is wmux's explicit
quality-of-life escalation for ending the entire attached session. A
confirmation is stored per client and rendered over the normal status row with
the cursor hidden. Accepting enqueues the already-parsed command list;
rejecting clears it. Neither path writes prompt text into the authoritative
pane grid.

## Queue and command effects

Every source submits a bounded `CommandList` to one fair command queue. Each
client keeps FIFO order, and ready clients rotate round-robin one command at a
time, preventing a 256-command list from monopolizing the server owner. An
error stops only the rest of that invocation. Binding invocations follow the
same ordering but do not require a command reply.

Core execution returns typed effects for pane creation/input, copy mode,
client refresh, confirmation, detach, and shutdown. The server applies those
effects in order. This keeps target resolution and mux semantics testable
without allowing core code to access a PTY, native handle, clock, lock, or IO.

The completed command surface includes `bind-key`, `unbind-key`, `list-keys`,
`send-keys`, `send-prefix`, `switch-client`, `refresh-client`, and
`confirm-before`, alongside the existing session/window/pane commands.

## Performance evidence

The 2026-08-21 Windows release measurements used the checked-in deterministic
full workloads:

| Workload | Fixed work | Measured throughput | Gate | Measured allocations |
| --- | ---: | ---: | ---: | ---: |
| `key-unbound` | 10,000,000 routes | 139,753,419 routes/s | 15,000,000 routes/s | 0 |
| `key-prefix-binding` | 5,000,000 prefix/binding pairs | 52,068,364 pairs/s | 5,000,000 pairs/s | 0 |
| `command-queue` | 1,000,000 pre-parsed commands, 8 clients | 10,414,226 commands/s | 2,000,000 commands/s | not gated |
| `command-text` | 250,000 four-command lists | 632,065 lists/s | 200,000 lists/s | not gated |

The full release gate also retains every earlier parser, renderer, resize,
backpressure, and memory threshold. Workloads checksum their semantic result
and final queue state so the optimizer cannot remove the measured work.

No equivalent standalone command-parser or normalized key-routing
microbenchmark was exposed by either local reference tree, so no synthetic
cross-project speed ratio is claimed. The inspected reference revisions were
tmux `7b833d07d9f1b58343fc88d7de3c2e0bd9f9aa8c` and zellij
`82c4a24d701ecf9a48aa01bcc5c0bb3882747fe7`. The comparison is architectural:
wmux follows tmux's server-owned key/command semantics and zellij's normalized
key plus raw-byte transport, while the throughput table above measures wmux
only.

## Malformed-input and conformance evidence

Fixed-seed bounded tests feed arbitrary lossy UTF-8 through command, target,
and key parsers and arbitrary bytes through protocol-v6 key decoding. They
assert no panic, bounded diagnostics, and no partial state mutation. The
`command_text` libFuzzer target caps inputs at 1 MiB and walks every successfully
parsed command; its checked-in corpus includes quoted chains and malformed
escapes.

The portable conformance suite now has 13 cases. Its command cases exercise
parsing, qualified target resolution, binding mutation, prefix and repeat
routing, exact send-key bytes, session switching, client refresh, and both
confirmation decisions. The accepted fingerprint is
`00b763c726b9d162`.

The Phase 4 exit run executes formatting, clippy with warnings denied, all 274
workspace tests, fuzz-target compilation and clippy, release conformance, the
complete release performance gate, whitespace validation, native-import audit,
and client-policy audit. The two source audits produce no matches.
