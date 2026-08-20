# Cross-OS Conformance

Step 15 follows the same separation used by tmux's portable regression tests
and zellij's mock-backed grid and layout snapshots: multiplexer semantics run
without native handles, while each platform backend owns additional lifecycle
tests.

## Portable Suite

`wmux-conformance` executes the identical code on Windows, Linux, and macOS:

```powershell
cargo run -p wmux-conformance --release
```

The suite covers deterministic VT replay, detach and reattach persistence,
multiple-client scene consistency, resize and reflow, malformed terminal and
IPC input, key and bracketed-paste translation, and DEC mouse-mode routing.
Each case produces a
stable FNV-1a fingerprint. The aggregate fingerprint is checked against
`EXPECTED_PORTABLE_FINGERPRINT`; a semantic change must deliberately update
the fixture and expected value.

The GitHub Actions matrix runs the portable core, protocol, platform contract,
and conformance crates on all three operating systems. This prevents native
types or host-dependent behavior from entering the shared semantic path.

## Native Coverage

| Contract | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Portable semantic suite | Enforced | Enforced in CI | Enforced in CI |
| PTY input/output | ConPTY IOCP test | Awaiting Unix backend | Awaiting Unix backend |
| Process-tree cleanup | Job Object descendant test | Awaiting Unix backend | Awaiting Unix backend |
| Native transport | Named-pipe tests | Awaiting Unix backend | Awaiting Unix backend |

Windows process cleanup launches a real descendant process and verifies that
terminating the pane's Job Object kills the descendant. This matches the
documented Windows behavior that child processes join the parent's job by
default and `TerminateJobObject` terminates every associated process:

- <https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects>
- <https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject>

Linux and macOS native conformance must not be marked complete until the Unix
PTY/process/socket backend exists. The portable suite is ready now and becomes
the semantic acceptance contract for that backend.

## Server Invariants

Workspace tests supplement the portable fingerprints with runtime properties
that are independent of the native PTY implementation:

- detached pane output always mutates the authoritative screen
- clients consume pane generations independently
- pane output is serviced round-robin within byte and time budgets
- a full pane queue cannot block commands
- client output is byte bounded and coalesces while a client is slow
- malformed framed IPC is rejected without crashing the server
- application mouse modes take precedence over multiplexer scrollback
- attached clients retain independent history viewport offsets

These tests run in the Windows workspace job today and should move into a
platform-neutral server crate when native transport selection is introduced.
