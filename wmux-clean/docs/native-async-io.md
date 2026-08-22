# Native Asynchronous I/O

The Windows runtime uses Tokio's IOCP-backed named-pipe implementation. The
Unix runtime uses Tokio readiness around nonblocking AF_UNIX sockets and PTY
masters, mapping to epoll on Linux and kqueue on macOS. Async tasks own
transport progress only. They emit semantic events to the single state owner
and never mutate sessions, panes, grids, layouts, or client state.

## Client IPC

The server creates overlapped duplex named-pipe instances and awaits
`ConnectNamedPipe`, reads, and writes through IOCP. A replacement listener is
created immediately after each accepted connection. Each connected client has
one async task selecting between inbound frames and its bounded outbound queue.

The attached client uses a dedicated inbound IPC reader. It owns an in-progress
frame until the complete header and payload have been decoded, then forwards a
complete message to the input/output loop. Keyboard, paste, and resize events
must never cancel a partial `read_exact`; cancellation would discard bytes and
desynchronize the framed stream under key repeat.

Attached clients open an overlapped client handle and await server output,
console-input messages, and resize verification concurrently. Detached command
clients retain the synchronous protocol path because they perform one bounded
request/response transaction and do not participate in the interactive hot
path. Both paths use the same versioned wire framing.

Once attached, `CommandOk` and `CommandErr` are in-band completions for prefix
commands and do not change client lifecycle state. Only explicit shutdown,
detach, or transport EOF ends the attach loop. This matches tmux's separation
of command completion from `MSG_EXIT` and zellij's dedicated
`ServerToClientMsg::Exit` lifecycle message.

## ConPTY Channels

`CreatePseudoConsole` requires the handles passed as `hInput` and `hOutput` to
use synchronous I/O. wmux follows zellij's split-handle model for output:

```text
ConPTY synchronous output handle
  -> byte-mode named pipe
  -> wmux overlapped read handle
  -> Tokio IOCP reader
  -> bounded per-pane PlatformEvent queue
  -> fair state-owner parser scheduling
```

ConPTY input remains a synchronous pipe as required by Windows. A dedicated
per-pane writer worker owns that channel so a blocked ConPTY write cannot block
the state owner or an IOCP worker. Input messages remain ordered.

## Unix PTY Channels

The Unix adapter allocates a PTY at the requested initial size, gives the slave
to a child in its own session and process group, and retains only the
nonblocking master. `AsyncFd` drives both readable and writable readiness:

```text
nonblocking PTY master
  -> epoll (Linux) / kqueue (macOS)
  -> reused 16 KiB read buffer
  -> bounded 64-entry PlatformEvent queue
  -> fair state-owner parser scheduling
```

Pane input enters one bounded ordered queue. Its writer retries `EINTR`, clears
readiness on `EAGAIN`, and completes every short write before advancing to the
next input message. Resize requests apply `TIOCSWINSZ` to the PTY; the server
continues to derive the requested size from authoritative layout cells.

PTY EOF and child exit are independent native observations. The backend drains
queued output before emitting at most one exit and exactly one close. If EOF
arrives without a child status, it emits one unknown exit first. Pane cleanup
signals the complete process group, and backend shutdown force-kills and reaps
every remaining pane tree.

The Unix terminal frontend owns a restoration guard containing the exact saved
termios state. Entering attach mode applies raw input, while normal return,
detach, error propagation, and unwinding all restore the saved state through
the same drop path. Rendering remains one synchronized-output transaction and
clipboard writes remain one bounded OSC 52 transaction.

## Backpressure And Shutdown

Each pane has 64 queued 16 KiB output chunks. When full, the async read task
waits for capacity without consuming another chunk or occupying an IOCP worker.
The server preserves every VT byte.

Pane shutdown first signals the output task to enter drain-only mode, then
terminates the Job Object and closes the pseudoconsole. Drain-only mode keeps
reading final ConPTY output without waiting on the bounded semantic queue. This
prevents `ClosePseudoConsole` from deadlocking against a full output pipe.

The state owner blocks until an I/O notification or the next render,
synchronized-output, or resize deadline. There is no fixed I/O polling tick.

## Cross-OS Boundary

IOCP and named-pipe types remain inside `wmux-windows` and `wmux-server` I/O
coordination. Unix descriptors, termios, credentials, process groups, and
signals remain inside `wmux-unix`. Core events and platform IDs remain OS
neutral; both adapters implement the same ordering and backpressure contract.

## References

- zellij `zellij-server/src/os_input_output_windows.rs`
- tmux `tty.c`, `server-client.c`, and libevent buffer readiness handling
- Microsoft `CreatePseudoConsole` documentation
- Microsoft synchronous and overlapped named-pipe I/O documentation
- Microsoft I/O completion port documentation
- Linux `unix(7)`, `pty(7)`, `epoll(7)`, and `credentials(7)` documentation
- Apple `getpeereid(3)`, `openpty(3)`, `termios(4)`, and `kqueue(2)` documentation
