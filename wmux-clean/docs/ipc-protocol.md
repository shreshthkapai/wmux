# IPC Protocol

Protocol version: 7
Wire magic: WMX7
Maximum frame payload: 16777216 bytes

wmux IPC is a versioned, framed byte protocol between disposable clients and
the persistent server. Protocol constants and codecs live in `wmux-protocol`;
the transport is selected by the platform backend. Windows uses a local named
pipe whose endpoint and access checks are bound to the current user token SID.

## Frame format

Every frame has a 9-byte header followed by the declared payload:

| Offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 4 | magic | ASCII `WMX7` |
| 4 | 1 | message tag | unsigned byte |
| 5 | 4 | payload length | little-endian `u32` |

The payload length excludes the header and may not exceed 16 MiB. Integer
payload fields are little-endian. Text payloads are UTF-8. Byte-oriented input,
output, paste, key raw bytes, and clipboard payloads may contain any byte.

## Message tags

| Tag | Message | Payload |
| ---: | --- | --- |
| 1 | `Hello` | version `u32`, diagnostic PID `u32`, capability bits `u32` |
| 2 | `HelloOk` | version `u32`, diagnostic PID `u32`, capability bits `u32` |
| 3 | `Command` | UTF-8 command text |
| 4 | `CommandOk` | UTF-8 result text |
| 5 | `CommandErr` | UTF-8 error text |
| 6 | `Input` | opaque terminal input bytes |
| 7 | `Output` | opaque rendered terminal output bytes |
| 8 | `Resize` | columns `u16`, rows `u16` |
| 9 | `Detach` | empty |
| 10 | `Shutdown` | empty |
| 11 | `Key` | semantic key prefix followed by opaque original bytes |
| 12 | `Paste` | opaque paste bytes |
| 13 | `Mouse` | kind `u8`, button `u8`, modifiers `u8`, column `u16`, row `u16` |
| 14 | `Clipboard` | opaque clipboard bytes |
| 15 | `EnterControl` | empty |
| 16 | `ControlCommand` | sequence `u64`, UTF-8 command text |
| 17 | `ControlRecord` | structured record tag and record payload |

`Hello` and `HelloOk` payloads are exactly 12 bytes. Capability bit 0 means
synchronized output and bit 1 means scroll-region support; unknown bits are
preserved but do not grant behavior the receiver does not implement. Mouse
payloads are exactly 7 bytes. `Resize` is exactly 4 bytes. `Detach` and
`Shutdown` reject non-empty payloads.

### Semantic key payload

`Key` begins with a fixed 6-byte prefix and retains the exact application bytes
produced by the client after it:

| Offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 1 | key tag | unsigned byte |
| 1 | 1 | modifiers | bitset |
| 2 | 4 | key value | little-endian `u32` |
| 6 | remaining | raw | opaque original key bytes |

Key tag 0 is a Unicode scalar and stores the scalar in `key value`. Tags 1
through 15 are Left, Right, Up, Down, Home, End, PageUp, PageDown, Backspace,
Delete, Insert, Enter, Tab, BackTab, and Escape; their key value must be zero.
Tag 16 is a function key and its key value must be from 1 through 24.

Modifier bits 0 through 3 mean Shift, Alt, Control, and Super. Other modifier
bits, unknown key tags, invalid Unicode scalars, nonzero fixed-key values, and
out-of-range function keys are rejected. The raw suffix is not interpreted by
the protocol and may be empty.

## Handshake and identity

The first client frame must be `Hello` with version 7. A compatible server
answers `HelloOk` before accepting commands. A numeric mismatch returns a
protocol-version error naming both versions. Invalid magic means an
incompatible service owns the endpoint; the client reports that condition and
asks the user to stop it rather than retrying startup.

On Windows, `Hello.pid` is diagnostic only. The server derives peer identity
from the connected named-pipe client's impersonation token and compares its
user SID with the SID that owns the protected endpoint. Verification happens
before `HelloOk` and before client registration. No PID, username environment
variable, or protocol field is an authorization input.

## Malformed input and disconnects

The decoder rejects bad magic, payloads larger than the maximum, unknown tags,
invalid UTF-8 text, invalid mouse values, malformed semantic keys, and payloads
whose fixed length is wrong. An incomplete payload is an I/O error. EOF while
reading the next frame header is treated as a client disconnect. These failures
close only that connection; malformed IPC must not panic or mutate server state outside the
serialized owner loop.

The server may emit multiple ordinary frames before a command result. During
graceful shutdown, the requesting client receives its complete `CommandOk`
frame and other attached clients receive a complete `Shutdown` frame before
their connection writers return.

## Control records

After the normal handshake, `EnterControl` changes one disposable client into
a structured control consumer. The server answers with a `Ready` record.
`ControlCommand` sequences must increase monotonically. Each accepted command
produces `Begin`, zero or more ordered notifications or output records, and one
terminal `End` or `Error` record.

The first control-record byte identifies `Ready`, `Begin`, `Output`,
`Notification`, `End`, `Error`, or `Pause`. Sequences and stable object IDs are
little-endian `u64` values. Pane output remains arbitrary bytes and is limited
to 65,536 bytes per record. Command, result, and error text is bounded to 1
MiB. Optional notification names are length-prefixed and bounded to 65,536
bytes. Invalid tags, truncated integers or lengths, trailing fixed-record
bytes, invalid textual UTF-8, and over-limit fields are rejected.
