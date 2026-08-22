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
IPC input, key and bracketed-paste translation, DEC mouse-mode routing, and the
portable command/target/key-table model. Its platform-lifecycle case dispatches
spawn, write, resize, and terminate requests through `dyn PtyBackend`, then
asserts output, exit, and final-close event ordering. Each case produces a
stable FNV-1a fingerprint. The aggregate fingerprint is checked against
`EXPECTED_PORTABLE_FINGERPRINT`; a semantic change must deliberately update
the fixture and expected value.

The GitHub Actions matrix runs the portable core, protocol, platform contract,
config, shared server library, shared client library, and conformance crates on
all three operating systems. This prevents native types or host-dependent
behavior from entering the shared semantic path.

### Phase 6 portable contract evidence

The portable suite contains 14 cases and produces aggregate fingerprint
`f71b72b35879a1c6`. `EXPECTED_DIFFERENCES` is the only semantic-exception
registry and remains empty in Phase 6: ConPTY versus PTY, named pipes versus Unix
sockets, and SID versus UID are implementation mechanisms, not observable mux
differences. Tests may not hide differences behind platform-conditional
assertions.

The shared server real-protocol test uses an in-memory listener and scripted
PTY to cover create, attach, input, split, resize, detach, background output,
reattach, pane/session destruction, and server shutdown. It also rejects a
wrong peer before `Hello` and bounds wrong-pane and post-close events.

### Phase 4 command and key evidence

The Phase 4 verification run on 2026-08-21 passed all 274 workspace tests. The
portable suite contains 13 cases and produced aggregate fingerprint
`00b763c726b9d162` on Windows. Two new portable cases cover command-list
parsing, qualified target resolution, prefix and repeat handling, live binding
mutation, exact `send-keys` output, session switching, per-client refresh, and
confirmation rejection/acceptance.

Fixed-seed malformed-input tests cover the command, target, and key parsers and
protocol-v6 semantic-key decoder. The `command_text` fuzz target compiles on
stable Windows; sanitizer-backed fuzz execution remains a nightly Unix CI or
developer task. The full release performance rejection gate, including the
four Phase 4 routing/parser workloads, passed without weakening an earlier
threshold. See `command-key-model.md` for the model, exact measurements, and
reference comparison limits.

## Native Coverage

| Contract | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Portable semantic suite | Enforced | Enforced in CI | Enforced in CI |
| PTY input/output | ConPTY IOCP test | Native PTY tests verified | Native PTY CI gate configured |
| Process-tree cleanup | Job Object descendant test | Process-group descendant tests verified | Process-group CI gate configured |
| Native transport | Named-pipe tests | AF_UNIX and `SO_PEERCRED` tests verified | AF_UNIX and `getpeereid` CI gate configured |
| Full native lifecycle | Windows protocol lifecycle | Real Linux socket/PTY lifecycle verified | Native lifecycle CI gate configured |

### Phase 6 Unix lifecycle evidence

`wmux-unix` supplies the real platform adapter on Linux and macOS. Its native
integration test drives protocol v6 over a real AF_UNIX socket and PTY through:

```text
create -> attach -> type -> split -> resize -> detach
       -> background output -> reattach -> kill pane/session -> kill server
```

The test asserts rendered shell output after reattachment and verifies that the
owned socket and lock are removed after shutdown. Linux native execution is
verified locally with:

```powershell
cargo test -p wmux-unix
cargo test -p wmux-unix --test native_lifecycle
cargo check -p wmux -p wmux-server --bins
cargo run -p wmux-conformance --release
```

The `native-unix` CI matrix runs the same adapter tests, lifecycle test, binary
checks, format check, warnings-denied clippy, and portable conformance on
`ubuntu-latest` and `macos-latest`. Both `x86_64-apple-darwin` and
`aarch64-apple-darwin` compile checks pass locally. Native macOS behavior is
still reported as CI-gated rather than verified here because no macOS runner
was available in the local verification environment.

The final Phase 6 local gate on 2026-08-22 passed 285 Windows workspace tests
and 289 Linux Unix/shared tests. The Linux total includes 39 `wmux-unix` unit
tests and the real native lifecycle integration test. Windows and Linux both
produced all 14 case fingerprints and the same aggregate
`f71b72b35879a1c6`. A release Linux CLI smoke test also completed detached
session creation, session/pane listing, splitting, session destruction, server
shutdown, and socket/lock cleanup. The shared-source native seam audit found no
native Unix types or APIs outside `wmux-unix` and the two binary composition
roots.

### Phase 3 Windows lifecycle evidence

The following rows are native automated tests run on Windows. They exercise
unique per-test SID endpoints and bounded waits; they do not depend on an
interactive terminal host.

The Phase 3 verification run on 2026-08-20 passed all 209 workspace tests,
produced portable conformance fingerprint `77b632078fd0ab8b`, and passed the
full release performance gate.

| Contract | Automated evidence | Status |
| --- | --- | --- |
| Token-SID endpoint identity and bounded unique instance names | `wmux-windows::pipe::tests::endpoint_*` | Verified |
| Owner-only DACL, first-instance exclusion, and peer SID rejection | `factory_sddl_grants_only_the_owner_sid`, `factory_rejects_a_preexisting_first_pipe_instance`, `authenticated_pipe_is_full_duplex_and_rejects_a_different_sid` | Verified |
| Stale marker recovery without replacing a live lock owner | `server_lock_recovers_a_stale_lock_file`, `server_lock_rejects_a_second_live_owner`, `server_lock_prevents_replacement_while_its_owner_is_live` | Verified |
| WMI daemon survives bootstrap exit and is cleaned up by PID ownership | `native_wmi_launcher_returns_a_live_process`, `bootstrap_timeout_is_bounded_when_output_pipes_fill`, `bootstrap_return_is_bounded_when_descendant_inherits_output` | Verified |
| Client disconnect after authenticated hello preserves detached state | `disconnect_after_hello_preserves_detached_session` | Verified |
| Process exit plus ConPTY EOF yields one status and closes the event stream | `process_exit_and_conpty_eof_close_the_event_stream_once`, `exit_and_eof_are_coalesced_and_release_the_platform_pane` | Verified |
| `kill-server` sends complete final frames and releases listener/lock state | `shutdown_writer_flushes_reports_drain_and_returns`, `shutdown_drains_clients_releases_lock_and_restarts` | Verified |
| Same SID endpoint can restart after graceful shutdown | `shutdown_drains_clients_releases_lock_and_restarts` | Verified |

Interactive host/shell combinations remain a separate manual acceptance pass.
No automated ConPTY or renderer test identifies the enclosing host as Windows
Terminal, conhost, or VS Code, so those combinations are not inferred from the
native results above.

| Terminal host | PowerShell 7 | Windows PowerShell | cmd.exe |
| --- | --- | --- | --- |
| Windows Terminal | Manual pending | Manual pending | Manual pending |
| conhost | Manual pending | Manual pending | Manual pending |
| VS Code integrated terminal | Manual pending | Manual pending | Manual pending |

Windows process cleanup launches a real descendant process and verifies that
terminating the pane's Job Object kills the descendant. This matches the
documented Windows behavior that child processes join the parent's job by
default and `TerminateJobObject` terminates every associated process:

- <https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects>
- <https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-terminatejobobject>

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

These tests run in the platform-neutral server library and therefore execute
in the Windows, Linux, and macOS portable CI matrix.
