# Compatibility Matrix

This matrix records the evidence level for wmux's portable semantics and native
platform integrations. Consistent behavior does not require identical native
mechanics on every operating system.

Status terms:

- `CI-verified`: executed successfully on a hosted runner for that OS;
- `verified`: executed successfully in a documented local or manual check;
- `manual-pending`: interactive host acceptance has not been recorded;
- `not-run`: the check intentionally runs on another supported host.

## Automated release evidence

The [v1.0.17 quality run](https://github.com/shreshthkapai/wmux/actions/runs/33201827599)
passed on the release commit.

| Contract | Windows | Linux | macOS |
| --- | --- | --- | --- |
| Portable protocol conformance | `CI-verified` | `CI-verified` | `CI-verified` |
| Shared runtime tests | `CI-verified` | `CI-verified` | `CI-verified` |
| Native transport authentication | `CI-verified` | `CI-verified` | `CI-verified` |
| Native PTY input, output, resize, and exit | `CI-verified` | `CI-verified` | `CI-verified` |
| Abrupt disconnect and authoritative reattach | `CI-verified` | `CI-verified` | `CI-verified` |
| Native pane and descendant cleanup | `CI-verified` | `CI-verified` | `CI-verified` |
| Endpoint cleanup and same-endpoint restart | `CI-verified` | `CI-verified` | `CI-verified` |
| Terminal-mode save and restore | `CI-verified` | `CI-verified` | `CI-verified` |
| Deterministic stress gate | `CI-verified` | `CI-verified` | `CI-verified` |
| Malformed-input corpus replay | `CI-verified` | `CI-verified` | `CI-verified` |
| Sanitizer-backed fuzz smoke | `not-run` | `CI-verified` | `not-run` |
| Full release performance gate | `CI-verified` | `CI-verified` | `CI-verified` |
| Sequenced presentation and slow-sink convergence | `CI-verified` | `CI-verified` | `CI-verified` |
| Coherent row and final-cursor replay | `CI-verified` | `CI-verified` | `CI-verified` |
| Printable punctuation and ordered native mouse events | `CI-verified` | `CI-verified` | `CI-verified` |

Windows uses authenticated named pipes, SID identity, ConPTY, and Job Objects.
Linux uses protected AF_UNIX sockets, `SO_PEERCRED`, PTYs, and process groups.
macOS uses protected AF_UNIX sockets, `getpeereid`, PTYs, and process groups.

## Interactive terminal acceptance

Automated terminal guards prove saved-mode restoration on recoverable paths.
They do not identify or validate every enclosing terminal host and shell
combination, so those combinations remain explicit manual acceptance work.

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

See [cross-os-conformance.md](cross-os-conformance.md) for the tested semantic
contract and [known-differences.md](known-differences.md) for native-mechanism
differences.
