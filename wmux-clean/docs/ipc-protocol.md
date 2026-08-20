# IPC Protocol

Protocol version: 5
Wire magic: WMX5
Maximum frame payload: 16777216 bytes

wmux IPC is a versioned, framed byte protocol between disposable clients and
the persistent server. Protocol constants and codecs live in `wmux-protocol`;
the transport is selected by the platform backend. Windows uses a local named
pipe whose endpoint and access checks are bound to the current user token SID.

## Frame format

Every frame has a 9-byte header followed by the declared payload:

| Offset | Size | Field | Encoding |
| --- | ---: | --- | --- |
| 0 | 4 | magic | ASCII `WMX5` |
| 4 | 1 | message tag | unsigned byte |
| 5 | 4 | payload length | little-endian `u32` |

The payload length excludes the header and may not exceed 16 MiB. Integer
payload fields are little-endian. Text payloads are UTF-8. Byte-oriented input,
output, paste, key, and clipboard payloads are opaque and may contain any byte.

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
| 11 | `Key` | opaque decoded-key bytes |
| 12 | `Paste` | opaque paste bytes |
| 13 | `Mouse` | kind `u8`, button `u8`, modifiers `u8`, column `u16`, row `u16` |
| 14 | `Clipboard` | opaque clipboard bytes |

`Hello` and `HelloOk` payloads are exactly 12 bytes. Capability bit 0 means
synchronized output and bit 1 means scroll-region support; unknown bits are
preserved but do not grant behavior the receiver does not implement. Mouse
payloads are exactly 7 bytes. `Resize` is exactly 4 bytes. `Detach` and
`Shutdown` reject non-empty payloads.

## Handshake and identity

The first client frame must be `Hello` with version 5. A compatible server
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
invalid UTF-8 text, invalid mouse values, and payloads whose fixed length is
wrong. An incomplete payload is an I/O error. EOF while reading the next frame
header is treated as a client disconnect. These failures close only that
connection; malformed IPC must not panic or mutate server state outside the
serialized owner loop.

The server may emit multiple ordinary frames before a command result. During
graceful shutdown, the requesting client receives its complete `CommandOk`
frame and other attached clients receive a complete `Shutdown` frame before
their connection writers return.
