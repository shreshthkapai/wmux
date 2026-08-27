# OS-Neutral Runtime Contract

The server runtime has one state-owning event loop. Concurrent producers
may create events, but they may not mutate sessions, windows, panes, grids,
layouts, clients, or command queues directly.

`wmux_core::ServerEvent` is the input contract for that loop:

```text
PtyOutput
PtyExited
ClientInput
ClientMouse
ClientResize
ClientWritable
Command
Timer
```

Core events contain only stable wmux IDs, byte buffers, normalized cell-based
mouse events, commands, dimensions, exit status, and timer IDs. They contain no Windows handles, Unix file
descriptors, ConPTY objects, PTY objects, named pipes, sockets, or async runtime
types.

`ClientMouse` remains structured until the state owner has resolved the target
pane. The pane screen's DEC mouse modes then decide whether the event is
encoded for the PTY or handled as multiplexer scrollback. Native mouse records
and terminal escape sequences never enter core state.

Copy completion travels in the opposite direction as the protocol-level
`Clipboard` semantic message. Platform clipboard handles never enter the core
or server event contract.

`wmux_platform::PlatformRequest` is the output contract from the runtime to a
platform backend:

```text
SpawnPane
WritePane
ResizePane
TerminatePane
```

`SpawnPane.cwd` is client-scoped launch context. A disposable client sends its
absolute invocation directory during the authenticated handshake; commands
that create panes preserve that directory through the serialized owner loop.
The native adapter applies it when creating the child process. The persistent
server's own working directory is not part of pane semantics.

`wmux_platform::PlatformEvent` carries PTY output, process exit, final stream
closure, and classified backend errors back from a backend. `PlatformPaneId`
is a stable semantic token, never a raw OS handle. `PtyExited` and `PtyClosed`
are deliberately separate: final output may arrive after process exit, while
closure is final and no event may follow it.

Windows maps the contract to ConPTY, Job Objects, and named pipes. Linux and
macOS map it to PTYs, process groups, Unix sockets, and terminal modes. Backend
implementation choices must not change core event ordering or multiplexer
semantics.

## Live Ownership Model

`wmux-server` runs one `wmux-state-owner` thread. It exclusively owns
`ServerState`, `CommandQueue`, pane terminal engines, client render baselines,
layout transactions, and platform-pane coordination. IOCP connection tasks
decode protocol messages into `ServerEvent` values, consume bounded outbound
queues, and report `ClientWritable` events after completed writes. These I/O
tasks cannot access server state.

`ClientWritable` releases only outbound byte accounting. It does not certify a
physical terminal update and cannot unlock another render. A validated protocol
v9 `OutputAck` becomes the server-internal `OutputPresented` owner message; only
that matching sequence opens the per-client presentation gate. Neither event
mutates core state outside the state-owner loop.

ConPTY IOCP readers emit `PlatformEvent` values into a bounded queue owned by
their pane and notify the owner when data becomes ready. The owner converts
those semantic events into terminal-engine mutations even with zero attached
clients, so detach never pauses the authoritative pane screen. Idle owner waits
are event or deadline driven; no fixed one-millisecond I/O poll remains.

The Windows `PtyBackend` owns each `ConptyPane`, event receiver, raw handle,
IOCP registration, input worker, shutdown signal, Job Object, and
pseudoconsole. The server submits only semantic requests and polls semantic
events; no native controller or receiver enters the shared server library,
`wmux-core`, or the authoritative state model.

## Fair PTY Scheduling

Each Windows pane has a bounded queue of 64 output events. ConPTY reads are
16 KiB, so unread output is bounded to approximately 1 MiB per pane plus event
metadata. Queue overflow asynchronously suspends only that pane's IOCP reader.
It does not block an I/O worker thread. Bytes are never dropped because
dropping part of a VT stream would corrupt terminal state.

The owner services panes in round-robin order. One pane may contribute at most
64 KiB or one millisecond of collection work per turn, with a four millisecond
output-round budget. Adjacent chunks are coalesced and passed to the parser in
one call. Parser work is bounded by the byte budget; a single parser call is
not preempted. Up to 256 control events are handled before each output round,
and the next loop returns to control events after that round. This keeps input,
commands, resize, and client-writable notifications responsive during sustained
pane output.

Each coalesced parser call produces one semantic terminal-operation batch. The
state owner applies it to the pane grid and commits one pane generation plus
line generations and a bounded damage-journal entry. Clients retain independent
consumed-generation maps; rendering one client never clears damage for another.
