# Windows Client Input

The Windows attach client uses a two-stage input model:

```text
Windows console event reader
  -> structured key, paste, mouse, and resize events
  -> wmux prefix/key-table handling
  -> versioned IPC semantic message
  -> authoritative pane mode translation
  -> ConPTY input
```

`wmux-windows` reads Unicode `INPUT_RECORD` values directly with
`ReadConsoleInputW` and normalizes them before IPC. For printable keys, the
record's translated `UnicodeChar` is authoritative; virtual-key codes are used
for navigation, function keys, and control combinations that carry no printable
character. This keeps keyboard-layout output intact, including the shifted
digit row, IME-only text records, and UTF-16 surrogate pairs. The decoder also
owns native mouse-button transitions and resize records, so the complete
Windows client input boundary is deterministic and unit-testable.

Native paste shortcuts become one `CF_UNICODETEXT` event. Wmux supports
`Ctrl+V` and `Ctrl+Shift+V`; clipboard-open contention is retried briefly and
never closes the attach client. Paste payloads never pass through the wmux
prefix-key state machine.

Printable punctuation retains both its semantic character and exact UTF-8
application bytes. In particular, a plain leading `&` reaches the active pane
as one `0x26` byte whether typed or pasted. Only prefix followed by `&` invokes
the configured window action. Shift metadata from the keyboard layout does not
turn an unprefixed printable symbol into a multiplexer command.

The server retains paste as a semantic event until it reads the active pane's
authoritative bracketed-paste mode. It then emits either the payload or
`ESC[200~`, payload, `ESC[201~` as one ordered pane-input operation. This
keeps terminal input decoding in the client while pane mode and PTY translation
remain server concerns.

During attachment, wmux requests report-all keyboard events together with
terminal keyboard disambiguation and restores the previous mode on exit. Plain
Enter remains carriage return, Alt+Enter remains the legacy escape-plus-return
sequence, and both Ctrl+J and Ctrl+Enter produce line feed. This gives
multiline terminal applications the same default chord across native Windows
and Unix attach clients. Shift+Enter and other modified Enter events retain
their identity through CSI-u encoding when the terminal host reports them.

Mouse input is normalized without changing event order, cell coordinates,
button identity, or Shift/Alt/Control modifiers. Down, drag, up, and wheel
events use the same OS-neutral protocol representation as the Unix client, so
selection and application-mouse policy remain server-owned.

The pane screen also tracks private input mode `9001` as authoritative terminal
state. When a pane enables that mode, the server translates each routed
semantic key into bounded `Vk;Sc;Uc;Kd;Cs;Rc` records, including paired key-down
and key-up records. This preserves control, Alt, Shift, navigation, and function
key identity for Windows-native applications launched through Unix interop
without moving platform handles or console records into the core.

An input-reader failure is sent to the attach loop as an error. Channel closure
is also an error. Neither condition is treated as a user detach, preventing an
unread tail of console input from being silently consumed by the parent shell.
