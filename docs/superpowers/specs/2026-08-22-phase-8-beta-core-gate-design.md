# Phase 8 Cross-Platform Beta Core Gate Design

## Status and scope

Phase 8 is the nonvisual beta-core qualification phase. It does not add UI
chrome, themes, status lines, overlays, packaging, or public release assets.
It turns the behavior implemented in Phases 1 through 7 into repeatable release
evidence and fixes any core reliability defect exposed by that evidence.

The gate covers the canonical `wmux-clean` workspace. Source code and developer
test tools remain there. GitHub Actions workflow files must live at repository
root because GitHub does not discover workflows nested under
`wmux-clean/.github`.

## Success criteria

The phase passes only when all of the following have executable evidence:

- repeated attach/detach and abrupt client disconnect preserve sessions;
- server shutdown and restart leave no stale endpoint;
- resize storms converge on the final size;
- pane exit and kill-during-output preserve ordered terminal state;
- one slow client cannot block or exhaust the server;
- many clients and panes retain independent authoritative views;
- 100,000-line history and maximum-size paste paths remain bounded;
- synchronized output and control subscriptions cannot create unbounded queues;
- normal detach, error, panic unwinding, and parent-terminal loss release the
  terminal-mode guard;
- pane, window, session, job, and server termination clean up native process
  trees;
- malformed parser and protocol input receives sanitizer-backed fuzz smoke on
  Unix CI and stable corpus replay everywhere;
- the existing full release performance gate passes without weaker thresholds;
- portable semantics have the same fingerprint on Windows, Linux, and macOS;
- every observed difference is classified and no unexplained divergence is
  accepted.

An uncatchable process kill such as Unix `SIGKILL` or Windows
`TerminateProcess` cannot run in-process cleanup code. Phase 8 does not claim
otherwise. The supported terminal-restoration contract covers every path on
which the client regains control: normal detach, protocol/terminal errors,
panic unwinding, EOF/terminal closure, and catchable termination. An
uncatchable kill is recorded as an operating-system limit, with shell/terminal
reset recovery documented rather than hidden.

## Reference model

### tmux

tmux marks a lost client dead, removes client-scoped state, emits a detach
notification, and leaves sessions and panes owned by the server. Control mode
has an explicit lag policy: output is paused/discarded or a client that is too
far behind exits. Client exit waits for critical command/file output to drain.
The terminal saves its original termios state and restores it during tty close.
Jobs and pane processes are terminated by process identity/group rather than by
client lifetime.

Relevant local sources are `tmux/server-client.c`, `tmux/control.c`,
`tmux/tty.c`, `tmux/job.c`, and `tmux/regress/`.

### zellij

zellij isolates socket writes in a per-client sender with a bounded queue. A
full queue disconnects that client rather than blocking the router or allowing
unbounded memory growth. Its integration suite drives detach/attach through a
fake PTY and verifies that the reattached client receives the authoritative
split and output.

Relevant local sources are `zellij-server/src/os_input_output.rs` and
`zellij-integration-tests/tests/clients.rs`.

### Native contracts

Windows console input and output modes are captured with `GetConsoleMode`,
changed with `SetConsoleMode`, and restored from the captured values. Windows
pane and job trees remain Job Object-owned with
`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. On Unix, terminal attributes are derived
from `tcgetattr` and restored with `tcsetattr`; pane and job cleanup targets the
owned process group.

Primary references:

- <https://learn.microsoft.com/en-us/windows/console/setconsolemode>
- <https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects>
- <https://pubs.opengroup.org/onlinepubs/009696799/functions/tcsetattr.html>
- <https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/tcsetpgrp.3.html>

## Test architecture

### Deterministic stress runner

A new `wmux-stress` developer crate owns a memory transport, scripted PTY/job
backends, framed protocol client, and deterministic scenario runner. It calls
the real `wmux-server` event loop. It does not duplicate server semantics or
reach into private server state.

The runner has `ci` and `full` profiles. `ci` keeps pull-request latency low;
`full` uses the beta thresholds, including 100,000 history lines and a 16 MiB
paste. Each scenario returns observed counts and a stable fingerprint. A failed
invariant returns an error and a nonzero process exit.

Scenarios are grouped so one failure names the broken contract:

1. lifecycle: repeated attach/detach, abrupt stream drop, reattach, clean
   shutdown, and restart;
2. event pressure: resize storms, pane output followed by exit, and termination
   during queued output;
3. fan-out: many clients, many panes, a deliberately stalled client, and an
   independently responsive controller;
4. storage: 100,000-line history, synchronized output timeout/commit, control
   output pressure, and the maximum legal paste.

The stress crate is a release gate, not a benchmark. Time budgets are generous
deadlock detectors. Performance limits remain in `wmux-bench`.

### Focused server and client regressions

Private queue counters and render baselines remain tested inside
`wmux-server`, where tests can observe them without widening production APIs.
The stress runner proves the external consequence; focused tests prove the
internal bound.

`wmux-client` uses a fake terminal guard to prove that every recoverable attach
exit drops the guard exactly once. Native terminal crates retain exact saved
mode round-trip tests.

### Native lifecycle suites

Unix and Windows integration tests start an isolated real server and native
PTY. They cover abrupt client loss, detach/reattach, pane exit, process-tree
termination, server shutdown, endpoint removal, and restart on the same
endpoint. Windows receives explicit per-instance platform constructors so the
tests never touch a developer's running default server.

The same Unix test source runs on Linux and macOS CI. Local cross-compilation is
compile evidence only; native macOS runtime evidence comes from `macos-latest`.

## Fuzz and malformed-input gate

Stable CI replays every checked-in corpus seed through deterministic malformed
input tests and compiles/clippies all fuzz targets. Ubuntu CI installs
`cargo-fuzz` and gives `command_text`, `protocol_frame`, and `terminal_bytes` a
short fixed-duration sanitizer run. Crash artifacts are uploaded on failure.
Long local fuzzing remains documented for release candidates.

## CI topology

One repository-root workflow uses `defaults.run.working-directory: wmux-clean`.
Its jobs are:

- `quality`: formatting, workspace clippy, fuzz-target clippy, and whitespace;
- `portable`: Windows, Ubuntu, and macOS portable tests plus exact conformance;
- `native`: Windows, Ubuntu, and macOS full workspace/native lifecycle tests;
- `stress`: all three operating systems, release `wmux-stress --profile ci`;
- `fuzz-smoke`: Ubuntu sanitizer smoke and artifact retention;
- `performance`: Windows full release gate, preserving the established
  comparable baseline host.

macOS native execution is required before Phase 8 can be called complete.
Merely configuring the matrix is not runtime evidence.

## Documentation and evidence

`compatibility-matrix.md` separates automation evidence from manual terminal
smoke tests. A cell is never marked supported because a target compiled.
`known-differences.md` classifies each discrepancy as shared bug, native bug,
intentional platform difference, or terminal capability. `beta-core-gate.md`
contains exact commands, thresholds, fingerprints, dates, hosts/runners, and
remaining limitations.

Phase 8 can finish locally with Windows and Linux evidence plus macOS compile
evidence, but its overall status remains “macOS runtime pending” until the
repository-root workflow succeeds on an actual macOS runner.

