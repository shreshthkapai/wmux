# Testing Strategy

wmux testing is split into fast deterministic tests, Windows integration
scripts, golden/snapshot checks, property-style invariant tests, stress tests,
and soak tests. The goal is to catch correctness bugs early without pretending a
single unit-test run proves terminal-multiplexer stability.

## Test Tiers

### Unit Tests

Unit tests cover pure logic and must be fast enough to run on every local build:

```powershell
ctest --test-dir build-vs -C Debug --output-on-failure
```

Current unit-test areas include:

- command parser and runtime command mapping
- target resolver
- attach keymap and input mode transitions
- terminal input decoder
- mouse parser
- command prompt parser
- config parser and validation
- IPC framing and payload parsing
- session/window/pane model
- layout tree operations
- terminal VT parser
- terminal screen grid mutation
- Unicode width and cell handling
- copy selection extraction
- paste buffer limits
- status line rendering
- platform info and terminal cleanup helpers

Unit tests must not spawn long-running shells, assume a specific user terminal,
or depend on an existing daemon.

### Integration Tests

Integration tests exercise Windows behavior that cannot be proven by pure unit
tests. They are PowerShell scripts under `scripts/` and should be run from a
native Windows terminal, not WSL.

Important scripts:

- `scripts\test-attach-lifecycle.ps1`
- `scripts\test-detach-reattach.ps1`
- `scripts\test-window-switching.ps1`
- `scripts\test-pane-focus.ps1`
- `scripts\test-command-mode.ps1`
- `scripts\test-mouse-focus.ps1`
- `scripts\test-process-lifecycle.ps1`
- `scripts\test-process-cleanup.ps1`
- `scripts\test-daemon-recovery.ps1`
- `scripts\test-resize-stress.ps1`
- `scripts\test-render-throughput.ps1`

These scripts are allowed to start the daemon, spawn ConPTY shells, send attach
frames, resize panes, and verify daemon-visible state. They expect to own an
empty daemon unless the script says otherwise.

### Golden Tests

Golden tests compare a stable input sequence against an expected state snapshot.
They are preferred for parser, grid, renderer, command, and layout behavior
where regressions are subtle.

Current golden-style coverage includes:

- VT sequence to screen-grid snapshots
- PowerShell and cmd prompt snapshots
- git diff color snapshots
- alternate-screen snapshots
- progress-bar carriage-return snapshots
- renderer frame assertions
- input bytes to decoded key events
- command text to runtime command objects
- layout tree to pane rectangles

When a terminal behavior changes intentionally, update the golden expectation
and explain the semantic change in the commit or PR.

### Property-Style Tests

Property tests exercise many generated operation sequences and assert invariants
after each step. The current implementation uses deterministic pseudo-random
sequences inside the C++ test executable rather than a third-party property
testing dependency.

Required layout invariants:

- every pane appears exactly once in the tree
- every split has at least two children
- weights match children and are positive finite values
- same-axis splits are flattened
- active pane remains valid
- computed rectangles fit inside the terminal
- computed rectangles cover the layout area without overlap
- pane count matches leaf count

The deterministic seed must stay fixed unless the test is deliberately being
expanded; reproducibility matters more than random novelty.

### Stress Tests

Stress tests apply bounded pressure and look for responsiveness, cleanup, and
resource-growth regressions:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-stress-suite.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The stress suite covers create/kill loops, attach/detach, window
create/switch loops, split/kill loops, daemon restart, high output, resize
storms, copy/paste, malformed IPC, shell spawn failure, mouse event pressure,
and Unicode output pressure.

Focused sections can be run with `-Only`, for example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-stress-suite.ps1 -Wmux .\build-vs\Debug\wmux.exe -Only UnicodeOutput
```

### Soak Tests

Soak tests run repeated cycles while sampling resources:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-soak.ps1 -Wmux .\build-vs\Debug\wmux.exe -DurationSeconds 3600
```

Track memory, CPU, handle count, process count, event/render queue depth,
render latency, dropped/coalesced frames, logs, and orphan processes. A soak
failure is a product bug unless the failure is clearly environmental.

## Local Validation Policy

During active development:

1. Run `ctest` after focused code changes.
2. Run the quick release gate every few related steps or after touching attach,
   IPC, layout, rendering, process lifecycle, terminal cleanup, config, or input.
3. Run the smallest integration script that covers the changed subsystem.
4. Run the full release gate before claiming a build is stable or broadly
   testable.

Quick gate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe -Quick
```

Full gate:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-release-gate.ps1 -Wmux .\build-vs\Debug\wmux.exe
```

The quick gate is for development feedback. It is not release evidence.

## Regression Policy

Every bug fix should add or tighten the smallest test that would have caught the
bug:

- pure parser/state bug: unit test
- terminal sequence bug: golden VT/grid test
- layout mutation bug: unit or property-style layout test
- attach/daemon/process bug: integration script
- performance/resource bug: stress or soak assertion
- cleanup bug: release-gate or targeted lifecycle script

Do not lower limits, skip failing scripts, or weaken assertions without
documenting why the old expectation was wrong.

## Manual Tests

Some behavior still requires manual verification because host terminals differ:

- actual keyboard delivery in Windows Terminal, VSCode terminal, WezTerm, and
  Alacritty
- mouse click, drag, and wheel behavior in each host terminal
- Ctrl+C and Ctrl+Break cleanup behavior
- closing a terminal tab while attached
- copy/paste into external Windows applications
- real full-screen TUI apps such as editors and pagers

Manual findings should be turned into automated tests whenever the behavior can
be reproduced through daemon IPC or a stable terminal sequence.
