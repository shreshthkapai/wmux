# Reference tmux Semantics

This document tracks the tmux behavior wmux intentionally copies, where wmux
differs because it is Windows/ConPTY-native, and what test coverage exists.

It is a product and engineering contract. Before changing a tmux-inspired
behavior, update this file first.

Primary tmux reference:

```text
https://man7.org/linux/man-pages/man1/tmux.1.html
```

## Principles

- Copy tmux workflow semantics where they make sense on native Windows.
- Do not copy implementation details that are Unix-specific.
- Do not add placeholder behavior when ConPTY or host-terminal behavior differs.
- If exact tmux behavior is not practical, document the difference here and
  surface it to users through status/errors where possible.
- Keep tests tied to the behavior, not to incidental implementation details.

## Coverage Labels

```text
Covered   Unit and/or script coverage exists for the intended wmux behavior.
Partial   Some coverage exists, but edge cases or exact tmux parity remain open.
Manual    Manual validation is required.
Gap       Behavior needs tests before it can be considered reliable.
```

## Prefix Behavior

tmux behavior:

- Default prefix is `Ctrl+b`.
- Prefix enters a one-key command state.
- `Ctrl+b Ctrl+b` sends a literal prefix key to the active pane.
- Unknown prefix keys are consumed by tmux's key table and do not become normal
  shell input.

wmux intended behavior:

- Default prefix is `Ctrl+b`.
- Prefix enters explicit `PrefixPending` client mode.
- The following key is decoded through the key binding table and becomes a
  runtime command when bound.
- Unknown prefix keys should show a status message and return to normal mode.
- Normal shell input must resume after the prefix decision.

Intentional differences:

- Prefix behavior is decoded from Windows terminal input and capability quirks,
  not from terminfo.
- Prefix remapping exists in config, but tmux's full key table/config language
  is not the v1 target.

Test coverage:

- Covered by `tests/attach_keymap_test.cpp`.
- Covered by `tests/attach_input_mode_test.cpp`.
- Covered by `tests/terminal_input_test.cpp`.
- Manual validation remains required in Windows Terminal and VSCode terminal.

## New Window

tmux behavior:

- `Ctrl+b c` creates a new window and switches the attached client to it.
- `new-window` can also be run as a command.
- Each window has independent pane state.

wmux intended behavior:

- `Ctrl+b c` creates a new window in the attached session and switches to it.
- `wmux new-window -n <name>` creates a named window through control IPC.
- The new window starts with one pane and one platform PTY process.
- Existing windows and panes keep their shell state.

Intentional differences:

- wmux uses stable `WindowId` internally. Window indexes are display metadata.
- Full tmux targeting syntax is not implemented yet.

Test coverage:

- Covered by `tests/attach_keymap_test.cpp`.
- Covered by `tests/command_engine_test.cpp`.
- Covered by `tests/session_manager_test.cpp`.
- Covered by `scripts/test-window-switching.ps1`.

## Next/Previous Window

tmux behavior:

- `Ctrl+b n` selects the next window.
- `Ctrl+b p` selects the previous window.
- Window selection wraps.
- Window shell state remains independent.

wmux intended behavior:

- `Ctrl+b n` and `Ctrl+b p` select the next/previous window for the session.
- Active window is tracked by stable ID.
- Rendering shows only the active window for the attached client.
- Window switching must not recreate or reset pane PTY processes.

Intentional differences:

- Multi-client window view semantics are still simpler than tmux.
- Full window index targeting is not complete.

Test coverage:

- Covered by `tests/attach_keymap_test.cpp`.
- Covered by `tests/command_engine_test.cpp`.
- Covered by `tests/session_manager_test.cpp`.
- Covered by `scripts/test-window-switching.ps1`.

## Pane Split

tmux behavior:

- `Ctrl+b %` splits the active pane left/right.
- `Ctrl+b "` splits the active pane top/bottom.
- `split-window -h` and `split-window -v` create panes from commands.
- Same-axis splits behave as one split group in the layout model.
- The new pane starts a shell and becomes active.

wmux intended behavior:

- `Ctrl+b %` creates a left/right split.
- `Ctrl+b "` creates a top/bottom split.
- Splits are stored in the n-child weighted layout arena.
- Same-axis splits are flattened.
- Opposite-axis splits are nested.
- The new pane starts a platform PTY process and becomes active.
- ConPTY resize happens only after pane body rectangles are computed.

Intentional differences:

- Minimum pane sizes may refuse a split earlier than tmux in very small Windows
  terminals.
- Tmux has many extra split flags and targeting forms not implemented yet.

Test coverage:

- Covered by `tests/session_manager_test.cpp`.
- Covered by `tests/command_engine_test.cpp`.
- Covered by `scripts/test-pane-focus.ps1`.
- Covered by `scripts/test-command-mode.ps1`.
- Partial for complex nested real-terminal rendering under heavy output.

## Pane Kill

tmux behavior:

- `Ctrl+b x` kills the active pane after confirmation.
- `kill-pane` destroys the target pane.
- If no panes remain in the containing window, the window is destroyed.

wmux intended behavior:

- `Ctrl+b x` kills the active pane with a safety guard.
- If the active pane is the last pane in a multi-window session, the active
  window can be removed and another window is selected.
- If it is the last pane of the last window, wmux refuses instead of killing the
  entire session accidentally.
- Pane kill is idempotent and must clean up the shell process tree where
  Windows allows it.

Intentional differences:

- wmux is stricter than tmux for the last pane of the last window because the
  product goal is persistent workflow safety.
- tmux confirmation UI is not fully cloned yet.

Test coverage:

- Covered by `tests/session_manager_test.cpp`.
- Covered by process cleanup scripts through `scripts/test-release-gate.ps1`.
- Partial for all descendant process-tree cleanup cases because Windows Job
  Object assignment can be environment-dependent.

## Spread/Equalize Panes

tmux behavior:

- `select-layout -E` spreads panes evenly.
- The behavior is semantic, not a separate horizontal/vertical equalize command.
- It works against the meaningful split group around the active pane and may
  climb outward when the local group is already even.

wmux intended behavior:

- `Ctrl+b E` maps to `SpreadPanesEvenly`.
- Lowercase `Ctrl+b e` is intentionally not bound.
- The command starts at the active pane leaf, walks to the parent split group,
  equalizes that group if visual sizes change, otherwise climbs upward.
- The layout tree shape is preserved.
- Weights are updated with largest-remainder style sizing during geometry
  computation so leftover cells are distributed predictably.
- After equalization, pane body sizes are recomputed and PTYs are resized.

Intentional differences:

- wmux uses the n-child layout arena and Windows terminal geometry rather than
  tmux's internal layout code.
- Exact tmux behavior for every historical layout preset is not claimed.

Test coverage:

- Covered by `tests/attach_keymap_test.cpp`.
- Covered by `tests/command_engine_test.cpp`.
- Covered by `tests/session_manager_test.cpp`.
- Partial for deep mixed-axis manual parity with tmux.

## Copy Mode Entry/Exit

tmux behavior:

- `Ctrl+b [` enters copy mode and lets the user inspect history.
- `q` exits copy mode.
- `Escape` exits copy mode in common configurations.
- Copy mode input is not sent to the pane.

wmux intended behavior:

- `Ctrl+b [` enters copy mode for the active pane.
- `q` and `Escape` exit copy mode.
- Arrow/Page/Home/End navigation moves the copy-mode cursor/viewport.
- Copy-mode input never goes to the active shell.
- Resize clamps the cursor, selection, and viewport safely.

Intentional differences:

- tmux supports vi/emacs copy-mode key tables and many extra commands. wmux v1
  only implements the core workflow.

Test coverage:

- Covered by `tests/attach_input_mode_test.cpp`.
- Covered by `tests/copy_selection_test.cpp`.
- Covered by `tests/daemon_render_test.cpp`.
- Partial for full pager/editor alternate-screen scenarios.

## Copy Selection

tmux behavior:

- Selection starts in copy mode.
- Selection can be extended and copied into a tmux paste buffer.
- Wrapped lines are copied as logical text where practical.

wmux intended behavior:

- `Space` starts selection.
- Cursor movement extends selection.
- `Enter` copies selected text into the wmux paste buffer.
- Selection supports reversed ranges.
- Selection may span scrollback and the live grid.
- Copy extraction returns logical text with normalized newlines.
- Wide cells and combining marks are handled defensively.

Intentional differences:

- Full Unicode property database parity is not claimed.
- Complex terminal applications need more golden tests before claiming tmux
  parity.

Test coverage:

- Covered by `tests/copy_selection_test.cpp`.
- Covered by `tests/terminal_grid_test.cpp`.
- Covered by `tests/paste_buffer_test.cpp`.
- Partial for real-world alternate-screen applications.

## Paste Buffer

tmux behavior:

- `Ctrl+b ]` pastes the most recent paste buffer into the active pane.
- tmux maintains paste buffers internally.

wmux intended behavior:

- `Ctrl+b ]` pastes the current wmux paste buffer into the active pane.
- Copy mode writes the internal wmux paste buffer first.
- Windows clipboard write is best-effort and must not block or break internal
  copy state.
- Paste size is bounded by resource limits.
- Bracketed paste is used when supported and enabled.

Intentional differences:

- Windows clipboard integration is primary. OSC52 is capability-gated and not a
  v1 dependency.
- tmux's full buffer list and choose-buffer UI are not implemented yet.

Test coverage:

- Covered by `tests/paste_buffer_test.cpp`.
- Covered by `tests/ipc_protocol_test.cpp`.
- Covered by `tests/attach_input_mode_test.cpp`.
- Covered by copy/paste paths in `scripts/test-stress-suite.ps1`.

## Mouse Click Focus

tmux behavior:

- With `set -g mouse on`, clicking a pane selects that pane.
- Mouse behavior depends on terminal support.

wmux intended behavior:

- Mouse mode is capability-gated and daemon-configured.
- With mouse enabled, SGR mouse press events are decoded into typed
  `MouseEvent` values.
- Mouse coordinates are resolved through `TargetResolver`.
- Click-to-focus becomes a runtime command and changes the active pane.
- Input after focus routes only to the active pane.

Intentional differences:

- SGR mouse is the supported target. Legacy mouse encodings are not a v1
  stability target.
- Host-terminal quirks are handled through `TerminalCapabilities`.

Test coverage:

- Covered by `tests/mouse_input_test.cpp`.
- Covered by `tests/terminal_input_test.cpp`.
- Covered by `tests/command_engine_test.cpp`.
- Covered by `scripts/test-mouse-focus.ps1`.

## Mouse Drag Resize

tmux behavior:

- With mouse mode enabled, dragging pane borders resizes panes.
- Layout changes preserve the pane tree and resize PTYs.

wmux intended behavior:

- SGR mouse drag events are parsed when mouse mode is enabled.
- Dragging a pane border resolves to a resize target.
- Resize happens through the daemon command/event path.
- Layout weights are updated, body rectangles recomputed, and affected PTYs are
  resized after final geometry is known.

Intentional differences:

- Modifier-aware mouse gestures are not complete.
- Behavior outside pane borders/status line is intentionally ignored.

Test coverage:

- Covered by `tests/mouse_input_test.cpp`.
- Covered by `tests/ipc_protocol_test.cpp`.
- Covered by `scripts/test-mouse-resize.ps1`.
- Partial for terminal-specific drag quirks.

## Status Line

tmux behavior:

- A status line is shown at the bottom by default.
- It displays session/window context and transient command messages.
- It is used for command prompt and interactive feedback.

wmux intended behavior:

- Status line shows session, active window, active pane, and mode/status
  messages.
- Command prompt uses the status row.
- User-visible errors should appear in status instead of silently failing.
- Temporary messages should clear by policy rather than persist forever.

Intentional differences:

- wmux does not yet implement tmux's full status formatting language.
- The richer status model is tracked separately as the UX/status hardening step.

Test coverage:

- Covered by `tests/daemon_render_test.cpp` for render behavior.
- Covered by `tests/command_mode_test.cpp` for command prompt status text.
- Partial for complete temporary message clearing and all required feedback
  cases.

## Detach/Reattach

tmux behavior:

- `Ctrl+b d` detaches the current client.
- Sessions and pane processes keep running after detach or terminal close.
- A later attach reconnects to the existing session.
- tmux uses a separate server process and client processes.

wmux intended behavior:

- `Ctrl+b d` sends an explicit detach message.
- Client disconnect, pipe failure, or terminal close must not kill sessions.
- The daemon owns sessions, windows, panes, PTY processes, screen grids, and
  scrollback.
- Reattach opens a new attach stream, recomputes layout for the new client
  terminal size, sends a full frame, and resumes live output.
- Terminal cleanup is RAII-protected in the client platform layer.

Intentional differences:

- tmux uses Unix sockets and PTYs. wmux uses Windows named pipes/framed attach
  streams and ConPTY through the platform backend.
- Multi-client semantics are simpler than tmux for now.

Test coverage:

- Covered by `tests/ipc_protocol_test.cpp` for detach frames.
- Covered by `scripts/test-attach-lifecycle.ps1`.
- Covered by `scripts/test-detach-reattach.ps1`.
- Covered by release-gate attach/detach and soak paths.
- Manual validation remains required for terminal close and Ctrl+C/Ctrl+Break
  behavior across host terminals.

## Intentional Non-Parity For Now

These tmux behaviors are not v1 requirements unless separately added to the
roadmap:

```text
full tmux command language
full key table and bind-key model
tmux control mode
choose-tree/choose-buffer UI
window links across sessions
pane zoom
pane synchronization
hooks
formats/status formatting language
all layout presets
remote client protocol
```

When one of these becomes a target, add a section to this file before
implementation.
