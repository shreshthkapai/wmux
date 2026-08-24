use std::{
    fmt, fs,
    path::{Path, PathBuf},
};

use serde::Deserialize;
use wmux_core::{
    scalar_width, AnimationSpec, AnimationTarget, BorderGlyphSet, BorderTheme, Color, Playback,
    StatusTheme, Style, UiFrame, UiTheme, MAX_THEME_FPS, MAX_THEME_FRAMES,
};

pub const THEME_SCHEMA_VERSION: u16 = 1;
pub const MAX_THEME_DOCUMENT_BYTES: usize = 64 * 1024;
const MAX_THEME_NAME_BYTES: usize = 128;
const MAX_STATUS_TEMPLATE_BYTES: usize = 1_024;
const DEFAULT_ANIMATION_FPS: u8 = 12;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ThemeError {
    pub path: String,
    pub message: String,
}

impl ThemeError {
    fn new(path: impl Into<String>, message: impl Into<String>) -> Self {
        Self {
            path: path.into(),
            message: message.into(),
        }
    }
}

impl fmt::Display for ThemeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}: {}", self.path, self.message)
    }
}

impl std::error::Error for ThemeError {}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ThemeSources {
    pub(crate) preset: String,
    pub(crate) theme_file: Option<PathBuf>,
    pub(crate) theme_provider: Option<String>,
    pub(crate) overrides: ThemePatch,
    pub(crate) animation: AnimationOptions,
}

pub type UiConfig = ThemeSources;

impl Default for ThemeSources {
    fn default() -> Self {
        Self {
            preset: "default".to_owned(),
            theme_file: None,
            theme_provider: None,
            overrides: ThemePatch::default(),
            animation: AnimationOptions::default(),
        }
    }
}

impl ThemeSources {
    pub fn preset_name(&self) -> &str {
        &self.preset
    }

    pub fn theme_file(&self) -> Option<&Path> {
        self.theme_file.as_deref()
    }

    pub fn theme_provider(&self) -> Option<&str> {
        self.theme_provider.as_deref()
    }

    pub fn animation_name(&self) -> Option<&str> {
        self.animation.choice.map(AnimationChoice::name)
    }

    pub fn animation_target(&self) -> Option<AnimationTarget> {
        self.animation.target
    }

    pub fn animation_fps(&self) -> Option<u8> {
        self.animation.fps
    }

    pub fn animation_playback(&self) -> Option<Playback> {
        self.animation.playback
    }

    pub fn resolve(&self, provider: Option<&[u8]>) -> Result<UiTheme, ThemeError> {
        let mut candidate = preset(&self.preset)?;
        if let Some(path) = &self.theme_file {
            let bytes = fs::read(path).map_err(|error| {
                ThemeError::new("ui.theme_file", format!("{}: {error}", path.display()))
            })?;
            parse_theme_document(&bytes)?.apply(&mut candidate)?;
        }
        if let Some(bytes) = provider {
            parse_theme_document(bytes)?.apply(&mut candidate)?;
        }
        self.overrides.apply(&mut candidate)?;
        self.animation.apply(&mut candidate)?;
        validate_resolved(&candidate)?;
        Ok(candidate)
    }

    pub(crate) fn resolve_relative_theme_file(&mut self, directory: &Path) {
        if let Some(path) = self.theme_file.as_mut() {
            if path.is_relative() {
                *path = directory.join(&*path);
            }
        }
    }

    pub(crate) fn apply_config_option(
        &mut self,
        key: &str,
        value: &str,
    ) -> Result<bool, ThemeError> {
        if !key.starts_with("ui.") {
            return Ok(false);
        }
        match key {
            "ui.theme" => {
                if value.is_empty() {
                    return Err(ThemeError::new(key, "must not be empty"));
                }
                self.preset = value.to_owned();
            }
            "ui.theme_file" => {
                self.theme_file = (!value.is_empty()).then(|| PathBuf::from(value));
            }
            "ui.theme_provider" => {
                self.theme_provider = (!value.is_empty()).then(|| value.to_owned());
            }
            "ui.border.style" => {
                apply_flat_border_glyphs(
                    key,
                    value,
                    self.overrides.border.get_or_insert_default(),
                )?;
            }
            "ui.border.foreground" => {
                self.overrides.border.get_or_insert_default().style.fg =
                    Some(parse_color(key, value)?);
            }
            "ui.border.background" => {
                self.overrides.border.get_or_insert_default().style.bg =
                    Some(parse_color(key, value)?);
            }
            "ui.border.attributes" => {
                self.overrides
                    .border
                    .get_or_insert_default()
                    .style
                    .merge(parse_style_attributes(key, value)?);
            }
            "ui.active_border.style" => {
                apply_flat_border_glyphs(
                    key,
                    value,
                    self.overrides.active_border.get_or_insert_default(),
                )?;
            }
            "ui.active_border.foreground" => {
                self.overrides
                    .active_border
                    .get_or_insert_default()
                    .style
                    .fg = Some(parse_color(key, value)?);
            }
            "ui.active_border.background" => {
                self.overrides
                    .active_border
                    .get_or_insert_default()
                    .style
                    .bg = Some(parse_color(key, value)?);
            }
            "ui.active_border.attributes" => {
                self.overrides
                    .active_border
                    .get_or_insert_default()
                    .style
                    .merge(parse_style_attributes(key, value)?);
            }
            "ui.status.style" => {
                let style = parse_style_attributes(key, value)?;
                apply_all_status_styles(self.overrides.status.get_or_insert_default(), &style);
            }
            "ui.status.foreground" => {
                let style = StylePatch {
                    fg: Some(parse_color(key, value)?),
                    ..StylePatch::default()
                };
                apply_all_status_styles(self.overrides.status.get_or_insert_default(), &style);
            }
            "ui.status.background" => {
                let style = StylePatch {
                    bg: Some(parse_color(key, value)?),
                    ..StylePatch::default()
                };
                apply_all_status_styles(self.overrides.status.get_or_insert_default(), &style);
            }
            "ui.status.left"
            | "ui.status.center"
            | "ui.status.window"
            | "ui.status.active_window"
            | "ui.status.right" => {
                validate_template(key, value)?;
                let status = self.overrides.status.get_or_insert_default();
                match key {
                    "ui.status.left" => status.left = Some(value.to_owned()),
                    "ui.status.center" => status.center = Some(value.to_owned()),
                    "ui.status.window" => status.window = Some(value.to_owned()),
                    "ui.status.active_window" => status.active_window = Some(value.to_owned()),
                    "ui.status.right" => status.right = Some(value.to_owned()),
                    _ => unreachable!("matched status template key"),
                }
            }
            "ui.animation" => {
                self.animation.choice = Some(parse_animation_choice(key, value)?);
            }
            "ui.animation_target" => {
                self.animation.target = Some(parse_animation_target(key, value)?);
            }
            "ui.animation_fps" => {
                let fps = value.parse::<u8>().map_err(|_| {
                    ThemeError::new(key, format!("must be between 1 and {MAX_THEME_FPS}"))
                })?;
                validate_fps(Some(fps), key)?;
                self.animation.fps = Some(fps);
            }
            "ui.animation_playback" => {
                self.animation.playback = Some(parse_playback(key, value)?);
            }
            _ => return Err(ThemeError::new(key, "unknown UI option")),
        }
        Ok(true)
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct ThemePatch {
    name: Option<String>,
    border: Option<BorderPatch>,
    active_border: Option<BorderPatch>,
    status: Option<StatusPatch>,
    animation: Option<AnimationPatch>,
}

impl ThemePatch {
    fn apply(&self, theme: &mut UiTheme) -> Result<(), ThemeError> {
        if let Some(name) = &self.name {
            theme.name.clone_from(name);
        }
        if let Some(patch) = &self.border {
            patch.apply("border", &mut theme.base.border)?;
            if let Some(animation) = theme.animation.as_mut() {
                for frame in &mut animation.frames {
                    patch.apply("border", &mut frame.border)?;
                }
            }
        }
        if let Some(patch) = &self.active_border {
            patch.apply("active_border", &mut theme.base.active_border)?;
            if let Some(animation) = theme.animation.as_mut() {
                for frame in &mut animation.frames {
                    patch.apply("active_border", &mut frame.active_border)?;
                }
            }
        }
        if let Some(patch) = &self.status {
            patch.apply("status", &mut theme.base.status)?;
            if let Some(animation) = theme.animation.as_mut() {
                for frame in &mut animation.frames {
                    patch.apply("status", &mut frame.status)?;
                }
            }
        }
        if let Some(animation) = &self.animation {
            theme.animation = Some(animation.resolve(&theme.base)?);
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
struct StylePatch {
    fg: Option<Color>,
    bg: Option<Color>,
    bold: Option<bool>,
    dim: Option<bool>,
    italic: Option<bool>,
    underline: Option<bool>,
    reverse: Option<bool>,
    hidden: Option<bool>,
    strikethrough: Option<bool>,
}

impl StylePatch {
    fn apply(&self, style: &mut Style) {
        if let Some(value) = self.fg {
            style.fg = value;
        }
        if let Some(value) = self.bg {
            style.bg = value;
        }
        if let Some(value) = self.bold {
            style.bold = value;
        }
        if let Some(value) = self.dim {
            style.dim = value;
        }
        if let Some(value) = self.italic {
            style.italic = value;
        }
        if let Some(value) = self.underline {
            style.underline = value;
        }
        if let Some(value) = self.reverse {
            style.reverse = value;
        }
        if let Some(value) = self.hidden {
            style.hidden = value;
        }
        if let Some(value) = self.strikethrough {
            style.strikethrough = value;
        }
    }

    fn merge(&mut self, later: Self) {
        macro_rules! replace_some {
            ($($field:ident),+ $(,)?) => {
                $(if later.$field.is_some() { self.$field = later.$field; })+
            };
        }
        replace_some!(
            fg,
            bg,
            bold,
            dim,
            italic,
            underline,
            reverse,
            hidden,
            strikethrough,
        );
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
struct BorderPatch {
    style: StylePatch,
    glyphs: Option<BorderGlyphSet>,
    visible: Option<bool>,
}

impl BorderPatch {
    fn apply(&self, path: &str, border: &mut BorderTheme) -> Result<(), ThemeError> {
        self.style.apply(&mut border.style);
        if let Some(glyphs) = self.glyphs {
            validate_glyph_set(path, glyphs)?;
            border.glyphs = glyphs;
        }
        if let Some(visible) = self.visible {
            border.visible = visible;
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
struct StatusPatch {
    base: Option<StylePatch>,
    left_style: Option<StylePatch>,
    center_style: Option<StylePatch>,
    window_style: Option<StylePatch>,
    active_window_style: Option<StylePatch>,
    right_style: Option<StylePatch>,
    prompt_style: Option<StylePatch>,
    left: Option<String>,
    center: Option<String>,
    window: Option<String>,
    active_window: Option<String>,
    right: Option<String>,
}

impl StatusPatch {
    fn apply(&self, path: &str, status: &mut StatusTheme) -> Result<(), ThemeError> {
        macro_rules! apply_style {
            ($field:ident) => {
                if let Some(patch) = &self.$field {
                    patch.apply(&mut status.$field);
                }
            };
        }
        apply_style!(base);
        apply_style!(left_style);
        apply_style!(center_style);
        apply_style!(window_style);
        apply_style!(active_window_style);
        apply_style!(right_style);
        apply_style!(prompt_style);

        macro_rules! apply_template {
            ($field:ident) => {
                if let Some(value) = &self.$field {
                    validate_template(&format!("{path}.{}", stringify!($field)), value)?;
                    status.$field.clone_from(value);
                }
            };
        }
        apply_template!(left);
        apply_template!(center);
        apply_template!(window);
        apply_template!(active_window);
        apply_template!(right);
        Ok(())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct FramePatch {
    duration_ms: Option<u32>,
    border: Option<BorderPatch>,
    active_border: Option<BorderPatch>,
    status: Option<StatusPatch>,
}

impl FramePatch {
    fn apply(&self, index: usize, frame: &mut UiFrame) -> Result<(), ThemeError> {
        if let Some(duration_ms) = self.duration_ms {
            frame.duration_ms = duration_ms;
        }
        if let Some(patch) = &self.border {
            patch.apply(
                &format!("animation.frames[{index}].border"),
                &mut frame.border,
            )?;
        }
        if let Some(patch) = &self.active_border {
            patch.apply(
                &format!("animation.frames[{index}].active_border"),
                &mut frame.active_border,
            )?;
        }
        if let Some(patch) = &self.status {
            patch.apply(
                &format!("animation.frames[{index}].status"),
                &mut frame.status,
            )?;
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct AnimationPatch {
    target: AnimationTarget,
    playback: Playback,
    fps: Option<u8>,
    frames: Vec<FramePatch>,
}

impl AnimationPatch {
    fn resolve(&self, base: &UiFrame) -> Result<AnimationSpec, ThemeError> {
        validate_fps(self.fps, "animation.fps")?;
        if self.frames.is_empty() {
            return Err(ThemeError::new(
                "animation.frames",
                "must contain at least one frame",
            ));
        }
        if self.frames.len() > MAX_THEME_FRAMES {
            return Err(ThemeError::new(
                "animation.frames",
                format!("has more than {MAX_THEME_FRAMES} frames"),
            ));
        }
        let fallback_duration = frame_duration(self.fps.unwrap_or(DEFAULT_ANIMATION_FPS));
        let mut frames = Vec::with_capacity(self.frames.len());
        for (index, patch) in self.frames.iter().enumerate() {
            let mut frame = base.clone();
            frame.duration_ms = fallback_duration;
            patch.apply(index, &mut frame)?;
            validate_frame_duration(index, frame.duration_ms)?;
            mask_animation_target(base, self.target, &mut frame);
            frames.push(frame);
        }
        validate_animation_status_widths(&frames)?;
        AnimationSpec::new(self.target, self.playback, frames)
            .map_err(|message| ThemeError::new("animation", message))
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum AnimationChoice {
    Off,
    Pulse,
    Sweep,
    Shimmer,
    ColorCycle,
    Custom,
}

impl AnimationChoice {
    pub(crate) const fn name(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Pulse => "pulse",
            Self::Sweep => "sweep",
            Self::Shimmer => "shimmer",
            Self::ColorCycle => "colour-cycle",
            Self::Custom => "custom",
        }
    }
}

#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub(crate) struct AnimationOptions {
    pub(crate) choice: Option<AnimationChoice>,
    pub(crate) target: Option<AnimationTarget>,
    pub(crate) fps: Option<u8>,
    pub(crate) playback: Option<Playback>,
}

impl AnimationOptions {
    fn apply(&self, theme: &mut UiTheme) -> Result<(), ThemeError> {
        validate_fps(self.fps, "ui.animation_fps")?;
        match self.choice {
            Some(AnimationChoice::Off) => theme.animation = None,
            Some(AnimationChoice::Custom) => {
                if theme.animation.is_none() {
                    return Err(ThemeError::new(
                        "ui.animation",
                        "custom requires animation frames from a theme file or provider",
                    ));
                }
            }
            Some(choice) => {
                theme.animation = Some(built_in_animation(
                    &theme.base,
                    choice,
                    self.target.unwrap_or(AnimationTarget::Both),
                    self.fps.unwrap_or(DEFAULT_ANIMATION_FPS),
                    self.playback.unwrap_or(Playback::Loop),
                )?);
            }
            None => {}
        }

        if let Some(animation) = theme.animation.take() {
            let target = self.target.unwrap_or(animation.target);
            let playback = self.playback.unwrap_or(animation.playback);
            let mut frames = animation.frames;
            if let Some(fps) = self.fps {
                let duration = frame_duration(fps);
                for frame in &mut frames {
                    frame.duration_ms = duration;
                }
            }
            for frame in &mut frames {
                mask_animation_target(&theme.base, target, frame);
            }
            validate_animation_status_widths(&frames)?;
            theme.animation = Some(
                AnimationSpec::new(target, playback, frames)
                    .map_err(|message| ThemeError::new("ui.animation", message))?,
            );
        }
        Ok(())
    }
}

pub fn parse_theme_document(bytes: &[u8]) -> Result<ThemePatch, ThemeError> {
    if bytes.len() > MAX_THEME_DOCUMENT_BYTES {
        return Err(ThemeError::new(
            "document",
            format!("exceeds {MAX_THEME_DOCUMENT_BYTES} bytes"),
        ));
    }
    let document: ThemeDocument = serde_json::from_slice(bytes)
        .map_err(|error| ThemeError::new("document", error.to_string()))?;
    if document.schema != THEME_SCHEMA_VERSION {
        return Err(ThemeError::new(
            "schema",
            format!(
                "unsupported version {}; expected {THEME_SCHEMA_VERSION}",
                document.schema
            ),
        ));
    }
    document.into_patch()
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ThemeDocument {
    schema: u16,
    name: Option<String>,
    border: Option<BorderDocument>,
    active_border: Option<BorderDocument>,
    status: Option<StatusDocument>,
    animation: Option<AnimationDocument>,
}

impl ThemeDocument {
    fn into_patch(self) -> Result<ThemePatch, ThemeError> {
        Ok(ThemePatch {
            name: self.name,
            border: self
                .border
                .map(|document| document.into_patch("border"))
                .transpose()?,
            active_border: self
                .active_border
                .map(|document| document.into_patch("active_border"))
                .transpose()?,
            status: self
                .status
                .map(|document| document.into_patch("status"))
                .transpose()?,
            animation: self
                .animation
                .map(AnimationDocument::into_patch)
                .transpose()?,
        })
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct StyleDocument {
    fg: Option<String>,
    bg: Option<String>,
    bold: Option<bool>,
    dim: Option<bool>,
    italic: Option<bool>,
    underline: Option<bool>,
    reverse: Option<bool>,
    hidden: Option<bool>,
    strikethrough: Option<bool>,
}

impl StyleDocument {
    fn into_patch(self, path: &str) -> Result<StylePatch, ThemeError> {
        Ok(StylePatch {
            fg: self
                .fg
                .map(|value| parse_color(&format!("{path}.fg"), &value))
                .transpose()?,
            bg: self
                .bg
                .map(|value| parse_color(&format!("{path}.bg"), &value))
                .transpose()?,
            bold: self.bold,
            dim: self.dim,
            italic: self.italic,
            underline: self.underline,
            reverse: self.reverse,
            hidden: self.hidden,
            strikethrough: self.strikethrough,
        })
    }
}

#[derive(Deserialize)]
#[serde(untagged)]
enum GlyphDocument {
    Named(String),
    Custom(Box<CustomGlyphDocument>),
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CustomGlyphDocument {
    vertical: Option<String>,
    horizontal: Option<String>,
    down_right: Option<String>,
    down_left: Option<String>,
    up_right: Option<String>,
    up_left: Option<String>,
    vertical_right: Option<String>,
    vertical_left: Option<String>,
    horizontal_down: Option<String>,
    horizontal_up: Option<String>,
    cross: Option<String>,
}

impl GlyphDocument {
    fn into_glyphs(self, path: &str) -> Result<BorderGlyphSet, ThemeError> {
        match self {
            Self::Named(name) => named_glyphs(path, &name),
            Self::Custom(document) => document.into_glyphs(path),
        }
    }
}

impl CustomGlyphDocument {
    fn into_glyphs(self, path: &str) -> Result<BorderGlyphSet, ThemeError> {
        macro_rules! glyph {
            ($field:ident) => {
                parse_glyph(
                    &format!("{path}.{}", stringify!($field)),
                    self.$field.as_deref().ok_or_else(|| {
                        ThemeError::new(
                            format!("{path}.{}", stringify!($field)),
                            "is required for a custom glyph set",
                        )
                    })?,
                )?
            };
        }
        Ok(BorderGlyphSet {
            vertical: glyph!(vertical),
            horizontal: glyph!(horizontal),
            down_right: glyph!(down_right),
            down_left: glyph!(down_left),
            up_right: glyph!(up_right),
            up_left: glyph!(up_left),
            vertical_right: glyph!(vertical_right),
            vertical_left: glyph!(vertical_left),
            horizontal_down: glyph!(horizontal_down),
            horizontal_up: glyph!(horizontal_up),
            cross: glyph!(cross),
        })
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct BorderDocument {
    style: Option<StyleDocument>,
    glyphs: Option<GlyphDocument>,
    visible: Option<bool>,
    fg: Option<String>,
    bg: Option<String>,
    bold: Option<bool>,
    dim: Option<bool>,
    italic: Option<bool>,
    underline: Option<bool>,
    reverse: Option<bool>,
    hidden: Option<bool>,
    strikethrough: Option<bool>,
}

impl BorderDocument {
    fn into_patch(self, path: &str) -> Result<BorderPatch, ThemeError> {
        let mut style = self
            .style
            .map(|document| document.into_patch(&format!("{path}.style")))
            .transpose()?
            .unwrap_or_default();
        style.merge(
            StyleDocument {
                fg: self.fg,
                bg: self.bg,
                bold: self.bold,
                dim: self.dim,
                italic: self.italic,
                underline: self.underline,
                reverse: self.reverse,
                hidden: self.hidden,
                strikethrough: self.strikethrough,
            }
            .into_patch(path)?,
        );
        Ok(BorderPatch {
            style,
            glyphs: self
                .glyphs
                .map(|glyphs| glyphs.into_glyphs(&format!("{path}.glyphs")))
                .transpose()?,
            visible: self.visible,
        })
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct StatusDocument {
    base: Option<StyleDocument>,
    left_style: Option<StyleDocument>,
    center_style: Option<StyleDocument>,
    window_style: Option<StyleDocument>,
    active_window_style: Option<StyleDocument>,
    right_style: Option<StyleDocument>,
    prompt: Option<StyleDocument>,
    left: Option<String>,
    center: Option<String>,
    window: Option<String>,
    active_window: Option<String>,
    right: Option<String>,
}

impl StatusDocument {
    fn into_patch(self, path: &str) -> Result<StatusPatch, ThemeError> {
        macro_rules! style {
            ($field:ident) => {
                self.$field
                    .map(|document| document.into_patch(&format!("{path}.{}", stringify!($field))))
                    .transpose()?
            };
        }
        let patch = StatusPatch {
            base: style!(base),
            left_style: style!(left_style),
            center_style: style!(center_style),
            window_style: style!(window_style),
            active_window_style: style!(active_window_style),
            right_style: style!(right_style),
            prompt_style: self
                .prompt
                .map(|document| document.into_patch(&format!("{path}.prompt")))
                .transpose()?,
            left: self.left,
            center: self.center,
            window: self.window,
            active_window: self.active_window,
            right: self.right,
        };
        for (field, value) in [
            ("left", patch.left.as_deref()),
            ("center", patch.center.as_deref()),
            ("window", patch.window.as_deref()),
            ("active_window", patch.active_window.as_deref()),
            ("right", patch.right.as_deref()),
        ] {
            if let Some(value) = value {
                validate_template(&format!("{path}.{field}"), value)?;
            }
        }
        Ok(patch)
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct FrameDocument {
    duration_ms: Option<u32>,
    border: Option<BorderDocument>,
    active_border: Option<BorderDocument>,
    status: Option<StatusDocument>,
}

impl FrameDocument {
    fn into_patch(self, index: usize) -> Result<FramePatch, ThemeError> {
        Ok(FramePatch {
            duration_ms: self.duration_ms,
            border: self
                .border
                .map(|document| document.into_patch(&format!("animation.frames[{index}].border")))
                .transpose()?,
            active_border: self
                .active_border
                .map(|document| {
                    document.into_patch(&format!("animation.frames[{index}].active_border"))
                })
                .transpose()?,
            status: self
                .status
                .map(|document| document.into_patch(&format!("animation.frames[{index}].status")))
                .transpose()?,
        })
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct AnimationDocument {
    target: Option<String>,
    playback: Option<String>,
    fps: Option<u8>,
    frames: Vec<FrameDocument>,
}

impl AnimationDocument {
    fn into_patch(self) -> Result<AnimationPatch, ThemeError> {
        Ok(AnimationPatch {
            target: self
                .target
                .as_deref()
                .map(|value| parse_animation_target("animation.target", value))
                .transpose()?
                .unwrap_or(AnimationTarget::Both),
            playback: self
                .playback
                .as_deref()
                .map(|value| parse_playback("animation.playback", value))
                .transpose()?
                .unwrap_or(Playback::Loop),
            fps: self.fps,
            frames: self
                .frames
                .into_iter()
                .enumerate()
                .map(|(index, frame)| frame.into_patch(index))
                .collect::<Result<_, _>>()?,
        })
    }
}

fn preset(name: &str) -> Result<UiTheme, ThemeError> {
    let mut theme = UiTheme {
        name: name.to_owned(),
        ..UiTheme::default()
    };
    match name {
        "default" => {}
        "minimal" => {
            theme.base.border.style.fg = Color::Indexed(8);
            theme.base.border.style.dim = true;
            theme.base.active_border.glyphs = BorderGlyphSet::SINGLE;
            theme.base.active_border.style.fg = Color::Indexed(7);
            paint_status(&mut theme.base.status, Color::Indexed(7), Color::Indexed(0));
            theme.base.status.base.dim = true;
            theme.base.status.left = " {session} ".to_owned();
            theme.base.status.right = " {pane_index}:{pane_title} ".to_owned();
        }
        "double" => {
            theme.base.border.glyphs = BorderGlyphSet::DOUBLE;
            theme.base.active_border.glyphs = BorderGlyphSet::DOUBLE;
            theme.base.border.style.fg = Color::Indexed(7);
            theme.base.active_border.style.fg = Color::Indexed(15);
            paint_status(&mut theme.base.status, Color::Indexed(7), Color::Indexed(0));
        }
        "neon" => {
            theme.base.border.style.fg = Color::Indexed(14);
            theme.base.active_border.style.fg = Color::Indexed(13);
            paint_status(
                &mut theme.base.status,
                Color::Indexed(15),
                Color::Indexed(4),
            );
        }
        "sakura" => {
            theme.base.border.style.fg = Color::Indexed(8);
            theme.base.active_border.style.fg = Color::Indexed(13);
            paint_status(
                &mut theme.base.status,
                Color::Indexed(15),
                Color::Indexed(5),
            );
            theme.base.status.left = " ✿ wmux · {session} ".to_owned();
            theme.base.status.right = " pane {pane_index} · {pane_title} ✿ ".to_owned();
        }
        _ => {
            return Err(ThemeError::new(
                "ui.theme",
                format!("unknown preset {name:?}"),
            ));
        }
    }
    Ok(theme)
}

fn paint_status(status: &mut StatusTheme, fg: Color, bg: Color) {
    let base = Style {
        fg,
        bg,
        ..Style::default()
    };
    status.base = base;
    status.left_style = base;
    status.center_style = base;
    status.window_style = base;
    status.active_window_style = Style { bold: true, ..base };
    status.right_style = base;
    status.prompt_style = base;
}

fn built_in_animation(
    base: &UiFrame,
    choice: AnimationChoice,
    target: AnimationTarget,
    fps: u8,
    playback: Playback,
) -> Result<AnimationSpec, ThemeError> {
    validate_fps(Some(fps), "ui.animation_fps")?;
    let duration_ms = frame_duration(fps);
    let colors: &[u8] = match choice {
        AnimationChoice::Pulse => &[13, 15, 13, 8],
        AnimationChoice::Sweep => &[14, 12, 13, 11],
        AnimationChoice::Shimmer => &[15, 7, 15, 8],
        AnimationChoice::ColorCycle => &[9, 11, 10, 14, 12, 13],
        AnimationChoice::Off | AnimationChoice::Custom => &[],
    };
    let mut frames = colors
        .iter()
        .map(|color| {
            let mut frame = base.clone();
            frame.duration_ms = duration_ms;
            match choice {
                AnimationChoice::Pulse | AnimationChoice::Sweep => {
                    frame.active_border.style.fg = Color::Indexed(*color);
                }
                AnimationChoice::Shimmer => {
                    frame.status.center_style.fg = Color::Indexed(*color);
                    frame.status.active_window_style.fg = Color::Indexed(*color);
                }
                AnimationChoice::ColorCycle => {
                    frame.border.style.fg = Color::Indexed(*color);
                    frame.active_border.style.fg = Color::Indexed(*color);
                    frame.status.left_style.fg = Color::Indexed(*color);
                    frame.status.center_style.fg = Color::Indexed(*color);
                    frame.status.right_style.fg = Color::Indexed(*color);
                }
                AnimationChoice::Off | AnimationChoice::Custom => {}
            }
            mask_animation_target(base, target, &mut frame);
            frame
        })
        .collect::<Vec<_>>();
    if frames.is_empty() {
        frames.push(base.clone());
        frames[0].duration_ms = duration_ms;
    }
    AnimationSpec::new(target, playback, frames)
        .map_err(|message| ThemeError::new("ui.animation", message))
}

fn mask_animation_target(base: &UiFrame, target: AnimationTarget, frame: &mut UiFrame) {
    match target {
        AnimationTarget::Borders => frame.status = base.status.clone(),
        AnimationTarget::Status => {
            frame.border = base.border.clone();
            frame.active_border = base.active_border.clone();
        }
        AnimationTarget::Both => {}
    }
}

fn apply_flat_border_glyphs(
    path: &str,
    value: &str,
    border: &mut BorderPatch,
) -> Result<(), ThemeError> {
    if value == "none" {
        border.visible = Some(false);
        return Ok(());
    }
    if value == "custom" {
        return Err(ThemeError::new(
            path,
            "custom glyph maps must be defined in a theme document",
        ));
    }
    border.glyphs = Some(named_glyphs(path, value)?);
    border.visible = Some(true);
    Ok(())
}

fn apply_all_status_styles(status: &mut StatusPatch, style: &StylePatch) {
    for target in [
        &mut status.base,
        &mut status.left_style,
        &mut status.center_style,
        &mut status.window_style,
        &mut status.active_window_style,
        &mut status.right_style,
        &mut status.prompt_style,
    ] {
        target.get_or_insert_default().merge(style.clone());
    }
}

fn parse_style_attributes(path: &str, value: &str) -> Result<StylePatch, ThemeError> {
    let mut style = StylePatch {
        bold: Some(false),
        dim: Some(false),
        italic: Some(false),
        underline: Some(false),
        reverse: Some(false),
        hidden: Some(false),
        strikethrough: Some(false),
        ..StylePatch::default()
    };
    for attribute in value
        .split(|character: char| character == ',' || character.is_whitespace())
        .filter(|attribute| !attribute.is_empty())
    {
        match attribute {
            "default" | "none" => {}
            "bold" => style.bold = Some(true),
            "dim" => style.dim = Some(true),
            "italic" => style.italic = Some(true),
            "underline" => style.underline = Some(true),
            "reverse" => style.reverse = Some(true),
            "hidden" => style.hidden = Some(true),
            "strikethrough" => style.strikethrough = Some(true),
            _ => {
                return Err(ThemeError::new(
                    path,
                    format!("unknown style attribute {attribute:?}"),
                ));
            }
        }
    }
    Ok(style)
}

fn parse_animation_choice(path: &str, value: &str) -> Result<AnimationChoice, ThemeError> {
    match value {
        "off" | "none" => Ok(AnimationChoice::Off),
        "pulse" => Ok(AnimationChoice::Pulse),
        "sweep" => Ok(AnimationChoice::Sweep),
        "shimmer" => Ok(AnimationChoice::Shimmer),
        "colour-cycle" | "color-cycle" => Ok(AnimationChoice::ColorCycle),
        "custom" => Ok(AnimationChoice::Custom),
        _ => Err(ThemeError::new(
            path,
            "must be off, pulse, sweep, shimmer, colour-cycle, or custom",
        )),
    }
}

fn parse_color(path: &str, value: &str) -> Result<Color, ThemeError> {
    if value == "default" {
        return Ok(Color::Default);
    }
    if let Some(index) = value.strip_prefix("ansi:") {
        return index
            .parse::<u8>()
            .map(Color::Indexed)
            .map_err(|_| ThemeError::new(path, "must be default, ansi:0..255, or #RRGGBB"));
    }
    if let Some(hex) = value.strip_prefix('#') {
        if hex.len() == 6 && hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            let red = u8::from_str_radix(&hex[0..2], 16).expect("validated hex");
            let green = u8::from_str_radix(&hex[2..4], 16).expect("validated hex");
            let blue = u8::from_str_radix(&hex[4..6], 16).expect("validated hex");
            return Ok(Color::Rgb(red, green, blue));
        }
    }
    Err(ThemeError::new(
        path,
        "must be default, ansi:0..255, or #RRGGBB",
    ))
}

fn named_glyphs(path: &str, name: &str) -> Result<BorderGlyphSet, ThemeError> {
    match name {
        "single" => Ok(BorderGlyphSet::SINGLE),
        "heavy" => Ok(BorderGlyphSet::HEAVY),
        "double" => Ok(BorderGlyphSet::DOUBLE),
        "ascii" => Ok(BorderGlyphSet::ASCII),
        _ => Err(ThemeError::new(
            path,
            "must be single, heavy, double, ascii, or a custom glyph map",
        )),
    }
}

fn parse_glyph(path: &str, value: &str) -> Result<char, ThemeError> {
    let mut chars = value.chars();
    let Some(glyph) = chars.next() else {
        return Err(ThemeError::new(path, "must contain one displayed cell"));
    };
    if chars.next().is_some() || glyph.is_control() || scalar_width(glyph) != 1 {
        return Err(ThemeError::new(path, "must contain one displayed cell"));
    }
    Ok(glyph)
}

fn validate_glyph_set(path: &str, glyphs: BorderGlyphSet) -> Result<(), ThemeError> {
    for (field, glyph) in [
        ("vertical", glyphs.vertical),
        ("horizontal", glyphs.horizontal),
        ("down_right", glyphs.down_right),
        ("down_left", glyphs.down_left),
        ("up_right", glyphs.up_right),
        ("up_left", glyphs.up_left),
        ("vertical_right", glyphs.vertical_right),
        ("vertical_left", glyphs.vertical_left),
        ("horizontal_down", glyphs.horizontal_down),
        ("horizontal_up", glyphs.horizontal_up),
        ("cross", glyphs.cross),
    ] {
        if glyph.is_control() || scalar_width(glyph) != 1 {
            return Err(ThemeError::new(
                format!("{path}.glyphs.{field}"),
                "must contain one displayed cell",
            ));
        }
    }
    Ok(())
}

fn parse_animation_target(path: &str, value: &str) -> Result<AnimationTarget, ThemeError> {
    match value {
        "border" | "borders" => Ok(AnimationTarget::Borders),
        "status" => Ok(AnimationTarget::Status),
        "both" => Ok(AnimationTarget::Both),
        _ => Err(ThemeError::new(path, "must be border, status, or both")),
    }
}

fn parse_playback(path: &str, value: &str) -> Result<Playback, ThemeError> {
    match value {
        "once" => Ok(Playback::Once),
        "loop" => Ok(Playback::Loop),
        _ => Err(ThemeError::new(path, "must be once or loop")),
    }
}

fn validate_fps(fps: Option<u8>, path: &str) -> Result<(), ThemeError> {
    if fps.is_some_and(|fps| fps == 0 || fps > MAX_THEME_FPS) {
        return Err(ThemeError::new(
            path,
            format!("must be between 1 and {MAX_THEME_FPS}"),
        ));
    }
    Ok(())
}

const fn frame_duration(fps: u8) -> u32 {
    1_000_u32.div_ceil(fps as u32)
}

fn validate_frame_duration(index: usize, duration_ms: u32) -> Result<(), ThemeError> {
    let minimum = frame_duration(MAX_THEME_FPS);
    if duration_ms < minimum {
        return Err(ThemeError::new(
            format!("animation.frames[{index}].duration_ms"),
            format!("must be at least {minimum}ms"),
        ));
    }
    Ok(())
}

fn validate_template(path: &str, template: &str) -> Result<(), ThemeError> {
    if template.len() > MAX_STATUS_TEMPLATE_BYTES {
        return Err(ThemeError::new(
            path,
            format!("exceeds {MAX_STATUS_TEMPLATE_BYTES} bytes"),
        ));
    }
    if template.chars().any(char::is_control) {
        return Err(ThemeError::new(path, "must not contain control characters"));
    }
    let mut rest = template;
    while let Some(open) = rest.find('{') {
        let after_open = &rest[open + 1..];
        let Some(close) = after_open.find('}') else {
            return Err(ThemeError::new(path, "contains an unclosed template field"));
        };
        let field = &after_open[..close];
        if !matches!(
            field,
            "session" | "window_index" | "window_name" | "pane_index" | "pane_title" | "windows"
        ) {
            return Err(ThemeError::new(
                path,
                format!("contains unknown template field {{{field}}}"),
            ));
        }
        rest = &after_open[close + 1..];
    }
    if rest.contains('}') {
        return Err(ThemeError::new(path, "contains an unmatched closing brace"));
    }
    Ok(())
}

fn validate_animation_status_widths(frames: &[UiFrame]) -> Result<(), ThemeError> {
    let Some(first) = frames.first() else {
        return Ok(());
    };
    let reference = status_template_widths(&first.status);
    for (index, frame) in frames.iter().enumerate().skip(1) {
        if status_template_widths(&frame.status) != reference {
            return Err(ThemeError::new(
                format!("animation.frames[{index}].status"),
                "decorations must keep the same displayed width in every frame",
            ));
        }
    }
    Ok(())
}

fn status_template_widths(status: &StatusTheme) -> [u16; 5] {
    [
        displayed_width(&status.left),
        displayed_width(&status.center),
        displayed_width(&status.window),
        displayed_width(&status.active_window),
        displayed_width(&status.right),
    ]
}

fn displayed_width(value: &str) -> u16 {
    value.chars().fold(0_u16, |width, character| {
        width.saturating_add(u16::from(scalar_width(character).min(2)))
    })
}

fn validate_resolved(theme: &UiTheme) -> Result<(), ThemeError> {
    if theme.name.len() > MAX_THEME_NAME_BYTES {
        return Err(ThemeError::new(
            "name",
            format!("exceeds {MAX_THEME_NAME_BYTES} bytes"),
        ));
    }
    validate_glyph_set("border", theme.base.border.glyphs)?;
    validate_glyph_set("active_border", theme.base.active_border.glyphs)?;
    for (field, value) in [
        ("left", theme.base.status.left.as_str()),
        ("center", theme.base.status.center.as_str()),
        ("window", theme.base.status.window.as_str()),
        ("active_window", theme.base.status.active_window.as_str()),
        ("right", theme.base.status.right.as_str()),
    ] {
        validate_template(&format!("status.{field}"), value)?;
    }
    if let Some(animation) = &theme.animation {
        validate_animation_status_widths(&animation.frames)?;
        for (index, frame) in animation.frames.iter().enumerate() {
            validate_frame_duration(index, frame.duration_ms)?;
            validate_glyph_set(
                &format!("animation.frames[{index}].border"),
                frame.border.glyphs,
            )?;
            validate_glyph_set(
                &format!("animation.frames[{index}].active_border"),
                frame.active_border.glyphs,
            )?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::parse_config;
    use wmux_core::{BorderGlyphSet, Color, Playback, UiTheme};

    #[test]
    fn missing_ui_keys_preserve_the_exact_default_theme() {
        let config = parse_config("agent_ui = plain\n").unwrap();

        assert_eq!(config.ui().resolve(None).unwrap(), UiTheme::default());
        assert!(config.ui().theme_file().is_none());
        assert!(config.ui().theme_provider().is_none());
    }

    #[test]
    fn provider_patch_precedes_explicit_overrides() {
        let config =
            parse_config("ui.theme = neon\nui.active_border.foreground = \"#ffffff\"\n").unwrap();
        let provider = br##"{"schema":1,"active_border":{"style":{"fg":"#ff0000"}}}"##;

        let theme = config.ui().resolve(Some(provider)).unwrap();

        assert_eq!(theme.name, "neon");
        assert_eq!(theme.base.active_border.style.fg, Color::Rgb(255, 255, 255));
    }

    #[test]
    fn schema_rejects_unknown_fields_and_wide_glyphs_with_paths() {
        let unknown = parse_theme_document(br#"{"schema":1,"bordr":{}}"#).unwrap_err();
        assert!(unknown.to_string().contains("bordr"));

        let wide = r#"{
            "schema": 1,
            "border": {
                "glyphs": {
                    "vertical": "界",
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
            }
        }"#;
        let error = parse_theme_document(wide.as_bytes()).unwrap_err();
        assert!(error.to_string().contains("border.glyphs.vertical"));
    }

    #[test]
    fn presets_are_static_legible_and_opt_in() {
        let double = parse_config("ui.theme = double\n")
            .unwrap()
            .ui()
            .resolve(None)
            .unwrap();
        assert_eq!(double.base.border.glyphs, BorderGlyphSet::DOUBLE);
        assert!(double.animation.is_none());

        let animated = parse_config(
            "ui.theme = sakura\nui.animation = pulse\nui.animation_fps = 10\nui.animation_playback = once\n",
        )
        .unwrap()
        .ui()
        .resolve(None)
        .unwrap();
        let animation = animated.animation.unwrap();
        assert_eq!(animation.playback, Playback::Once);
        assert!(animation
            .frames
            .iter()
            .all(|frame| frame.duration_ms == 100));
    }

    #[test]
    fn ui_assignments_do_not_leak_into_command_source() {
        let config =
            parse_config("ui.theme = minimal\nui.border.style = ascii\ndisplay-message ready\n")
                .unwrap();

        assert_eq!(
            config.command_source().text(),
            "\n\ndisplay-message ready\n"
        );
        assert_eq!(
            config.ui().resolve(None).unwrap().base.border.glyphs,
            BorderGlyphSet::ASCII
        );
    }

    #[test]
    fn document_animation_limits_are_reported_without_panics() {
        let too_fast = br#"{
            "schema": 1,
            "animation": {
                "target": "both",
                "playback": "loop",
                "frames": [{"duration_ms": 1}]
            }
        }"#;

        let patch = parse_theme_document(too_fast).unwrap();
        let sources = ThemeSources {
            overrides: patch,
            ..ThemeSources::default()
        };
        let error = sources.resolve(None).unwrap_err();
        assert!(error
            .to_string()
            .contains("animation.frames[0].duration_ms"));
    }
}
