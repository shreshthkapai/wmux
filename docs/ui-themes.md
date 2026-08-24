# UI Themes And Animation

wmux can style pane borders and the server-owned status row without changing
pane applications or their colours. The default theme remains static and uses
the host terminal's default foreground, background, palette, and font.

## Quick Start

Add these keys to the file shown by `wmux config path`:

```text
ui.theme = sakura
ui.animation = sweep
ui.animation_target = both
ui.animation_fps = 12
ui.animation_playback = loop
```

Apply a saved change to a running server atomically:

```sh
wmux theme reload
```

An invalid reload reports the exact field that failed and keeps the active
theme and every pane intact.

## Built-In Presets And Animations

`ui.theme` accepts five presets:

| Preset | Appearance |
| --- | --- |
| `default` | Terminal-default colours, single inactive borders, heavy active edge, reverse status row |
| `minimal` | Dim single borders and a compact status layout |
| `double` | Double-line borders with a high-contrast active edge |
| `neon` | Bright palette-index border and status accents |
| `sakura` | Pink palette-index accents and floral status decorations |

The default preset does not select a font or fixed RGB colour. `default`
colours inherit the terminal's current foreground or background, and `ansi:N`
colours use its configurable 0-255 palette. The host terminal always owns font
selection.

`ui.animation` accepts `off`, `pulse`, `sweep`, `shimmer`, `colour-cycle`, or
`custom`. `color-cycle` and `none` are accepted aliases. `custom` requires
frames supplied by a theme file or provider. The remaining animation keys are:

| Key | Values | Default when animation is enabled |
| --- | --- | --- |
| `ui.animation_target` | `border`, `status`, `both` | `both` |
| `ui.animation_fps` | `1` through `30` | `12` |
| `ui.animation_playback` | `once`, `loop` | `loop` |

`once` holds the final frame. `loop` repeats in wmux; an external provider is
never rerun for each frame or cycle. Static themes create no animation timer.

## Main Configuration Keys

Sources are layered in this order, with later values winning:

```text
built-in preset -> theme file -> provider document -> main-config overrides
```

The complete source and override keys are:

```text
ui.theme
ui.theme_file
ui.theme_provider

ui.border.style
ui.border.foreground
ui.border.background
ui.border.attributes

ui.active_border.style
ui.active_border.foreground
ui.active_border.background
ui.active_border.attributes

ui.status.style
ui.status.foreground
ui.status.background
ui.status.left
ui.status.center
ui.status.window
ui.status.active_window
ui.status.right

ui.animation
ui.animation_target
ui.animation_fps
ui.animation_playback
```

Border style values are `single`, `heavy`, `double`, `ascii`, or `none`.
Custom glyph maps belong in a theme document. Attribute values are a
comma-separated combination of `bold`, `dim`, `italic`, `underline`,
`reverse`, `hidden`, and `strikethrough`. `default` or `none` clears all seven
attributes. Colours use `default`, `ansi:0` through `ansi:255`, or `#RRGGBB`.

Theme-file paths relative to the main configuration file are resolved from
that file's directory.

## Theme Document Schema

A theme file and provider output use strict JSON schema version 1. Unknown
fields are rejected. The document limit is 64 KiB and the name limit is 128
bytes.

```json
{
  "schema": 1,
  "name": "violet",
  "border": {
    "style": { "fg": "default", "dim": true },
    "glyphs": "single",
    "visible": true
  },
  "active_border": {
    "fg": "#ff66cc",
    "bold": true,
    "glyphs": "heavy"
  },
  "status": {
    "base": { "fg": "default", "bg": "default", "reverse": true },
    "left_style": { "bold": true },
    "center_style": { "fg": "ansi:13" },
    "window_style": { "dim": true },
    "active_window_style": { "bold": true },
    "right_style": { "italic": true },
    "prompt": { "underline": true },
    "left": " wmux · {session} ",
    "center": "{windows}",
    "window": " {window_index}:{window_name} ",
    "active_window": " [{window_index}:{window_name}] ",
    "right": " pane {pane_index} · {pane_title} "
  },
  "animation": {
    "target": "both",
    "playback": "loop",
    "fps": 12,
    "frames": [
      { "active_border": { "fg": "#ff66cc" } },
      { "active_border": { "fg": "#66ccff" } }
    ]
  }
}
```

Every style object accepts `fg`, `bg`, `bold`, `dim`, `italic`, `underline`,
`reverse`, `hidden`, and `strikethrough`. Border objects also accept those
style fields directly, as shown by `active_border` above.

`glyphs` accepts `single`, `heavy`, `double`, `ascii`, or an object containing
all eleven fields below:

```json
{
  "vertical": "|",
  "horizontal": "-",
  "down_right": "+",
  "down_left": "+",
  "up_right": "+",
  "up_left": "+",
  "vertical_right": "+",
  "vertical_left": "+",
  "horizontal_down": "+",
  "horizontal_up": "+",
  "cross": "+"
}
```

Each glyph must be exactly one displayed terminal cell. Animation frames may
patch `duration_ms`, `border`, `active_border`, and `status`. A frame duration
must be at least 34 ms, and a document may contain at most 64 frames. Status
decorations must retain the same displayed width in every animation frame so
the window list never shifts.

Status templates are limited to 1,024 bytes and accept these fields:

```text
{session} {windows} {window_index} {window_name} {pane_index} {pane_title}
```

## One-Shot Providers

`ui.theme_provider` is an explicitly configured command. It is trusted user
code with the same permissions as wmux; wmux does not download or sandbox it.
The command runs once at server startup and once for each `wmux theme reload`,
prints exactly one schema-1 JSON document, and exits successfully. Its complete
captured output must be UTF-8 JSON, so do not print diagnostics alongside it.

wmux supplies `WMUX_THEME_SCHEMA=1` and `WMUX_VERSION`. Provider execution is
limited to 2 seconds and 64 KiB of captured output. A timeout, nonzero exit,
backend failure, malformed output, or superseded reload leaves the previous
theme active. Only the newest completed generation may commit.

Example PowerShell provider, saved as `theme-provider.ps1`:

```powershell
@'
{"schema":1,"name":"generated","active_border":{"fg":"#ff66cc"}}
'@ | Write-Output
```

Configure it with an absolute path appropriate for the machine:

```text
ui.theme_provider = powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\me\.config\wmux\theme-provider.ps1
```

The provider may return animation frames. wmux stores those validated frames
and performs `once` or `loop` playback internally without starting the command
again.

## Runtime And Performance Rules

Animation state is client-scoped. Each attached client receives the current
frame for its own known render baseline. If a client is blocked, wmux does not
queue obsolete frames or spin on expired deadlines; it renders one coherent
current frame when the client becomes writable. The frame-rate ceiling is 30
FPS, and static defaults retain the existing no-idle-redraw path.
