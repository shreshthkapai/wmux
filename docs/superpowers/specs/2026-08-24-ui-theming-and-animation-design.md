# UI Theming And Animation Design

Date: 2026-08-24
Status: Approved for implementation planning

## Summary

wmux will support three progressively more expressive UI customization levels
for pane separators, active-pane highlighting, and the server-owned status row:

1. static built-in presets;
2. user-defined colours, styles, glyphs, status templates, and theme files;
3. bounded animations and optional one-shot external theme generators.

The current UI remains the exact default. Its foreground and background stay
terminal-default rather than fixed RGB values, so the separators normally look
white on a dark terminal and remain readable on a light terminal. The default
uses single inactive separators, heavy and bold active-pane edges, a
reverse-video status row, and no animation.

Static themes must add no idle timer, redraw, allocation, or provider process
while the UI is idle.
Animated themes run through the existing client-scoped render scheduler and
diff renderer. External generators never participate in the frame loop.

## Goals

- Preserve the current appearance and performance for every existing config.
- Apply the same customization model to pane borders and the bottom status row.
- Provide useful built-in themes without requiring a separate theme file.
- Support complete custom glyph, colour, style, status, and animation data.
- Permit user-authored programs to generate a theme once, then let wmux render
  and loop the validated result internally.
- Keep theme state server-owned, rendering client-scoped, and platform mechanics
  behind existing backend boundaries.
- Validate all untrusted theme bytes before they enter authoritative UI state.
- Degrade safely on invalid configuration, provider failure, slow clients, and
  terminals without suitable Unicode glyph support.

## Non-goals

- Pixel graphics or terminal-specific image protocols.
- Executing code embedded in a theme document.
- Loading native libraries into the server process.
- Giving a provider pane output, keystrokes, process handles, or mutable server
  state.
- Downloading themes or providers automatically.
- Running a provider once per frame.
- Letting decorative animation move pane geometry or cover pane content.
- Replacing the existing format engine with a general scripting language.

## Existing Architecture

The current structural renderer already owns the correct seam. A
`StructuralScene` contains pane rectangles, compact border spans, the active
pane, and the composed status line. Each attached client owns an independent
render baseline. The server's deadline scheduler coalesces structural changes
and pane output before the diff renderer serializes changed cells.

The theme feature extends this model. It does not move UI state into the
platform backend, client, or pane process. The core remains OS-neutral, the
server remains the state authority, and clients remain disposable render
targets.

## Selected Architecture

### Theme ownership

`wmux-core` will own the resolved theme domain model and pure rendering
semantics:

- `UiTheme`
- `UiFrame`
- `UiStyle`
- `BorderGlyphSet`
- `StatusTheme`
- `AnimationSpec`
- deterministic frame selection and status-template expansion

These values contain no paths, commands, JSON, clocks, or platform handles.
The core renderer receives a resolved `UiFrame` while building a structural
scene.

`wmux-config` will own configuration syntax, theme-file decoding, colour and
style parsing, schema-version checks, limits, preset selection, and layering.
It may depend on the OS-neutral core theme types; the core must not depend on
the config crate.

`wmux-server` will own the active resolved theme, provider lifecycle, atomic
reload, per-client animation state, animation deadlines, and redraw requests.
Provider execution will use the existing semantic job backend so process
mechanics and cleanup remain platform-specific.

No protocol-version change is required. Existing command messages can carry
the serialized server-side theme reload command, and existing output messages
already carry complete render transactions.

### Theme resolution order

The active theme is resolved once in this order:

```text
unchanged built-in default
  -> selected built-in preset
  -> optional static theme file
  -> optional one-shot provider result
  -> explicit config overrides
```

Later layers override only fields they specify. The fully resolved value has no
inheritance lookup during rendering. Explicit config overrides remain final so
a user can constrain or repair a generated theme without changing its provider.

## Built-in Presets

The initial built-in set will be:

- `default`: current terminal-native single/heavy UI with reverse status;
- `minimal`: low-decoration separators and a compact status treatment;
- `double`: connected double-line inactive and active separators;
- `neon`: vivid static colours with a compatible optional pulse;
- `sakura`: an anime-inspired palette and decorations with a compatible
  optional sweep.

Every preset must remain legible without animation. Built-in presets use the
portable indexed palette; custom documents may opt into RGB colours. Selecting
a preset does not enable its optional animation unless animation is explicitly
enabled.

## Main Configuration Surface

Common changes stay in `config.wmux` as flat keys that fit the existing parser:

```text
ui.theme = default
ui.theme_file = ""
ui.theme_provider = ""

ui.border.style = single
ui.border.foreground = default
ui.border.background = default
ui.active_border.style = heavy
ui.active_border.foreground = default
ui.active_border.background = default

ui.status.style = reverse
ui.status.foreground = default
ui.status.background = default

ui.animation = off
ui.animation_target = both
ui.animation_fps = 12
ui.animation_playback = loop
```

Omitted UI keys preserve current behavior. RGB values beginning with `#` must
be quoted in the line-oriented main config because `#` begins a comment outside
quotes.

Supported common values include:

- border style: `single`, `double`, `heavy`, `ascii`, `none`, or `custom`;
- colour: `default`, `ansi:N`, or `#RRGGBB`;
- style attributes: bold, dim, italic, underline, reverse, hidden, and
  strikethrough;
- animation: `off`, `pulse`, `sweep`, `shimmer`, `colour-cycle`, or `custom`;
- animation target: `border`, `status`, or `both`;
- playback: `once` or `loop`.

The generated default config will document these keys as comments while
leaving the effective values unchanged.

## Versioned Theme Document

Full themes use JSON so nested glyph maps, status segments, and frame sequences
remain explicit and generator-friendly. Static files and provider stdout use
the same schema.

An illustrative document is:

```json
{
  "schema": 1,
  "name": "sakura-night",
  "border": {
    "style": { "fg": "default", "bg": "default" },
    "glyphs": "single"
  },
  "active_border": {
    "style": { "fg": "#ff8fbd", "bg": "default", "bold": true },
    "glyphs": "heavy"
  },
  "status": {
    "base": { "fg": "default", "bg": "default", "reverse": true },
    "left": " wmux · {session} ",
    "window": " {window_index}:{window_name} ",
    "active_window": " [{window_index}:{window_name}] ",
    "right": " pane {pane_index} · {pane_title} ",
    "prompt": { "fg": "default", "bg": "default" }
  },
  "animation": {
    "target": "both",
    "playback": "loop",
    "fps": 12,
    "frames": [
      { "duration_ms": 84, "active_border": { "fg": "#ff8fbd" } },
      { "duration_ms": 84, "active_border": { "fg": "#ffd1e3" } }
    ]
  }
}
```

Unknown schema versions are rejected. Unknown fields are rejected by default
so misspelled style keys cannot silently produce a partial theme.

### Custom border glyphs

A custom glyph set contains one glyph for every topology used by connected
pane layouts:

- vertical and horizontal;
- four corners;
- four tees;
- crossing.

Inactive and active borders may select different built-in or custom glyph
sets. Every custom glyph must be exactly one displayed terminal cell. Control
characters, combining-only strings, newline, and width-two graphemes are
rejected. The `ascii` preset provides a portable fallback using `|`, `-`, and
`+`.

### Status customization

The bottom row exposes the same colour, attributes, decorations, and animation
capabilities as pane borders. It has independently styled base, left, ordinary
window, active-window, right, and prompt segments.

Supported template fields are bounded server metadata:

- `{session}`
- `{window_index}`
- `{window_name}`
- `{pane_index}`
- `{pane_title}`
- `{windows}` for the already bounded and centered window list

Control characters from metadata remain sanitized. Segment truncation and
centering retain the current collision rules, including the guarantee that
pane focus changes do not shift the centered window list. Animated decorative
strings must have the same displayed width in every frame.

Confirmation and editing prompts continue to replace the status row. Prompt
styling may change, but editing prompts must preserve the real cursor and its
grapheme-aware input position.

## One-shot Theme Providers

A provider is an explicitly configured user command. wmux starts it only at
server startup or explicit theme reload. The provider writes one versioned JSON
theme document to stdout and exits. After validation, wmux owns the resulting
immutable frames and performs all looping itself.

Providers are not discovered, downloaded, or enabled automatically. The
provider executes with the user's normal account permissions and is not a
sandbox; users must configure only commands they trust. wmux explicitly
supplies only a small documented environment containing the requested theme
schema and wmux version. It does not supply pane output, key input, session
contents, native handles, or mutable state.

Provider execution obeys these limits:

- 2-second wall-clock timeout;
- 64 KiB combined captured output;
- one JSON document;
- maximum 64 animation frames;
- termination and descendant cleanup through the job backend;
- nonzero exit, timeout, backend error, malformed UTF-8, malformed JSON, or
  validation failure leaves the current theme untouched.

Provider completion is an ordinary server event. It never blocks the event
loop or render path.

## Atomic Theme Reload

`wmux theme reload` will enqueue a serialized server-side reload command. It
will reread the configured theme source and rerun the optional provider without
terminating sessions or pane processes.

Resolution happens into a candidate value. Only a fully parsed and validated
candidate replaces the active theme. A successful swap invalidates structural
scene caches and requests one structural redraw for each attached client. A
failure preserves the last working theme and reports the precise field or
provider failure without partially applying any layer.

Concurrent reload requests are serialized. A newer request supersedes an older
provider result by generation number so stale output cannot replace a newer
theme.

## Animation Runtime

Each attached client owns a small animation state:

```text
theme generation
animation start
current frame
next frame deadline
playback mode
```

Frame selection is a pure function of the validated animation definition and
elapsed time. Tests inject explicit times; production uses the server's
monotonic clock.

The existing deadline scheduler gains animation deadlines. It schedules a
deadline only while an attached, writable client has an enabled animation.
Static themes have no deadline. Detached clients and clients under backpressure
do not advance queued frames. When a slow client becomes writable, wmux renders
the current frame directly and skips obsolete intermediate frames.

Animation changes rebuild the structural border/status layer and enter the
normal client-baseline diff path. Pane cells remain authoritative and are not
reparsed or recoloured. Pane output, input-priority redraws, and animation
changes due in the same cycle coalesce into one complete render transaction.

The limits are:

- maximum 30 frames per second;
- preset default of 12 frames per second or lower;
- maximum 64 frames;
- minimum frame duration derived from the 30 FPS cap;
- no geometry changes;
- stable displayed width for animated status decorations.

`once` playback stops scheduling after its final frame. `loop` playback
schedules only the next frame, not a periodic global tick. If no attached
client is animating, the server has no animation wakeup.

## Failure Handling

Theme errors are data errors, never panics. Errors include the source layer and
exact field path where possible. Examples include an unknown preset, invalid
colour, unsupported schema, missing topology glyph, non-cell-width glyph,
unstable animated status width, excessive frame rate, excessive frames,
provider timeout, and output limit violation.

At startup, a static file failure falls back to the unchanged default. The
server may initially show the resolved static theme while an asynchronous
provider runs; a valid result swaps atomically. Provider failure leaves that
static candidate active. During reload, any invalid candidate leaves the last
working theme active.

The server log records one concise diagnostic per failed load. Repeated render
frames must not repeat the same error.

## Performance Contract

The default static theme must preserve the existing performance model:

- no idle timer;
- no idle redraw;
- no per-frame parsing;
- no provider process;
- no pane-grid clone caused by theme state;
- no allocation per terminal cell;
- no output frame per animation step when no visible themed cell changed.

Resolved styles and glyphs are compact immutable values. Border spans remain
compact. Status templates are expanded only during structural composition.
Animation frames store deltas or resolved small UI values, not copies of pane
screens.

Performance tests will retain the current static thresholds and add an animated
scenario that verifies deadline capping, diff-sized output, frame coalescing,
and bounded allocation. Animation performance must never be used to relax a
static release gate.

## Testing Strategy

### Config and theme parsing

- Existing configs resolve to the exact current UI.
- Every preset parses and resolves deterministically.
- Layer precedence follows default, preset, file, provider, explicit override.
- Default, indexed, and RGB colours round-trip correctly.
- Unknown versions and fields fail with precise paths.
- Glyph topology, control-character, Unicode-width, frame-count, output-size,
  FPS, duration, and stable-width rules are enforced.

### Rendering

- Golden structural scenes cover every built-in preset.
- Inactive and active custom glyph maps preserve connected topology.
- Status base, left, windows, active window, right, and prompt styles compose
  correctly at narrow and wide sizes.
- Current-window centering remains stable across pane focus changes.
- Prompts preserve visibility and cursor positioning rules.
- Theme changes never mutate pane grid contents or pane generations.

### Animation and scheduling

- Explicit monotonic timestamps produce deterministic once and loop frames.
- Static themes create no animation deadline.
- Once animations stop on the final frame.
- Loop animations schedule only the next deadline.
- Detached and blocked clients create no animation backlog.
- Slow clients skip obsolete frames and resume from the current frame.
- Animation and pane damage coalesce into one client transaction.
- Multiple clients retain independent baselines and animation state.

### Provider and reload

- Valid provider output applies atomically.
- Nonzero exit, timeout, malformed output, size overflow, and backend error keep
  the working theme.
- Timed-out providers and descendants are terminated.
- Stale provider generations cannot overwrite newer reloads.
- Reload does not terminate sessions, panes, or clients.
- Provider behavior is tested through the semantic job backend on Windows and
  Unix, with portable semantics compiled and exercised in cross-OS CI.

### Release verification

- Formatting, strict linting, complete workspace tests, fuzz-harness checks,
  deterministic conformance, stress, and release performance gates remain
  required.
- The theme JSON decoder receives malformed-input fuzz coverage.
- Static and animated render benchmarks report separate results.

## Documentation

Public documentation will explain:

- terminal-native default colour behavior;
- built-in preset selection;
- common inline overrides;
- the complete versioned JSON schema;
- static and looping animation examples;
- one-shot provider configuration and limits;
- atomic reload;
- Unicode cell-width restrictions;
- performance implications of opt-in looping animation.

Examples will include a conservative custom theme, a colourful static theme,
an anime-inspired animated theme, and a minimal provider that generates a
looping theme document.

## Compatibility And Rollout

This feature is additive. Existing configuration, commands, sessions, IPC
messages, and default rendering remain valid. The default configuration does
not opt into a preset, external file, provider, or animation. Release archives
need no new runtime component.

The implementation will land in dependency order: core theme values and pure
rendering, config/schema resolution, static presets and customization,
client-scoped animation scheduling, provider/reload lifecycle, then complete
documentation and release verification.
