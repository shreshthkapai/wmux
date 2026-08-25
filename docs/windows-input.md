# Windows Client Input

The Windows attach client uses a two-stage input model:

```text
Windows console event reader
  -> structured key, paste, and resize events
  -> wmux prefix/key-table handling
  -> versioned IPC key or paste message
  -> authoritative pane mode translation
  -> ConPTY input
```

`wmux-windows` uses crossterm's Windows event parser rather than maintaining a
second hand-written `INPUT_RECORD` decoder. This preserves keyboard-layout and
UTF-16 surrogate handling. Crossterm's native Win32 parser does not aggregate
console paste records, so wmux converts native paste shortcuts into one
`CF_UNICODETEXT` event. It supports `Ctrl+V` and `Ctrl+Shift+V`.
Clipboard-open contention is retried briefly and never closes the attach
client. Terminals that emit `Event::Paste` are accepted directly. Paste
payloads never pass through the wmux prefix-key state machine.

The server retains paste as a semantic event until it reads the active pane's
authoritative bracketed-paste mode. It then emits either the payload or
`ESC[200~`, payload, `ESC[201~` as one ordered pane-input operation. This
keeps terminal input decoding in the client while pane mode and PTY translation
remain server concerns.

During attachment, wmux requests report-all keyboard events together with
terminal keyboard disambiguation and restores the previous mode on exit. Plain
Enter remains carriage return, Alt+Enter remains the legacy escape-plus-return
sequence, Ctrl+J remains line feed, and other modified Enter events retain their
identity through CSI-u encoding. This prevents terminal hosts from collapsing
distinct modified keys before the server routes them to the pane.

The pane screen also tracks private input mode `9001` as authoritative terminal
state. When a pane enables that mode, the server translates each routed
semantic key into bounded `Vk;Sc;Uc;Kd;Cs;Rc` records, including paired key-down
and key-up records. This preserves control, Alt, Shift, navigation, and function
key identity for Windows-native applications launched through Unix interop
without moving platform handles or console records into the core.

An input-reader failure is sent to the attach loop as an error. Channel closure
is also an error. Neither condition is treated as a user detach, preventing an
unread tail of console input from being silently consumed by the parent shell.
