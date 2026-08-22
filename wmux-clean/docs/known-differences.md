# Known Cross-Platform Differences

Every observed discrepancy must be assigned to exactly one of the four classes
below. An unexplained divergence is a failed beta-core gate. The portable
`EXPECTED_DIFFERENCES` registry is empty, and Windows and Linux currently
produce the same conformance and stress fingerprints.

## Shared bug

No shared bug is open in the Phase 8 evidence run.

Phase 8 found and fixed two shared defects before recording the gate:

- pane close after a resize hold could omit deferred final output; the server
  now publishes the final authoritative frame before close;
- an attach I/O error could leave the client input worker alive; an attach
  guard now stops it and releases the terminal guard exactly once.

## Native backend bug

No native backend bug is open in the Phase 8 evidence run.

One build-boundary defect was fixed: the Windows adapter crate is now gated at
its crate root, matching the Unix adapter, so a full Linux workspace lint does
not attempt to compile Windows APIs.

## Intentional platform difference

- Windows uses ConPTY, `CreateProcessW`, Job Objects, named pipes, and SID peer
  identity. Unix uses PTYs, process groups/sessions, AF_UNIX sockets, and native
  UID peer identity (`SO_PEERCRED` on Linux and `getpeereid` on macOS).
- Windows cleanup targets an owned Job Object. Unix cleanup targets the owned
  process group and closes the PTY. These mechanisms implement the same wmux
  process-tree and terminal-event contract without pretending that Windows has
  POSIX signals, file-descriptor passing, or shell job control.
- An uncatchable external kill, including Unix `SIGKILL` and Windows
  `TerminateProcess`, cannot execute in-process terminal restoration. Wmux
  restores modes on every path where the client regains control; recovery from
  an uncatchable kill belongs to the enclosing shell or terminal host.
- The established comparable performance gate runs on Windows. Linux and
  macOS retain correctness and stress gates; cross-host timing is not treated
  as a regression comparison.

## Unsupported terminal capability

No unsupported capability is known inside the negotiated beta-core VT and
input model. Interactive host/shell combinations are `manual-pending`, not
classified as unsupported. Byte-for-byte Unix tmux behavior for signals,
process groups, shell job control, or descriptor passing on native Windows is
outside the product promise and is classified above as an intentional platform
difference.
