# Copy Mode

wmux copy mode follows tmux's ownership model: pane history remains in the
server-owned terminal grid, while each attached client owns an independent
cursor, viewport, selection, rectangle flag, and search state. Entering copy
mode never pauses pane output and never copies terminal pixels back from the
client.

## Input Routing

`Ctrl-b [` sends the tmux `copy-mode` command. While a client is in copy mode,
its key and mouse events are consumed by the server key table before PTY input.
Other clients attached to the same session continue to control and render the
pane independently.

The implemented tmux-compatible bindings include:

```text
q, Escape, Ctrl-c       cancel
arrows, h/j/k/l         move cursor
PageUp/PageDown         move one page
Ctrl-u/Ctrl-d           move half a page
g/G                     history top/bottom
0, ^, $, Home, End      line navigation
b/w                     previous/next word
Space, Ctrl-Space       begin selection
v                       rectangle selection toggle
Enter, Ctrl-w, Alt-w    copy selection and cancel
/, ?                    search forward/backward
n/N                     repeat/reverse the last search
mouse wheel             move through copy history
left drag/release       extend selection, copy, and cancel
```

Search input is rendered in the pane's final row until Enter or Escape. Search,
selection extraction, and viewport materialization all operate on the same
canonical history lines used by scrollback; no raw PTY output is replayed.

## Clipboard Boundary

Copy completion emits a versioned `Clipboard` IPC message. The disposable
client asks its platform backend to claim the system clipboard. On Windows this
uses `CF_UNICODETEXT`; the server and core contain no Windows handles or
clipboard APIs. Future Unix and macOS clients implement the same semantic
message with their native clipboard integration.

## Reference Model

- tmux `window-copy.c` for per-client mode cursor, selection, search, and
  history navigation semantics.
- tmux `key-bindings.c` for the `copy-mode` and `copy-mode-vi` tables.
- zellij server selection/search actions for keeping copy state beside the
  authoritative terminal pane rather than in the terminal frontend.
