# Stability Testing

wmux treats stability and predictable performance as release criteria. The scripts in
`scripts/` are intended to catch lifecycle, IPC, rendering, resize, and resource-growth
regressions before wider testing.

For the full test-tier contract, see [Testing Strategy](testing-strategy.md).

## Release Gate

The release gate is the required check before calling a build stable:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

Use the quick profile only while developing the gate or checking script wiring:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe -Quick
```

See [Release Gate](release-gate.md) for the pass/fail criteria, required
artifacts, and known limitations.

## Stress Suite

Run the bounded stress suite from a Windows PowerShell prompt:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-stress-suite.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The default bounded matrix exercises repeated session create/kill, attach/detach,
window creation/switching, pane split/kill, daemon stop/restart, malformed IPC,
high-output rendering, resize storms, copy/paste, mouse event pressure, Unicode
output, and shell-spawn failure handling. It expects to own an empty daemon and
uses `cmd.exe /D /Q` as the default shell except during the explicit spawn-failure
section.

Run one matrix section while iterating:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-stress-suite.ps1 `
  -Wmux .\build-vs\Debug\wmux.exe `
  -Only MouseFlood
```

Useful `-Only` values are:

```text
CreateKill
AttachDetach
WindowSwitch
PaneSplitKill
OutputResize
CopyPaste
MouseFlood
UnicodeOutput
ShellSpawnFailure
DaemonIpc
```

For a heavier local run, increase the bounded counters explicitly:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-stress-suite.ps1 `
  -Wmux .\build-vs\Debug\wmux.exe `
  -SessionIterations 100 `
  -PaneIterations 100 `
  -AttachLoops 100 `
  -WindowIterations 25 `
  -HighOutputLines 10000 `
  -ResizeIterations 500 `
  -MouseEventIterations 2000 `
  -UnicodeLines 5000 `
  -CopyPasteLoops 25
```

## Soak Test

Run a longer resource-tracking soak:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-soak.ps1 -Wmux .\build-vs\Debug\wmux.exe -DurationSeconds 3600
```

The soak runner repeats smaller stress cycles against a long-lived daemon and writes CSV
resource samples plus per-cycle logs under `artifacts/soak/`. Track working set, private
memory, handles, thread count, child process count, session count, shell count, attach
workers, render frames, render bytes, and dropped output bytes. A production candidate
should show no unbounded growth across repeated cycles.
