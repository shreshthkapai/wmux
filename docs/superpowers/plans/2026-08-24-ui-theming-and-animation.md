# UI Theming And Animation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add backward-compatible built-in themes, complete custom border/status styling, deterministic animations, and one-shot external theme generators without adding idle work to the default UI.

**Architecture:** `wmux-core` owns immutable resolved theme values and pure frame/composition logic; `wmux-config` owns the flat config adapter, versioned JSON patches, presets, validation, and layering; `wmux-server` owns the active theme generation, atomic reload, provider jobs, and client-scoped animation deadlines. The existing structural-scene and client-baseline diff path remains the only physical UI renderer.

**Tech Stack:** Rust 2021 with Rust 1.96, existing terminal grid/render scheduler/job backend, `serde` and `serde_json` for the bounded theme document, cargo-fuzz for malformed theme input.

**Spec:** `docs/superpowers/specs/2026-08-24-ui-theming-and-animation-design.md`

## Global Constraints

- Work directly on `main`; do not create a feature branch or worktree.
- Do not use subagents. Execute inline in dependency order.
- Preserve the untracked `.agents/` directory without adding, editing, or deleting it.
- Keep workspace version `1.0.1` and do not move existing release tags.
- Do not introduce competitor product names into tracked source, docs, tests, variables, or comments.
- The default remains terminal-default foreground/background, single inactive separators, heavy/bold active edges, reverse-video status, and no animation.
- Existing config files and IPC messages remain valid; do not bump the protocol version.
- Static themes create no idle deadline, redraw, allocation, or provider process.
- Animation is capped at 30 FPS and 64 frames; provider output is capped at 64 KiB and provider runtime at 2 seconds.
- Invalid theme data never replaces a working theme and never panics the server.
- Custom border glyphs must be exactly one displayed cell; animated status decorations must keep a stable displayed width.
- All server mutations, including reload completion, remain serialized through the owner loop.
- Add tests before implementation for every task and commit each completed task directly on `main`.

## File Structure

- Create `crates/wmux-core/src/theme.rs`: resolved theme values, glyph topology, status templates, deterministic animation frame selection.
- Modify `crates/wmux-core/src/lib.rs`: export the theme API.
- Modify `crates/wmux-core/src/render.rs`: compose borders, status, overlays, and structural scenes from an explicit `UiFrame` while preserving default wrappers.
- Modify `Cargo.toml`: add workspace `serde` and `serde_json` dependencies.
- Modify `Cargo.lock`: lock the new serialization dependencies.
- Create `crates/wmux-config/src/theme.rs`: JSON schema, theme patches, built-in presets, validation, file/provider layering, and precise errors.
- Modify `crates/wmux-config/src/lib.rs`: parse `ui.*` keys, resolve relative theme paths, expose effective UI config.
- Modify `crates/wmux-config/Cargo.toml`: depend on core, serde, and serde_json.
- Modify `crates/wmux-core/src/command/mod.rs`: add and parse the serialized `reload-theme` command/effect.
- Modify `crates/wmux-core/src/command/execute.rs`: emit the reload effect.
- Modify `crates/wmux-cli/src/lib.rs`: adapt `wmux theme reload` to `reload-theme` and document it in help.
- Modify `crates/wmux-client/src/lib.rs`: show effective theme settings without changing server transport.
- Create `crates/wmux-server/src/theme_runtime.rs`: active generation, candidate resolution, per-client animation clock, provider generations, and deadlines.
- Modify `crates/wmux-server/src/lib.rs`: integrate themed scene composition, reload effects, animation wakeups, provider job events, and atomic redraws.
- Modify `crates/wmux-core/src/jobs.rs`: allow bounded output per job and identify theme-provider continuations.
- Modify `crates/wmux-bench/src/main.rs`: add an animated-UI scenario and rejection checks without relaxing static gates.
- Create `fuzz/fuzz_targets/theme_json.rs`: bounded malformed theme-document fuzz target.
- Add `fuzz/corpus/theme_json/minimal-theme` and `fuzz/corpus/theme_json/malformed-theme`: valid and malformed seeds.
- Modify `fuzz/Cargo.toml`, `fuzz/README.md`, and `.github/workflows/beta-core.yml`: compile, lint, and smoke the theme fuzz target.
- Create `docs/ui-themes.md`: public presets, schema, animation, provider, reload, and safety documentation.
- Modify `README.md`, `docs/hybrid-rendering.md`, and `docs/performance.md`: link and record the final behavior and performance evidence.

---

### Task 1: Core Theme Values And Deterministic Frames

**Files:**

- Create: `crates/wmux-core/src/theme.rs`
- Modify: `crates/wmux-core/src/lib.rs`

**Interfaces:**

- Consumes: existing `wmux_core::{Color, Style, scalar_width}`.
- Produces: `UiTheme`, `UiFrame`, `BorderTheme`, `BorderGlyphSet`, `StatusTheme`, `AnimationSpec`, `AnimationTarget`, `Playback`, `FrameSelection`, and the built-in glyph constants used by later tasks.

- [ ] **Step 1: Write failing default, topology, and animation tests**

Add unit tests in `theme.rs` with explicit expectations:

```rust
#[test]
fn default_theme_preserves_current_terminal_native_ui() {
    let theme = UiTheme::default();
    assert_eq!(theme.base.border.style, Style::default());
    assert_eq!(theme.base.border.glyphs, BorderGlyphSet::SINGLE);
    assert_eq!(theme.base.active_border.glyphs, BorderGlyphSet::HEAVY);
    assert!(theme.base.active_border.style.bold);
    assert!(theme.base.status.base.reverse);
    assert!(theme.animation.is_none());
}

#[test]
fn glyph_sets_resolve_every_connected_topology() {
    assert_eq!(BorderGlyphSet::SINGLE.glyph(UP | DOWN), '│');
    assert_eq!(BorderGlyphSet::SINGLE.glyph(LEFT | RIGHT), '─');
    assert_eq!(BorderGlyphSet::DOUBLE.glyph(UP | RIGHT | DOWN | LEFT), '╬');
    assert_eq!(BorderGlyphSet::ASCII.glyph(UP | RIGHT | DOWN | LEFT), '+');
}

#[test]
fn once_and_loop_frame_selection_are_deterministic() {
    let frames = vec![test_frame(100), test_frame(200)];
    let once = AnimationSpec::new(AnimationTarget::Both, Playback::Once, frames.clone()).unwrap();
    assert_eq!(once.select(0).index, 0);
    assert_eq!(once.select(100).index, 1);
    assert_eq!(once.select(300).next_in_ms, None);
    let looping = AnimationSpec::new(AnimationTarget::Both, Playback::Loop, frames).unwrap();
    assert_eq!(looping.select(300).index, 0);
    assert_eq!(looping.select(350).next_in_ms, Some(50));
}
```

- [ ] **Step 2: Run the focused tests and confirm the new API is missing**

Run:

```powershell
cargo test -p wmux-core theme::tests --locked
```

Expected: compilation fails because `theme` and its types are not defined.

- [ ] **Step 3: Implement the minimal resolved theme model**

Define the stable topology bits and public values in `theme.rs`:

```rust
pub const UP: u8 = 1;
pub const RIGHT: u8 = 2;
pub const DOWN: u8 = 4;
pub const LEFT: u8 = 8;
pub const MAX_THEME_FRAMES: usize = 64;
pub const MAX_THEME_FPS: u8 = 30;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct BorderGlyphSet {
    pub vertical: char,
    pub horizontal: char,
    pub down_right: char,
    pub down_left: char,
    pub up_right: char,
    pub up_left: char,
    pub vertical_right: char,
    pub vertical_left: char,
    pub horizontal_down: char,
    pub horizontal_up: char,
    pub cross: char,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BorderTheme {
    pub style: Style,
    pub glyphs: BorderGlyphSet,
    pub visible: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct StatusTheme {
    pub base: Style,
    pub left_style: Style,
    pub center_style: Style,
    pub window_style: Style,
    pub active_window_style: Style,
    pub right_style: Style,
    pub prompt_style: Style,
    pub left: String,
    pub center: String,
    pub window: String,
    pub active_window: String,
    pub right: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UiFrame {
    pub duration_ms: u32,
    pub border: BorderTheme,
    pub active_border: BorderTheme,
    pub status: StatusTheme,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UiTheme {
    pub name: String,
    pub base: UiFrame,
    pub animation: Option<AnimationSpec>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FrameSelection {
    pub index: usize,
    pub next_in_ms: Option<u32>,
}
```

Make `UiTheme::default()` reproduce the existing styles and text templates.
Implement `AnimationSpec::new` with the 64-frame and 30-FPS duration checks,
and `AnimationSpec::select(elapsed_ms)` without reading a clock.

- [ ] **Step 4: Export the API and pass focused/core tests**

Add `pub mod theme;` and re-exports to `lib.rs`. Run:

```powershell
cargo test -p wmux-core theme::tests --locked
cargo test -p wmux-core --lib --locked
```

Expected: all core tests pass.

- [ ] **Step 5: Commit Task 1**

```powershell
git add crates/wmux-core/src/theme.rs crates/wmux-core/src/lib.rs
git commit -m "feat(core): add resolved UI theme model"
```

### Task 2: Theme-aware Structural Composition

**Files:**

- Modify: `crates/wmux-core/src/render.rs`
- Modify: `crates/wmux-core/src/lib.rs`

**Interfaces:**

- Consumes: `UiFrame`, `BorderGlyphSet::glyph(u8)`, and `StatusTheme` from Task 1.
- Produces: `build_window_structure_with_theme(...)` and `build_window_scene_with_theme(...)`; all existing scene builders remain source-compatible wrappers over `UiTheme::default()`.

- [ ] **Step 1: Add failing default-parity and custom-scene tests**

Add renderer tests that assert both backward compatibility and customization:

```rust
#[test]
fn explicit_default_frame_matches_legacy_scene_exactly() {
    let (state, session) = split_test_state();
    let legacy = build_window_structure(&state, session, 40, 12).unwrap();
    let themed = build_window_structure_with_theme(
        &state,
        session,
        40,
        12,
        &UiTheme::default().base,
    )
    .unwrap();
    assert_eq!(legacy, themed);
}

#[test]
fn custom_frame_styles_borders_and_every_status_segment() {
    let (state, session) = split_test_state();
    let frame = custom_test_frame();
    let scene = build_window_scene_with_theme(
        &state,
        session,
        40,
        12,
        PaneSceneOverrides::empty(),
        None,
        &frame,
    )
    .unwrap();
    assert_scene_has_custom_border_and_status(&scene, &frame);
}
```

Add a regression that changes pane focus and proves the centered window list
starts in the same column for both active panes.

- [ ] **Step 2: Run the focused renderer tests and observe missing functions**

```powershell
cargo test -p wmux-core render::tests::explicit_default_frame_matches_legacy_scene_exactly --locked
cargo test -p wmux-core render::tests::custom_frame_styles_borders_and_every_status_segment --locked
```

Expected: compilation fails for the new theme-aware builders.

- [ ] **Step 3: Thread an explicit frame through structural composition**

Add:

```rust
pub fn build_window_structure_with_theme(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
    frame: &UiFrame,
) -> Option<StructuralScene>;

pub fn build_window_scene_with_theme(
    state: &ServerState,
    session: SessionId,
    cols: u16,
    rows: u16,
    overrides: PaneSceneOverrides<'_>,
    overlay: Option<ClientOverlay<'_>>,
    frame: &UiFrame,
) -> Option<Scene>;
```

Change `BorderSpan` to store the already selected `Style`. Select inactive or
active glyphs with `BorderGlyphSet::glyph(directions)` instead of the hardcoded
glyph function. Preserve the current two-pane half-border ownership rule.

Split status composition into pure helpers:

```rust
fn expand_status_template(template: &str, context: &StatusContext<'_>) -> String;
fn build_status_line(..., theme: &StatusTheme) -> Option<Line>;
fn draw_prompt(..., style: Style);
```

Keep left and right sections bounded to one third, keep the center anchored to
terminal width, and treat `{windows}` as the centered window-list placeholder.
Sanitize all metadata before expansion.

- [ ] **Step 4: Preserve default wrappers and pass all core tests**

Existing public builders construct one default theme and delegate to the new
functions. Export the two explicit builders from `lib.rs`. Run:

```powershell
cargo fmt --all -- --check
cargo test -p wmux-core --lib --locked
```

Expected: all existing and new tests pass with no default-scene drift.

- [ ] **Step 5: Commit Task 2**

```powershell
git add crates/wmux-core/src/render.rs crates/wmux-core/src/lib.rs
git commit -m "feat(core): compose themed borders and status"
```

### Task 3: Presets, JSON Schema, And Config Layering

**Files:**

- Modify: `Cargo.toml`
- Modify: `Cargo.lock`
- Modify: `crates/wmux-config/Cargo.toml`
- Create: `crates/wmux-config/src/theme.rs`
- Modify: `crates/wmux-config/src/lib.rs`
- Modify: `crates/wmux-client/src/lib.rs`

**Interfaces:**

- Consumes: Task 1 theme values.
- Produces: `UiConfig`, `ThemeSources`, `ThemePatch`, `ThemeError`, `parse_theme_document(&[u8])`, `ThemeSources::resolve(Option<&[u8]>)`, and `WmuxConfig::ui()`.

- [ ] **Step 1: Add failing config and schema tests**

Cover unchanged defaults, preset selection, layering, strict schema, colours,
glyph width, and animation limits:

```rust
#[test]
fn missing_ui_keys_preserve_the_exact_default_theme() {
    let config = parse_config("agent_ui = plain\n").unwrap();
    assert_eq!(config.ui().resolve(None).unwrap(), UiTheme::default());
}

#[test]
fn provider_patch_precedes_explicit_overrides() {
    let config = parse_config(
        "ui.theme = neon\nui.active_border.foreground = \"#ffffff\"\n",
    )
    .unwrap();
    let provider = br#"{"schema":1,"active_border":{"style":{"fg":"#ff0000"}}}"#;
    let theme = config.ui().resolve(Some(provider)).unwrap();
    assert_eq!(theme.base.active_border.style.fg, Color::Rgb(255, 255, 255));
}

#[test]
fn schema_rejects_unknown_fields_and_wide_glyphs_with_paths() {
    let unknown = parse_theme_document(br#"{"schema":1,"bordr":{}}"#).unwrap_err();
    assert!(unknown.to_string().contains("bordr"));
    let wide = parse_theme_document(wide_glyph_document()).unwrap_err();
    assert!(wide.to_string().contains("border.glyphs.vertical"));
}
```

- [ ] **Step 2: Run config tests and confirm missing schema/config types**

```powershell
cargo test -p wmux-config --locked
```

Expected: compilation fails for the new UI API.

- [ ] **Step 3: Add serialization dependencies**

Add to workspace dependencies:

```toml
serde = { version = "1.0", features = ["derive"] }
serde_json = "1.0"
```

Add `serde.workspace = true`, `serde_json.workspace = true`, and the existing
path/version form of `wmux-core` to `wmux-config`. Let Cargo update both lock
files later when the fuzz crate adds its dependency.

- [ ] **Step 4: Implement strict document parsing and pure layering**

In `wmux-config/src/theme.rs`, define `#[serde(deny_unknown_fields)]` patch
types for the document, styles, glyph maps, status segments, and frames.
Validate before converting to core values. Use a field-path error:

```rust
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ThemeError {
    pub path: String,
    pub message: String,
}

pub fn parse_theme_document(bytes: &[u8]) -> Result<ThemePatch, ThemeError>;

impl ThemeSources {
    pub fn resolve(&self, provider: Option<&[u8]>) -> Result<UiTheme, ThemeError> {
        let mut candidate = preset(&self.preset)?;
        apply_optional_file(&mut candidate, self.theme_file.as_deref())?;
        apply_optional_provider(&mut candidate, provider)?;
        self.overrides.apply(&mut candidate)?;
        validate_resolved(&candidate)?;
        Ok(candidate)
    }
}
```

Implement `default`, `minimal`, `double`, `neon`, and `sakura` with indexed
colours. Keep optional animations disabled until `ui.animation` requests one.

- [ ] **Step 5: Parse `ui.*` keys without leaking them into command source**

Extend `WmuxConfig` with `ui: UiConfig`. Recognize all common keys from the
spec, including file/provider strings, border/status styles, animation target,
FPS, and playback. Resolve a relative `ui.theme_file` against the main config
directory in `load_or_create`; keep `parse_config` deterministic for unit tests.
Unknown `ui.*` keys return a line-numbered error.

Add the effective values to `wmux config effective` without printing provider
output or executing a provider.

- [ ] **Step 6: Pass config, client, and workspace unit tests**

```powershell
cargo fmt --all -- --check
cargo test -p wmux-config --locked
cargo test -p wmux-client --lib --locked
cargo test -p wmux-core --lib --locked
```

Expected: all tests pass; existing config fixtures remain accepted.

- [ ] **Step 7: Commit Task 3**

```powershell
git add Cargo.toml Cargo.lock crates/wmux-config crates/wmux-client/src/lib.rs
git commit -m "feat(config): add layered UI themes"
```

### Task 4: Atomic Static Theme Reload And Command Surface

**Files:**

- Modify: `crates/wmux-core/src/command/mod.rs`
- Modify: `crates/wmux-core/src/command/execute.rs`
- Modify: `crates/wmux-cli/src/lib.rs`
- Create: `crates/wmux-server/src/theme_runtime.rs`
- Modify: `crates/wmux-server/src/lib.rs`

**Interfaces:**

- Consumes: `ThemeSources::resolve(None)` and Task 2 explicit scene builders.
- Produces: `Command::ReloadTheme`, `CommandEffect::ReloadTheme`, user-facing `wmux theme reload`, `ThemeRuntime::active()`, and atomic generation swaps.

- [ ] **Step 1: Add failing command, CLI, and server reload tests**

Add parser/formatter and adapter expectations:

```rust
assert_eq!(parse_command_text("reload-theme").unwrap()[0], Command::ReloadTheme);
assert!(parse_command_text("reload-theme extra").is_err());
assert_eq!(server(&["theme", "reload"]).argv, args(&["reload-theme"]));
```

Add a server test that starts with the default, writes a valid test config
through an injected `ThemeLoader`, executes `reload-theme`, and asserts:

```rust
assert_eq!(owner.theme.generation(), 2);
assert_eq!(owner.theme.active().name, "double");
assert!(owner.clients.values().all(|view| view.full_render));
assert!(owner.runtime.state.panes.contains_key(&pane));
```

Add a failure case proving generation, theme, panes, and clients are unchanged.

- [ ] **Step 2: Run focused tests and confirm reload support is absent**

```powershell
cargo test -p wmux-cli theme_reload --locked
cargo test -p wmux-core reload_theme --locked
cargo test -p wmux-server static_theme_reload --locked
```

Expected: the new command and runtime types do not compile.

- [ ] **Step 3: Add serialized command/effect support**

Add `ReloadTheme` to `Command`, the command table, parser, formatter, pure
execution result, and `CommandEffect`:

```rust
ReloadTheme,

CommandEffect::ReloadTheme { requester: queued.client }
```

Map `wmux theme reload` to `reload-theme` in the CLI before ordinary command
resolution. Add it to help while retaining `reload-theme` as the direct server
command.

- [ ] **Step 4: Add an atomic static theme runtime**

Create focused server-owned values:

```rust
pub(crate) struct ThemeRuntime {
    generation: u64,
    active: Arc<UiTheme>,
    sources: ThemeSources,
}

impl ThemeRuntime {
    pub fn active(&self) -> &UiTheme;
    pub fn generation(&self) -> u64;
    pub fn replace(&mut self, sources: ThemeSources, theme: UiTheme) -> u64;
}
```

Inject a small loader seam in server tests so reload tests do not touch the
user's config file:

```rust
pub(crate) trait ThemeLoader: Send + Sync {
    fn load_sources(&self) -> Result<ThemeSources, String>;
}
```

The production implementation loads `WmuxConfig` and clones only its UI theme
sources. It resolves a candidate and swaps only after success; theme reload
does not unexpectedly reapply pane environment or command-source settings.

- [ ] **Step 5: Render every client from the active static frame**

Replace server calls to default-only builders with
`build_window_structure_with_theme` and `build_window_scene_with_theme`, using
`theme.active().base`. A successful swap invalidates structural caches and
requests one structural render for every attached client. A failed reload
returns `CommandErr` and retains the active generation.

- [ ] **Step 6: Pass focused and complete workspace unit tests**

```powershell
cargo fmt --all -- --check
cargo test -p wmux-cli --locked
cargo test -p wmux-core --lib --locked
cargo test -p wmux-server --lib --locked
```

Expected: static themes and reload work without changing pane/session state.

- [ ] **Step 7: Commit Task 4**

```powershell
git add crates/wmux-core/src/command crates/wmux-cli/src/lib.rs crates/wmux-server/src
git commit -m "feat(server): reload static UI themes atomically"
```

### Task 5: Client-scoped Animation Scheduling

**Files:**

- Modify: `crates/wmux-server/src/theme_runtime.rs`
- Modify: `crates/wmux-server/src/lib.rs`

**Interfaces:**

- Consumes: `AnimationSpec::select(elapsed_ms)` and active theme generations.
- Produces: `ClientThemeState::frame(...)`, `ClientThemeState::next_deadline(...)`, reset-on-generation behavior, and scheduler integration with no global tick.

- [ ] **Step 1: Add failing deterministic scheduler tests**

Cover static, once, looping, blocked, detached, and slow-client behavior:

```rust
#[test]
fn static_theme_never_schedules_an_animation_deadline() {
    let state = ClientThemeState::new(1, Instant::now());
    assert_eq!(state.next_deadline(&UiTheme::default()), None);
}

#[test]
fn looping_theme_schedules_only_the_next_frame_and_skips_backlog() {
    let start = Instant::now();
    let mut state = ClientThemeState::new(7, start);
    assert_eq!(state.frame(&looping_theme(), start).index, 0);
    assert_eq!(state.frame(&looping_theme(), start + Duration::from_millis(350)).index, 1);
    assert_eq!(state.next_deadline(&looping_theme()).unwrap(), start + Duration::from_millis(400));
}
```

In server tests, block a client, advance beyond several frames, drain it, and
assert one current-frame output rather than several queued outputs.

- [ ] **Step 2: Run focused tests and observe missing client animation state**

```powershell
cargo test -p wmux-server theme_animation --locked
```

Expected: compilation fails for `ClientThemeState`.

- [ ] **Step 3: Implement per-client clocks and generation reset**

Add:

```rust
pub(crate) struct ClientThemeState {
    generation: u64,
    started_at: Instant,
    rendered_frame: usize,
}

impl ClientThemeState {
    pub fn select<'a>(&mut self, theme: &'a UiTheme, generation: u64, now: Instant)
        -> (&'a UiFrame, Option<Instant>);
}
```

Reset `started_at` when theme generation changes. `once` returns no deadline
after its final frame; `loop` returns exactly one future deadline.

- [ ] **Step 4: Integrate animation into existing render deadlines**

Add `theme_state` to `ClientView`. Before sleeping, include the next animation
deadline only for attached, unblocked clients. When due, request a structural
render through the existing scheduler. After delivery, schedule the next
deadline; after backpressure, schedule none until `ClientWritable` requests the
current frame.

Pass the selected frame into both structural-cache comparison and final scene
composition. Do not mark pane generations consumed until the combined frame is
accepted, preserving the existing transactional baseline rule.

- [ ] **Step 5: Pass animation, server, stress-smoke, and static-idle tests**

```powershell
cargo fmt --all -- --check
cargo test -p wmux-server theme_animation --locked
cargo test -p wmux-server --lib --locked
cargo run --locked -p wmux-stress --release -- --profile ci
```

Expected: deterministic frame behavior, no static deadline, and no queued
animation backlog.

- [ ] **Step 6: Commit Task 5**

```powershell
git add crates/wmux-server/src/theme_runtime.rs crates/wmux-server/src/lib.rs
git commit -m "feat(server): schedule client-scoped UI animations"
```

### Task 6: One-shot Provider Jobs And Generation-safe Reload

**Files:**

- Modify: `crates/wmux-core/src/jobs.rs`
- Modify: `crates/wmux-server/src/theme_runtime.rs`
- Modify: `crates/wmux-server/src/lib.rs`
- Modify: `crates/wmux-config/src/theme.rs`

**Interfaces:**

- Consumes: the semantic `JobBackend`, provider command from `ThemeSources`, and `ThemeSources::resolve(Some(output))`.
- Produces: per-job output limits, `JobContinuation::ThemeProvider { generation }`, 2-second deadlines, stale-generation rejection, and foreground reload completion.

- [ ] **Step 1: Add failing bounded-job and provider lifecycle tests**

Extend job tests:

```rust
let job = jobs.start_with_output_limit(
    "provider".into(),
    false,
    JobContinuation::ThemeProvider { generation: 3 },
    queued(1),
    64 * 1024,
).unwrap();
assert!(jobs.append_output(job, &vec![b'x'; 64 * 1024 + 1]));
assert!(jobs.finish(job).unwrap().output_truncated());
```

Add server tests for valid output, nonzero exit, malformed JSON, timeout,
oversize output, backend error, descendant termination request, and stale
generation. The success case must wait for `Closed`, apply exactly once, reset
client animation state, redraw all attached clients, and complete the original
foreground invocation.

- [ ] **Step 2: Run focused tests and confirm provider continuation is absent**

```powershell
cargo test -p wmux-core jobs::tests --locked
cargo test -p wmux-server theme_provider --locked
```

Expected: compilation fails for provider jobs and runtime state.

- [ ] **Step 3: Generalize job output limits without changing shell jobs**

Store `output_limit` in `Job`. Keep `JobStore::start(...)` as a wrapper using
`MAX_JOB_OUTPUT_BYTES`, and add:

```rust
pub fn start_with_output_limit(
    &mut self,
    command: String,
    background: bool,
    continuation: JobContinuation,
    queued: QueuedCommand,
    output_limit: usize,
) -> Result<JobId, String>;
```

Add `ThemeProvider { generation: u64 }` to `JobContinuation`. Clamp the supplied
limit to `1..=MAX_JOB_OUTPUT_BYTES` and preserve existing shell-job behavior.
Add an owner-free constructor for the startup provider without manufacturing a
fake queued command:

```rust
pub fn start_internal(
    &mut self,
    command: String,
    continuation: JobContinuation,
    client: ClientId,
    source: CommandSource,
    output_limit: usize,
) -> Result<JobId, String>;
```

- [ ] **Step 4: Start providers outside the render path**

On startup, resolve and apply the static candidate, then start the configured
provider as an internal background job. On `reload-theme`, hold the foreground
command owner while its provider runs. Store:

```rust
pub(crate) struct PendingProvider {
    pub generation: u64,
    pub job: JobId,
    pub deadline: Instant,
    pub sources: ThemeSources,
    pub static_candidate: Arc<UiTheme>,
}
```

Set only documented provider environment values, including
`WMUX_THEME_SCHEMA=1` and `WMUX_VERSION`. Do not send pane/session metadata.

- [ ] **Step 5: Validate output and atomically finish reloads**

On `Closed`, require exit code zero, non-truncated UTF-8 output, strict JSON,
and a fully resolved valid theme. Apply only if its generation is still the
latest requested generation. Complete foreground reload with
`theme reloaded: <name>` or the precise error. Startup failure logs once and
keeps the static candidate.

- [ ] **Step 6: Enforce timeout and stale-result cleanup**

Include provider deadlines in `ServerOwner::next_deadline`. On expiry, submit
`JobRequest::Terminate`, remove the pending candidate, finish any foreground
owner with an error, and ignore later events for that stale job. A newer reload
terminates or supersedes the older pending provider before incrementing the
generation.

- [ ] **Step 7: Pass provider, job, server, and native lifecycle tests**

```powershell
cargo fmt --all -- --check
cargo test -p wmux-core jobs::tests --locked
cargo test -p wmux-server theme_provider --locked
cargo test -p wmux-server --lib --locked
cargo test -p wmux-windows --locked
cargo test -p wmux-unix --locked
```

Expected: all lifecycle tests pass and no provider survives timeout/shutdown.

- [ ] **Step 8: Commit Task 6**

```powershell
git add crates/wmux-core/src/jobs.rs crates/wmux-config/src/theme.rs crates/wmux-server/src
git commit -m "feat(server): load bounded one-shot theme providers"
```

### Task 7: Fuzzing, Benchmarks, Documentation, And Release Gate

**Files:**

- Modify: `crates/wmux-bench/src/main.rs`
- Modify: `fuzz/Cargo.toml`
- Modify: `fuzz/Cargo.lock`
- Create: `fuzz/fuzz_targets/theme_json.rs`
- Create: `fuzz/corpus/theme_json/minimal-theme`
- Create: `fuzz/corpus/theme_json/malformed-theme`
- Modify: `fuzz/README.md`
- Modify: `.github/workflows/beta-core.yml`
- Create: `docs/ui-themes.md`
- Modify: `README.md`
- Modify: `docs/hybrid-rendering.md`
- Modify: `docs/performance.md`
- Modify: `CHANGELOG.md`

**Interfaces:**

- Consumes: complete public theme/config/runtime/provider behavior from Tasks 1-6.
- Produces: stable user documentation, malformed-input coverage, an animated UI benchmark, and release-grade verification evidence.

- [ ] **Step 1: Add a failing benchmark registration test/check**

Register `animated-ui` in the benchmark scenario table and full suite, then add
gate assertions that require:

```rust
let animated = find("animated-ui").expect("required animated UI report");
require(animated.samples > 0, "animated-ui produced no samples");
require(animated.p95_us <= 5_000.0, "animated-ui p95 exceeded 5 ms");
require(animated.queue_peak <= 1, "animated-ui queued obsolete frames");
```

Run before implementing the scenario:

```powershell
cargo test -p wmux-bench --locked
cargo run --locked -p wmux-bench --release -- --suite smoke --scenario animated-ui
```

Expected: failure because the scenario is not implemented.

- [ ] **Step 2: Implement the animated diff workload**

Build one split structural scene, alternate two validated UI frames for the
smoke/full iteration count, render through a retained `RenderState`, and report
generated bytes, latency, allocation, and queue peak separately from static
scenarios. Do not change an existing threshold.

- [ ] **Step 3: Add the bounded theme JSON fuzz target**

Add `wmux-config` to fuzz dependencies and create:

```rust
#![no_main]

use libfuzzer_sys::fuzz_target;
use std::hint::black_box;

const MAX_INPUT_BYTES: usize = 64 * 1024;

fuzz_target!(|data: &[u8]| {
    if data.len() <= MAX_INPUT_BYTES {
        black_box(wmux_config::parse_theme_document(data));
    }
});
```

Register the binary, add valid/malformed corpus seeds, add compile/lint/smoke
commands to the fuzz README and CI workflow, and include its artifact directory
in upload preparation.

- [ ] **Step 4: Write public documentation and examples**

Document the exact main-config keys, five presets, complete schema fields,
colour syntax, one-cell glyph rule, stable-width status animation rule,
`once`/`loop`, 30-FPS and 64-frame limits, trusted one-shot provider model,
2-second/64-KiB limits, and `wmux theme reload`.

Include copyable examples for:

```text
ui.theme = sakura
ui.animation = sweep
ui.animation_target = both
ui.animation_fps = 12
ui.animation_playback = loop
```

and a provider that prints one schema-1 JSON document and exits. Link
`docs/ui-themes.md` from README. Update rendering/performance docs and the
Unreleased changelog without naming competitor products.

- [ ] **Step 5: Run focused formatting, lint, tests, and fuzz checks**

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --locked
cargo check --manifest-path fuzz/Cargo.toml --bins --locked
cargo clippy --manifest-path fuzz/Cargo.toml --bins --locked -- -D warnings
```

Expected: all commands pass.

- [ ] **Step 6: Run deterministic behavior, stress, and performance gates**

Use the repository's current authoritative command flags from each binary's
help, then run the same release profiles enforced by CI:

```powershell
cargo run --locked -p wmux-conformance --release
cargo run --locked -p wmux-stress --release -- --profile ci
cargo run --locked -p wmux-stress --release -- --profile full
cargo run --locked -p wmux-bench --release -- --suite full --gate
```

Expected: conformance is deterministic, stress passes, every existing static
gate remains unchanged, and the new animated gate passes independently.

- [ ] **Step 7: Run supply-chain and release-shape checks**

```powershell
cargo deny check
cargo dist generate --check
cargo dist plan --tag=v1.0.1
```

Expected: dependency/license checks pass with only previously documented
duplicate-version warnings; distribution generation and the five-target plan
remain valid.

- [ ] **Step 8: Scan tracked content and inspect the complete diff**

```powershell
$blockedNames = @(
    -join (116,109,117,120 | ForEach-Object { [char]$_ }),
    -join (122,101,108,108,105,106 | ForEach-Object { [char]$_ }),
    -join (119,109,117,120,45,99,108,101,97,110 | ForEach-Object { [char]$_ })
)
foreach ($blockedName in $blockedNames) {
    git grep -n -i -- $blockedName -- . ":(exclude).agents/**"
}
git diff --check
git status --short
git diff --stat 46ffa0e..HEAD
```

Expected: the naming scan returns no matches; `git diff --check` is clean; only
`.agents/` remains untracked outside the intended feature changes.

- [ ] **Step 9: Commit final evidence and documentation**

```powershell
git add Cargo.lock CHANGELOG.md README.md docs/ui-themes.md docs/hybrid-rendering.md docs/performance.md crates/wmux-bench fuzz .github/workflows/beta-core.yml
git commit -m "docs: publish UI theme customization guide"
```

- [ ] **Step 10: Verify and push `main`**

```powershell
git status --short --branch
git log -8 --oneline --decorate
git push origin main
```

Expected: only `?? .agents/` remains locally; all feature commits are directly
on `main`; the push advances `origin/main` without moving release tags.
