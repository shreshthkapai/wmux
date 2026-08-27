# Compatibility Matrix

This matrix records the evidence level for wmux's portable semantics and native
platform integrations. A released feature is not assumed to have identical
native mechanics on every operating system; each status describes what was
actually executed.

Every status cell uses one of these terms:

- `verified`: executed successfully in the current local evidence run;
- `CI-verified`: executed successfully by the repository-root workflow;
- `compile-only`: cross-compiled successfully, without native execution;
- `manual-pending`: runtime or interactive acceptance has not been executed;
- `unsupported`: deliberately outside the current product contract.

Configuring a CI job is not `CI-verified`. Until the new workflow completes on
an actual macOS runner, macOS runtime rows remain `manual-pending` or
`compile-only`.

## Automated core evidence

| Contract | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Portable protocol conformance | `verified` | `verified` | `compile-only` |
| Workspace and shared-runtime tests | `verified` | `verified` | `compile-only` |
| Native transport authentication | `verified` | `verified` | `compile-only` |
| Native PTY input, output, resize, and exit | `verified` | `verified` | `compile-only` |
| Abrupt disconnect and authoritative reattach | `verified` | `verified` | `manual-pending` |
| Native pane and descendant cleanup | `verified` | `verified` | `manual-pending` |
| Endpoint cleanup and same-endpoint restart | `verified` | `verified` | `manual-pending` |
| Exact terminal-mode save and restore tests | `verified` | `verified` | `compile-only` |
| Deterministic stress gate | `verified` | `verified` | `manual-pending` |
| Stable malformed-input corpus replay | `verified` | `verified` | `compile-only` |
| Sanitizer-backed fuzz smoke | `manual-pending` | `verified` | `manual-pending` |
| Full comparable release performance gate | `verified` | `manual-pending` | `manual-pending` |
| Sequenced presentation and slow-sink convergence | `verified` | `manual-pending` | `manual-pending` |
| Coherent row and final-cursor replay | `verified` | `manual-pending` | `manual-pending` |
| Printable punctuation and ordered native mouse events | `verified` | `manual-pending` | `manual-pending` |

Windows uses authenticated named pipes, SID identity, ConPTY, and Job Objects.
Linux uses protected AF_UNIX sockets, `SO_PEERCRED`, PTYs, and process groups.
macOS uses protected AF_UNIX sockets, `getpeereid`, PTYs, and process groups;
both `x86_64-apple-darwin` and `aarch64-apple-darwin` compile, but that is not
native runtime evidence.

## Interactive terminal acceptance

Automated terminal guards prove exact saved-mode restoration on recoverable
paths. They cannot identify or validate the enclosing terminal host, so these
combinations remain explicit manual acceptance work.

| Windows host | PowerShell 7 | Windows PowerShell | cmd.exe |
| --- | --- | --- | --- |
| Windows Terminal | `manual-pending` | `manual-pending` | `manual-pending` |
| conhost | `manual-pending` | `manual-pending` | `manual-pending` |
| VS Code integrated terminal | `manual-pending` | `manual-pending` | `manual-pending` |

| Linux host | bash | zsh | fish |
| --- | --- | --- | --- |
| GNOME Terminal | `manual-pending` | `manual-pending` | `manual-pending` |
| Konsole | `manual-pending` | `manual-pending` | `manual-pending` |
| VS Code integrated terminal | `manual-pending` | `manual-pending` | `manual-pending` |

| macOS host | zsh | bash |
| --- | --- | --- |
| Terminal.app | `manual-pending` | `manual-pending` |
| iTerm2 | `manual-pending` | `manual-pending` |
| VS Code integrated terminal | `manual-pending` | `manual-pending` |

The historical Phase 8 commands and fingerprints remain in
[beta-core-gate.md](beta-core-gate.md). See
[known-differences.md](known-differences.md) for discrepancy classification.
