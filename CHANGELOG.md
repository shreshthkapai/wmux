# Changelog

All notable changes to wmux are documented in this file. The project follows
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Changed

- Promoted the production Cargo workspace to the repository root and removed
  the obsolete duplicate workspace and temporary nested directory.

## [1.0.1] - 2026-08-24

### Changed

- Pane processes now receive wmux-native identity variables and advertise
  `TERM_PROGRAM=wmux` without legacy multiplexer aliases.
- Project documentation and contribution guidance now define behavior from
  wmux requirements, standards, and native platform contracts.

## [1.0.0] - 2026-08-23

### Added

- Persistent per-user server with disposable attached clients and authoritative
  session, window, pane, layout, option, buffer, job, and client state.
- Native Windows ConPTY, named-pipe, process, and Job Object integration without
  a POSIX emulation dependency.
- Native Linux and macOS PTY, Unix-socket, credential, signal, process-group,
  and terminal-mode integration.
- Versioned framed IPC with authenticated local transports and bounded malformed
  input handling.
- Server-owned VT parsing, Unicode-aware screen grids, scrollback, alternate
  screens, terminal modes, dirty tracking, and client-scoped render baselines.
- Server-owned sessions, windows, panes, splits, targets, command queue, key
  tables, prefix handling, prompts, confirmations, copy mode, and paste buffers.
- Configuration, inherited options, formats, hooks, async shell jobs,
  notifications, and structured control mode.
- Theme-native status rendering, connected pane borders, focused-pane
  highlighting, and interactive window/session rename fields.
- Release artifacts for Windows x64, Linux x64/ARM64 musl, and macOS
  Intel/Apple Silicon with PowerShell and shell installers, SHA-256 checksums,
  an updater, and GitHub attestations.

### Known limitations

- v1.0.0 executables are not Authenticode-signed or Apple-notarized, so native
  platform trust prompts may appear.
- Updates require active sessions to be finished or explicitly terminated;
  live server state migration is not implemented.
- Windows ARM64 and package-manager repositories are not included in v1.0.0.

[Unreleased]: https://github.com/shreshthkapai/wmux/compare/v1.0.1...HEAD
[1.0.1]: https://github.com/shreshthkapai/wmux/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/shreshthkapai/wmux/releases/tag/v1.0.0
