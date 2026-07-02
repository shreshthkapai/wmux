# Engineering Principles

## Priority

For `wmux`, performance and stability are product requirements.

```text
stability and speed >>> everything else
```

The interactive experience must not randomly lag, stall, leak resources, grow
without bounds, or crash under normal terminal-heavy workflows.

## Core Rule

The UI path must never wait on slow work.

Do not block interactive input or rendering on:

- process IO
- disk logging
- slow IPC handlers
- config loading
- expensive parsing
- unrelated daemon state mutation
- synchronous cleanup of large buffers

## Ownership

Every resource must have one clear owner.

Use RAII for all Windows resources:

- handles
- pipes
- process handles
- thread handles
- ConPTY handles
- job objects
- named pipe connections

Avoid raw owning pointers. Prefer value types, smart pointers with clear
ownership, and small wrapper types such as `unique_handle`.

Runtime ownership should be keyed by stable IDs, not mutable display names.
Session, window, pane, and client names are lookup metadata. Renaming a visible
object must not move or recreate process ownership.

## Concurrency Model

Use one explicit concurrency model.

Recommended shape:

```text
daemon event loop
  owns session/window/pane state

IO workers
  read ConPTY pipes
  push bounded events to daemon

client stream handling
  forwards input, mouse, resize, and render events

state mutation
  serialized through the daemon event loop
```

Avoid arbitrary shared mutation from many threads. If state can be changed by
multiple threads, the design should be reconsidered.

## Bounded Memory

Unbounded growth is a bug.

Bound these from the beginning:

- pane output queues
- scrollback rings
- render event queues
- IPC message sizes
- log files
- pending client input
- terminal parser buffers
- detached-session backlog

When overloaded, behavior should be explicit:

- coalesce render updates
- drop stale intermediate frames
- cap scrollback
- reject oversized IPC messages
- preserve current interactivity over perfect historical replay

## Rendering

Rendering should be frame-based and coalesced.

Do not repaint the visible terminal for every byte read from a pane. Pane output
should update terminal state, mark dirty regions, and produce render frames at a
bounded rate.

The render path should be allocation-light and should avoid parsing JSON or
performing disk IO.

## Terminal Parsing

Terminal parsing must be incremental, bounded, and heavily tested.

Parser requirements:

- tolerate malformed or incomplete escape sequences
- avoid unbounded accumulation
- maintain a pane-local grid
- support dirty-region tracking
- handle common shell and TUI behavior
- support alternate screen buffers

Terminal parsing is a correctness and performance-critical subsystem.

## Logging

Logging must not cause lag.

Use structured logging with:

- severity levels
- rotating files
- bounded queues
- trace logging disabled by default
- enough context to debug daemon crashes and IPC failures

Never let heavy pane output or render loops synchronously block on logging.

## Failure Behavior

Expected behavior:

- If a client exits, the daemon and pane processes continue.
- If a client crashes, the daemon and pane processes continue.
- If the terminal window closes, the session remains attachable.
- If the daemon exits normally, it should clean up owned child processes unless
  a future design explicitly supports daemon recovery.
- If the daemon crashes, v1 may lose pane processes, but crashes should be rare
  and diagnosable.

`server stop` should have explicit behavior. It should either refuse when live
sessions exist or require a force option.

## Command IPC

Phase 2 command IPC is intentionally small and conservative:

- Keep each request and response bounded.
- Use one request per connection for the initial daemon skeleton.
- Treat daemon startup and shutdown as explicit lifecycle paths.
- Do not let stale development sockets replace or disturb a live daemon.
- Keep JSON inside validated command frames only; do not use per-keystroke JSON
  for attach streaming.

## Attach Streaming IPC

Attach streaming must keep shell data and lifecycle/control data separate.

- Use a dedicated attach transport endpoint for long-lived interactive streams.
- Use bounded framed messages for client input, detach, resize, mouse, render
  output, and stream errors.
- Treat intentional detach differently from broken pipe, malformed protocol,
  session kill, and server shutdown.
- Track active clients by stable client ID so cleanup paths can disconnect them
  deliberately.
- Keep detach/reattach covered by an integration test that exercises the real
  daemon and attach pipe, including abrupt pipe closure to simulate terminal
  exit.
- Do not parse JSON per keystroke.

## Stability Gates

Each implementation phase should include local validation. Early gates should
include:

- repeated session create/kill cycles without leaked shell processes
- 100 attach/detach cycles without handle leaks
- client crash does not kill daemon
- terminal state restores after client exit
- memory stays bounded during continuous output
- rapid resize does not corrupt layout
- malformed VT input does not crash parser
- many panes do not cause visible input lag

## Testing Strategy

The detailed testing contract lives in [Testing Strategy](testing-strategy.md).
This section summarizes the engineering intent.

Use fast unit tests for pure logic:

- command parsing
- session manager
- window manager
- pane tree operations
- layout rectangle calculation
- ring buffers
- terminal parser

Use integration tests for Windows behavior:

- ConPTY process spawn
- session kill cleanup with no leaked daemon-owned shell processes
- daemon window create/list/rename commands against live session state
- input/output round trips
- resizing pseudo consoles
- attach/detach
- daemon command IPC
- client crash handling

Use stress tests for real-world pressure:

- repeated session create/kill
- huge output
- long-running processes
- many panes
- rapid attach/detach
- rapid terminal resize
- copy mode over large scrollback

## Code Review Standard

Before merging interactive-path code, check:

- Can this block input or rendering?
- Can this allocate per byte or per cell?
- Can this grow without a hard limit?
- Which component owns this resource?
- What happens if the client disconnects here?
- What happens if the child process exits here?
- What happens if the IPC message is malformed?
- How is this tested?
