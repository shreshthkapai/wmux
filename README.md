# wmux

wmux is a persistent, cross-platform terminal multiplexer. It keeps shells,
servers, logs, builds, agents, and project workspaces alive when the terminal
client disconnects.

```text
persistent server
  -> disposable clients
  -> sessions
  -> windows
  -> panes
  -> server-owned virtual terminal state
```

wmux provides consistent multiplexer semantics across supported operating
systems and uses native platform mechanisms for process, terminal, and IPC
integration. It does not require a POSIX emulation layer on Windows.

## Install

### Windows

Run in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -c "irm https://github.com/shreshthkapai/wmux/releases/latest/download/wmux-installer.ps1 | iex"
```

### Linux and macOS

```sh
curl --proto '=https' --tlsv1.2 -LsSf https://github.com/shreshthkapai/wmux/releases/latest/download/wmux-installer.sh | sh
```

The generated installers select the correct release target and place `wmux`,
`wmux-server`, and `wmux-update` together under Cargo's standard binary
directory. Restart the shell if `wmux` is not immediately on `PATH`.

### Supported release targets

| Platform | Architecture | Release target |
| --- | --- | --- |
| Windows | x86-64 | `x86_64-pc-windows-msvc` |
| Linux | x86-64 | `x86_64-unknown-linux-musl` |
| Linux | ARM64 | `aarch64-unknown-linux-musl` |
| macOS | Intel | `x86_64-apple-darwin` |
| macOS | Apple Silicon | `aarch64-apple-darwin` |

Windows ARM64 and package-manager repositories are not part of v1.0.0.

## Quick start

Create and attach to a named session:

```sh
wmux new -s demo
```

Detach with `Ctrl-b d`, then reattach:

```sh
wmux attach -t demo
```

List persistent sessions:

```sh
wmux ls
```

Long-form commands remain available, but the short forms above are the
recommended everyday interface.

## Default keys

Press `Ctrl-b`, release it, then press the action key.

| Key | Action |
| --- | --- |
| `%` | Split left/right |
| `"` | Split top/bottom |
| Arrow key | Select an adjacent pane |
| `o` | Select the next pane |
| `0` through `9` | Select a window by index |
| `c` | Create a window |
| `n` / `p` | Select the next/previous window |
| `,` | Rename the current window |
| `$` | Rename the current session |
| `z` | Toggle pane zoom |
| `[` | Enter copy mode |
| `]` | Paste the latest buffer |
| `d` | Detach the current client |
| `x` | Confirm and kill the current pane |
| `&` | Confirm and kill the current window |
| `X` | Confirm and kill the entire current session |

Run `wmux list-keys` for the authoritative table. See the
[command and key model](docs/command-key-model.md) for command,
target, repeat, prompt, and confirmation behavior.

## Configuration

```sh
wmux config path
wmux config show
wmux config effective
```

wmux creates a default configuration file on first use. Configuration is
server-owned and uses the same command parser as interactive commands. See the
[command and key model](docs/command-key-model.md) and
[known platform differences](docs/known-differences.md).

## Update

First finish or explicitly terminate persistent work. A detached session is
still running and is not safe to discard during an update.

```sh
wmux ls
wmux kill-server
wmux-update
```

Re-running the original installer is also supported. v1 does not perform a
live handoff between running server versions; stopping the server before an
update prevents partial replacement, especially on Windows.

## Manual download and verification

Every [GitHub Release](https://github.com/shreshthkapai/wmux/releases) contains
platform archives, a matching `.sha256` sidecar, the two executables, this
README, the license, and the changelog.

On Linux or macOS:

```sh
sha256sum -c wmux-*.sha256
```

On Windows:

```powershell
Get-FileHash .\wmux-*.zip -Algorithm SHA256
```

Compare the printed hash with the archive's `.sha256` file. With the GitHub
CLI installed, verify build provenance with:

```sh
gh attestation verify <downloaded-archive> --repo shreshthkapai/wmux
```

The v1 binaries are not Authenticode-signed or Apple-notarized. Windows
SmartScreen or macOS Gatekeeper may therefore show a trust prompt. Checksums
and GitHub attestations provide integrity and build provenance, but they do not
replace platform code signing.

## Architecture and quality

The persistent server is the only authority for sessions, windows, panes,
terminal grids, layouts, options, commands, paste buffers, jobs, and clients.
Platform crates own only PTY/process, IPC, terminal-mode, and credential
mechanics. Rendering uses chunked IO, server-owned terminal grids, dirty-region
tracking, coalesced redraws, and batched terminal writes.

- [Platform contract](docs/platform-contract.md)
- [Cross-OS conformance](docs/cross-os-conformance.md)
- [Compatibility evidence](docs/compatibility-matrix.md)
- [Rendering model](docs/hybrid-rendering.md)
- [Performance gates](docs/performance.md)

## Contributing and security

Contributions are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md) before opening a pull request. Report
vulnerabilities using the private process in [SECURITY.md](SECURITY.md), never
through a public issue.

wmux is available under the [MIT License](LICENSE).
