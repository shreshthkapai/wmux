# Scrollback And Mouse Routing

wmux follows tmux's ownership and routing model and zellij's Windows native
input boundary.

## Ownership

Pane history is canonical server state. Completed logical lines live in the
pane grid and are reflowed lazily by requested width. A client stores only its
own offset from the live bottom for each pane. It never owns or mutates pane
history.

This gives two clients attached to one session independent views. New PTY
output continues to update the pane while either client is scrolled. The
server advances a scrolled client's offset by the pane's monotonic
history-added counter, preserving the viewed content until history eviction
makes that impossible.

Historical rendering uses a `PaneViewport` snapshot derived from the
authoritative grid. It enters the normal scene diff renderer, hides the live
application cursor in a scrolled pane, and does not replay PTY bytes or move
the pane cursor. Returning to offset zero resumes the ordinary damage-journal
path.

## Structured Input

Platform backends produce `wmux_platform::MouseEvent` values with zero-based
cell coordinates, event kind, button, and modifiers. Protocol version 5 carries
that event as a fixed-size frame. No platform console records or pre-encoded
mouse escape strings cross the boundary.

The Windows client requests native mouse and resize input while explicitly
clearing Quick Edit, line input, echo, and processed input. It enables xterm
any-event plus SGR reporting on the outer terminal for the duration of an
attachment and restores both output and console input modes on exit.

## Routing

The state owner hit-tests the event against the current structural scene:

1. If the target pane has requested DEC mouse tracking, wmux translates the
   event to pane-relative coordinates and sends it to that pane.
2. Otherwise, wheel-up moves that client's target-pane viewport five rows into
   history, matching tmux's default wheel binding.
3. Wheel-down moves five rows toward the live bottom and leaves history view at
   offset zero.
4. In copy mode, wheel and left-drag events are consumed by the client's
   server-owned copy state before application routing.
5. Non-wheel events without application mouse mode or copy mode remain
   available for later UI routing.

Supported application modes are X10 (`9`), normal (`1000`), button-event
(`1002`), any-event (`1003`), UTF-8 coordinates (`1005`), SGR (`1006`), and
urxvt (`1015`). Motion and release filtering follows the requested tracking
level. SGR is preferred when requested, followed by urxvt, UTF-8, and legacy
xterm encoding.

Mouse wheel input is therefore never translated into arrow keys or tied to a
specific terminal emulator. Applications that request mouse input receive it;
all other wheel input is multiplexer history navigation.

Copy-mode navigation, selection, search, and clipboard transfer are documented
in `docs/copy-mode.md`.

## Reference Model

- tmux: `server_client_check_mouse`, `input_key_get_mouse`,
  `input_key_mouse`, and the `WheelUpPane`/`WheelDownPane` default bindings.
- zellij: `stdin_handler_windows.rs` native console mode setup and normalized
  crossterm mouse events.
- Existing wmux lazy history, structural scene, damage journal, and independent
  client baseline contracts remain authoritative.
