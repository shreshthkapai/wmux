# Runtime Contract

This document defines the supported runtime environments for wmux. It is also
the place to record known limitations instead of pretending every terminal and
shell behaves identically.

## Host Terminal Support Tiers

Tier 1:

```text
Windows Terminal
```

Tier 2:

```text
VSCode integrated terminal
```

Tier 3:

```text
WezTerm
Alacritty
classic conhost, if practical
```

The terminal capability layer must be used for host-specific behavior.

## Shell Support Tiers

Tier 1:

```text
PowerShell 7 / pwsh
Windows PowerShell
cmd.exe
```

Tier 2:

```text
wsl.exe
Git Bash
NuShell
```

Tier 3:

```text
arbitrary custom shell command
```

## Known Unsupported Or Limited Cases

This list should be updated whenever a bug report proves a new class of
runtime issue.

```text
Unsupported host terminals: any terminal without working ConPTY/VT behavior.
Unsupported shells: shells that require unsupported console APIs or private protocols.
Mouse limitations: SGR mouse is expected; legacy encodings are not a stability target yet.
Clipboard limitations: Windows clipboard is primary; OSC52 is capability-gated.
Unicode limitations: wide cells, combining marks, emoji modifiers, ZWJ sequences,
and regional-indicator flag pairs are handled defensively; exact Unicode database
parity is not claimed yet.
TUI limitations: alternate-screen apps are supported at a basic level, not tmux parity yet.
Admin shells: elevated/non-elevated process boundaries may prevent some IPC workflows.
Remote shells: local persistence is supported; remote attach is not implemented.
Nested terminals: running wmux inside another multiplexer is not a supported v1 target.
```

## Windows Console-Control Cleanup

`Ctrl+Break` and related Windows console-control events are cleanup signals, not
wmux keybindings. They are handled by the attached client platform layer and are
not routed through the terminal input decoder, prefix handling, or command
engine.

When an attached client receives `CTRL_BREAK_EVENT`, `CTRL_CLOSE_EVENT`,
`CTRL_LOGOFF_EVENT`, or `CTRL_SHUTDOWN_EVENT`, it records the event, wakes the
normal attach loop, sends a best-effort detach frame, and exits through the same
RAII cleanup path used by normal detach. If Windows delivers a `CTRL_C_EVENT` as
an actual console-control event, it uses the same defensive cleanup path; normal
keyboard `Ctrl+C` in raw attach mode is still decoded as pane input and is not a
wmux shortcut. The console-control callback must not mutate
daemon/session/window/pane/layout state and must not perform terminal cleanup
directly.

The client cleanup path must restore, idempotently:

```text
raw input mode
VT output mode
mouse reporting
bracketed paste
focus events
cursor visibility/style
alternate screen
console output code page
```

Manual verification:

```text
1. Start wmux attached in Windows Terminal.
2. Enable mouse/bracketed paste if supported.
3. Trigger Ctrl+Break or an equivalent console-control interruption.
4. Confirm the terminal remains usable.
5. Confirm cursor is visible.
6. Confirm mouse reporting and bracketed paste are off.
7. Reattach and confirm the daemon/session/panes are still alive.
```

## Doctor Command

Users should run this command first when reporting runtime bugs:

```powershell
wmux doctor
wmux doctor --json
```

The command reports:

```text
wmux version
Windows version/build
host terminal guess
shell guess
ConPTY availability
VT input mode
VT output mode
truecolor support guess
mouse support guess
bracketed paste support guess
clipboard backend
config path
log path
IPC path
daemon status
active sessions/windows/panes
```

`--json` is intended for bug-report automation and future CI diagnostics.

For keyboard, mouse, paste, or host-terminal translation bugs, also run:

```powershell
wmux debug-keys
```

`debug-keys` prints decoded event names, mouse coordinates, and paste byte
counts. It must not print pasted text content.

## Terminal Capability Model

The capability model is intentionally explicit:

```text
TerminalCapabilities
+-- host
+-- supports_truecolor
+-- supports_256_color
+-- supports_sgr_mouse
+-- supports_mouse_drag
+-- supports_mouse_wheel
+-- supports_bracketed_paste
+-- supports_focus_events
+-- supports_cursor_style
+-- supports_alt_screen
+-- supports_extended_keys
+-- supports_osc52_clipboard
+-- supports_synchronized_output
+-- quirks
```

Known host values:

```text
windows-terminal
vscode
wezterm
alacritty
conhost
unknown
```

Known quirks:

```text
broken-alt-key-sequences
ctrl-break-special-handling
mouse-wheel-encoding-differs
no-osc52-clipboard
vscode-key-translation
legacy-conhost-mode
cursor-style-unsupported
unknown-escape-sequences
```

## Capability Overrides

Overrides use the same tmux-style `.wmux.conf` syntax as the rest of wmux
configuration:

```text
set -g terminal-host windows-terminal
set -g terminal-truecolor on
set -g terminal-256-color on
set -g terminal-mouse on
set -g terminal-mouse-drag on
set -g terminal-mouse-wheel on
set -g terminal-bracketed-paste on
set -g terminal-focus-events off
set -g terminal-cursor-style on
set -g terminal-alt-screen on
set -g terminal-extended-keys on
set -g terminal-osc52-clipboard off
set -g terminal-synchronized-output off
set -g terminal-quirk-no-osc52-clipboard on
```

`terminal-host` accepts:

```text
windows-terminal
vscode
wezterm
alacritty
conhost
unknown
```

Quirk overrides use the `terminal-quirk-<name>` form where `<name>` is one of
the known quirk names above. `wmux doctor` reports both detected capabilities
and effective capabilities after overrides.

## Compatibility Test Matrix

Manual compatibility scripts should cover:

```text
Windows Terminal + pwsh
Windows Terminal + Windows PowerShell
Windows Terminal + cmd
VSCode integrated terminal + pwsh
WezTerm + pwsh
Alacritty + pwsh
```

For each pairing, verify:

```text
Ctrl+b prefix
arrow keys
modified arrows
Alt keys
function keys
mouse click
mouse drag
mouse wheel
resize events
copy/paste
truecolor
alternate screen app
terminal cleanup after crash
```
