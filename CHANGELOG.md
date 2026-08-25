# Changelog

All notable changes to wmux are documented in this file. The project follows
[Semantic Versioning](https://semver.org/).

## [Unreleased]

No changes yet.

## [1.0.6] - 2026-08-25

### Fixed

- Applications that request alternate-screen wheel navigation now receive
  cursor navigation input instead of losing wheel events in an alternate
  buffer with no multiplexer scrollback.

## [1.0.5] - 2026-08-25

### Fixed

- Normal keyboard input and paste now return the originating client from wheel
  scrollback to the live pane before the input is rendered, without changing
  another client's independent viewport or exiting explicit copy mode.

## [1.0.4] - 2026-08-24

### Changed

- Attached terminals now preserve modified Enter identities, use button-event
  mouse tracking instead of unpressed pointer-motion tracking, and support
  server-owned left-drag text selection when an application has not requested
  mouse input.
- Atomic render transactions avoid redundant physical cursor hide/show
  transitions, reducing cursor flicker and terminal state churn during rapid
  full-screen updates.

### Fixed

- Linux and macOS clients now treat a refused stale Unix socket as an absent
  server, allowing the existing safe startup path to replace a terminated
  server instead of failing with `Connection refused`.
- Ctrl+J, Alt+Enter, and distinct modified Enter events now retain the expected
  application identity across both platform input adapters and in panes that
  request private Win32 input records, including Windows-native applications
  launched through Unix interop.

## [1.0.3] - 2026-08-24

### Changed

- IPC protocol version 8 carries a bounded, absolute client working directory
  as client-scoped pane launch context.

### Fixed

- New sessions, windows, and panes now start in the directory from which their
  client invoked wmux instead of inheriting the persistent server's older
  working directory on Windows, Linux, and macOS.

## [1.0.2] - 2026-08-24

### Added

- Added terminal-native UI theming for pane borders and the status row with
  five presets, strict JSON theme files, explicit per-field overrides, custom
  one-cell glyph sets, and atomic live reloads.
- Added bounded client-scoped `once` and `loop` animations with built-in
  effects, custom frames, blocked-client coalescing, and an independent
  animated-render performance gate.
- Added trusted one-shot theme providers with strict output validation,
  startup and reload execution, time/output limits, and newest-generation-only
  commits.

### Changed

- Promoted the production Cargo workspace to the repository root and removed
  the obsolete duplicate workspace and temporary nested directory.

### Fixed

- Restored primary-screen cursor, style, wrap, and repaint state when terminal
  applications leave alternate-screen modes, including correct behavior across
  resize and reflow.

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

[Unreleased]: https://github.com/shreshthkapai/wmux/compare/v1.0.4...HEAD
[1.0.4]: https://github.com/shreshthkapai/wmux/compare/v1.0.3...v1.0.4
[1.0.3]: https://github.com/shreshthkapai/wmux/compare/v1.0.2...v1.0.3
[1.0.2]: https://github.com/shreshthkapai/wmux/compare/v1.0.1...v1.0.2
[1.0.1]: https://github.com/shreshthkapai/wmux/compare/v1.0.0...v1.0.1
[1.0.0]: https://github.com/shreshthkapai/wmux/releases/tag/v1.0.0
