# Cross-OS Conformance

wmux keeps portable multiplexer semantics independent from native handles.
Each platform backend owns process, terminal, transport, credential, and
cleanup mechanics behind the OS-neutral platform contract.

## Portable suite

`wmux-conformance` executes the same semantic cases on Windows, Linux, and
macOS:

```sh
cargo run --locked -p wmux-conformance --release
```

The suite covers deterministic VT replay, detach and reattach persistence,
multiple-client scene consistency, resize and reflow, malformed terminal and
IPC input, key and bracketed-paste translation, mouse routing, command and
target behavior, platform lifecycle ordering, and control-protocol behavior.
Each case produces a stable fingerprint. A semantic change must deliberately
update the relevant fixture and accepted aggregate.

`EXPECTED_DIFFERENCES` is the only registry for observable portable-semantic
exceptions. It is currently empty. Platform-specific tests may validate native
mechanics, but shared tests must not hide behavioral differences behind
conditional assertions.

## Current release evidence

The accepted portable aggregate for v1.0.17 is `b217356f473bf48e`. Protocol
version 9, client working-directory context, server-owned status rendering,
connected borders, active-pane emphasis, and stable window-list centering are
part of the current fixtures.

The [v1.0.17 quality run](https://github.com/shreshthkapai/wmux/actions/runs/33201827599)
completed successfully on the release commit. Hosted Windows, Linux, and macOS
runners passed portable semantics, native lifecycle, deterministic stress, and
the full release performance gate. The Linux sanitizer fuzz smoke and the
workspace-wide format, lint, test, and fuzz-build jobs also passed.

## Native coverage

| Contract | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Portable semantic suite | `CI-verified` | `CI-verified` | `CI-verified` |
| PTY input, output, resize, and exit | `CI-verified` | `CI-verified` | `CI-verified` |
| Process-tree cleanup | `CI-verified` | `CI-verified` | `CI-verified` |
| Native authenticated transport | `CI-verified` | `CI-verified` | `CI-verified` |
| Abrupt disconnect and authoritative reattach | `CI-verified` | `CI-verified` | `CI-verified` |
| Endpoint cleanup and restart | `CI-verified` | `CI-verified` | `CI-verified` |
| Deterministic stress gate | `CI-verified` | `CI-verified` | `CI-verified` |
| Full release performance gate | `CI-verified` | `CI-verified` | `CI-verified` |

Windows uses authenticated named pipes, SID identity, ConPTY, and Job Objects.
Linux uses protected AF_UNIX sockets, `SO_PEERCRED`, PTYs, and process groups.
macOS uses protected AF_UNIX sockets, `getpeereid`, PTYs, and process groups.
These mechanisms implement the same wmux contracts without claiming identical
native APIs.

## Native lifecycle

The production adapters drive this lifecycle over real transports and
pseudoterminals:

```text
create -> attach -> type -> split -> resize -> detach
       -> background output -> reattach -> kill pane/session -> kill server
```

Tests assert authoritative output after reattachment, process-tree cleanup,
one terminal close event, endpoint removal, and restart on the same endpoint.
Windows tests additionally cover owner-only named-pipe access and Job Object
cleanup. Unix tests cover peer credentials, descriptor discipline, process
groups, and terminal-mode restoration.

## Server invariants

Workspace and conformance tests protect these OS-neutral properties:

- detached pane output always mutates the authoritative screen;
- clients consume pane generations independently;
- pane output is serviced fairly within byte and time budgets;
- a full pane queue cannot block commands;
- slow-client output remains bounded and converges to current state;
- malformed terminal and IPC input cannot crash the server;
- application mouse modes take precedence over multiplexer scrollback;
- attached clients retain independent history viewport offsets;
- render sequencing preserves coherent rows and final cursor state.

See [compatibility-matrix.md](compatibility-matrix.md) for evidence levels and
[known-differences.md](known-differences.md) for native-mechanism differences.
