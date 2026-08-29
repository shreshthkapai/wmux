# Known Cross-Platform Differences

Every observed discrepancy must be classified as a shared bug, native backend
bug, intentional platform difference, or unsupported capability. An
unexplained divergence fails the release-quality gate. The portable
`EXPECTED_DIFFERENCES` registry is currently empty.

## Shared and native backend bugs

No open semantic difference is registered for the current release contract.
Fixed regressions remain protected by focused tests, including final output
after a resize hold, attach-worker cleanup, native input punctuation, mouse
event ordering, terminal-mode restoration, and endpoint restart.

## Intentional platform differences

- Windows uses ConPTY, `CreateProcessW`, Job Objects, named pipes, and SID peer
  identity. Unix uses PTYs, process groups or sessions, AF_UNIX sockets, and
  native UID peer identity (`SO_PEERCRED` on Linux and `getpeereid` on macOS).
- Windows cleanup targets an owned Job Object. Unix cleanup targets the owned
  process group and closes the PTY. These mechanisms implement the same wmux
  process-tree and terminal-event contract without pretending that Windows has
  POSIX signals, file-descriptor passing, or shell job control.
- An uncatchable external kill, including Unix `SIGKILL` and Windows
  `TerminateProcess`, cannot execute in-process terminal restoration. wmux
  restores modes whenever the client regains control; recovery from an
  uncatchable kill belongs to the enclosing shell or terminal host.
- Release performance thresholds execute on Windows, Linux, and macOS, but raw
  timings from different runner hardware are not treated as direct cross-host
  speed comparisons.

## Unsupported capabilities

No unsupported capability is registered inside the negotiated VT and input
model. Interactive host and shell combinations without a recorded manual pass
are `manual-pending`, not unsupported. Byte-for-byte Unix mechanics for
signals, process groups, shell job control, or descriptor passing on native
Windows are outside the product promise and are intentional platform
differences.
