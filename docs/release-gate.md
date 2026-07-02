# Release Gate

wmux must not be called stable until the release gate passes on native Windows
with the target release build. The gate is intentionally stricter than a normal
unit-test run because wmux owns long-lived processes, terminal state, named
pipes, ConPTY handles, output buffers, and interactive attach clients.

## Not Stable Until

- Stress tests leave no orphan `powershell.exe`, `pwsh.exe`, `cmd.exe`, or
  daemon-owned `wmux.exe` workers.
- Memory stays bounded under sustained high output, repeated attach/detach,
  pane splits, resize storms, copy/paste, and daemon restart cycles.
- Terminal state is restored after explicit detach, client disconnect, pipe
  failure, Ctrl+C/Ctrl+Break paths, and daemon shutdown.
- Attach/detach works repeatedly without stale clients, blocked pipes, or lost
  daemon session ownership.
- Panes and windows survive long-running workloads without corrupting focus,
  layout, buffers, or process ownership.
- Logs identify session/window/pane/client IDs and explain ConPTY, IPC,
  lifecycle, and cleanup failures clearly enough to debug without reproducing
  from scratch.
- Known limitations are documented in this file and in the stability testing
  notes before wider testing starts.

## Release Criteria

Run the full gate from a Windows PowerShell prompt:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

For a fast local smoke check while developing:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe -Quick
```

The full gate must pass before tagging a stability-focused release. The quick
gate is useful for validating the wiring, but it is not release evidence.

The gate currently runs:

- build validation
- unit tests through `ctest` and `wmux_tests.exe`
- attach lifecycle checks
- detach/reattach persistence checks
- daemon recovery checks
- process cleanup and orphan-shell checks, including create/kill-session,
  interactive pane kill, interactive window kill, and `server stop --force`
- resize stress
- render throughput pressure
- bounded stress suite
- resource-tracking soak
- known-limitation documentation checks

The default soak duration is one hour. Increase it for release candidates that
are expected to be used for full-day or multi-day workflows:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 `
  -Wmux .\build-vs\Release\wmux.exe `
  -BuildDir .\build-vs `
  -Config Release `
  -SoakDurationSeconds 28800
```

## Required Evidence

Keep the gate output and generated artifacts for each release candidate:

- `artifacts/release-gate/<timestamp>/soak/resource-samples.csv`
- stress and soak console logs
- `wmux server status` output from failed runs
- daemon/client logs from the failure window

For a candidate to pass, the resource sample CSV must show bounded memory,
handle count, thread count, child process count, live session count, live shell
count, attach worker count, render frame count, render byte count, and dropped
output count. Monotonic growth in memory, handles, threads, child processes, or
live workers is a release blocker.

Cleanup evidence must include both daemon counters and OS process checks. A path
is not considered clean if `wmux server status` reports zero live shells but the
old daemon process still has descendant `cmd.exe`, `powershell.exe`, `pwsh.exe`,
or ConPTY support processes.

## Known Limitations

Current hardening work is good enough for controlled testing, not for claiming
tmux-level maturity yet. The known limitations are:

- Terminal emulation coverage is still smaller than mature terminal
  multiplexers. The grid handles core cursor, erase, wrapping, scroll-region,
  color, alternate-screen, UTF-8, wide-cell, and combining-cell behavior, but it
  still needs broader xterm compatibility tests.
- Unicode width handling covers wide cells, combining marks, emoji modifiers,
  ZWJ emoji sequences, and regional-indicator flag pairs defensively, but it is
  not full Unicode-property database parity yet.
- Integration scripts share the singleton daemon namespace. Run attach-heavy
  scripts sequentially unless isolated daemon namespacing is added.
- Render optimization has throttling, pane-level partial frames, bounded client
  output queues, and slow-client accounting, but it is not yet a full
  dirty-region renderer.
- Copy mode handles scrollback/live-grid selection and wrapped-line extraction,
  but more real TUI/editor/pager scenarios need golden tests.
- Stress and soak scripts prove bounded behavior for their configured load.
  They do not replace multi-day manual use with real development workloads.

These limitations do not block continued development, but they block calling the
project stable until testing shows they are acceptable for the target release.

## Failure Policy

Any release-gate failure is treated as a product bug unless the failure is caused
by a documented external dependency outage or local environment issue. Do not
work around failures by lowering limits without recording why the original limit
was wrong.

Before retesting a failure:

1. Save the failing artifacts.
2. Stop the daemon with `wmux server stop --force`.
3. Check for orphan shell or daemon processes.
4. Re-run the smallest failing script directly.
5. Add or tighten a regression test before marking the issue fixed.
