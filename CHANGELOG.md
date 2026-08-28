# Changelog

All notable changes to wmux are documented in this file. The project follows
[Semantic Versioning](https://semver.org/).

## [Unreleased]

No changes yet.

## [1.0.15] - 2026-08-28

### Changed

- Attached clients acknowledge physically completed render transactions, while
  the server keeps at most one frame in flight per client and continues
  processing terminal output and user input against the latest scene.
- Changed terminal rows are emitted as coherent row updates, reducing partial
  visual states during rapidly updating full-screen applications.

### Fixed

- Windows native console input now preserves the complete shifted digit row,
  including `!`, `@`, `#`, `$`, `%`, `^`, `&`, `*`, `(`, and `)`.
- Closing or detaching after the final presented frame now exits cleanly instead
  of reporting a presentation-channel failure.

## [1.0.14] - 2026-08-27

### Fixed

- Ctrl+Enter now produces the cross-platform line-feed default expected by
  multiline terminal applications, while other modified Enter events remain
  distinguishable when the terminal host reports them.
- Split cursor hide, repaint, and show sequences retain each client's previous
  complete frame for at most eight milliseconds, reducing cursor flicker during
  animated terminal updates without delaying intentionally hidden cursors.

## [1.0.13] - 2026-08-26

### Changed

- Interactive output is published immediately after keyboard or mouse input,
  paced application frames use a one-millisecond coalescing window, and
  equivalent attached clients share immutable per-turn scene metadata.
- Windows starts the persistent server through bounded in-process WMI, avoiding
  the cold command-interpreter path while preserving the existing provider-owned
  lifetime contract and compatibility fallback.
- The complete release performance gate now runs on Windows, Linux, and macOS.

### Fixed

- Client command arguments now retain empty values, whitespace, quotes,
  backslashes, control characters, and Unicode across IPC.
- Unix terminal clients now drain complete keyboard, paste, and mouse bursts
  even when one read fills the dependency's internal buffer.
- Clients send their physical terminal size before attaching, and resize
  publication waits at most four milliseconds for unsynchronized output.

## [1.0.12] - 2026-08-26

### Fixed

- Alternate-screen wheel navigation now batches three cursor steps into one
  pane write, making coding-agent and full-screen TUI scrolling faster without
  multiplying IPC or PTY operations.
- Alt-drag now joins Shift-drag as an application-mouse override, with visible
  server-owned highlighting and clipboard copy in mouse-aware terminal apps.

## [1.0.11] - 2026-08-25

### Fixed

- Erasing saved terminal lines now removes canonical scrollback without
  clearing the live screen, preventing cleared prompt history from being
  reflowed into a pane after a layout or width change.
- Attached clients now request report-all keyboard events and preserve
  `Ctrl+Enter` and `Shift+Enter` as distinct CSI-u input for interactive
  applications on every supported platform.
- Mouse drag now starts server-owned selection only after movement, leaving
  plain clicks available for pane focus. Shift overrides application mouse
  tracking for selection, and right-click pastes the latest wmux buffer.

## [1.0.10] - 2026-08-25

### Fixed

- Clicking an inactive pane now consumes the complete activation gesture before
  application mouse routing. Mouse-aware applications no longer receive the
  focus click or place their own cursor at the clicked cell.

## [1.0.9] - 2026-08-25

### Fixed

- Plain pane clicks now change focus without entering copy mode or replacing
  the pane application's cursor with a selection cursor. Mouse selection
  remains available after explicitly entering copy mode.

## [1.0.8] - 2026-08-25

### Fixed

- Clicking an inactive pane now selects it before mouse selection or
  application routing, preventing the server recursion that terminated the
  session with a connection-reset error.

## [1.0.7] - 2026-08-25

### Fixed

- Inline terminal applications and coding agents now retain finalized output
  in server-owned scrollback when they use top-anchored scrolling regions.
- Explicit region scrolling and reverse-index operations now honor the active
  margins, preserving transcript history and viewport placement.

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
