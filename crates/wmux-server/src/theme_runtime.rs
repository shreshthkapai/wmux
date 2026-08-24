use std::sync::Arc;

use wmux_config::{ThemeError, ThemeSources, WmuxConfig};
use wmux_core::UiTheme;

pub(crate) trait ThemeLoader: Send + Sync {
    fn load_sources(&self) -> Result<ThemeSources, String>;
}

pub(crate) struct ConfigThemeLoader;

impl ThemeLoader for ConfigThemeLoader {
    fn load_sources(&self) -> Result<ThemeSources, String> {
        WmuxConfig::load_or_create()
            .map(|config| config.ui().clone())
            .map_err(|error| error.to_string())
    }
}

pub(crate) struct ThemeRuntime {
    generation: u64,
    active: Arc<UiTheme>,
    sources: ThemeSources,
}

impl ThemeRuntime {
    pub(crate) fn new(sources: ThemeSources) -> Result<Self, ThemeError> {
        let active = Arc::new(sources.resolve(None)?);
        Ok(Self {
            generation: 1,
            active,
            sources,
        })
    }

    pub(crate) fn active(&self) -> &UiTheme {
        &self.active
    }

    pub(crate) const fn generation(&self) -> u64 {
        self.generation
    }

    pub(crate) fn replace(&mut self, sources: ThemeSources, theme: UiTheme) -> u64 {
        self.generation = self.generation.saturating_add(1);
        self.active = Arc::new(theme);
        self.sources = sources;
        self.generation
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use wmux_config::{parse_config, ThemeSources};
    use wmux_core::{BorderGlyphSet, UiTheme};

    fn sources(config: &str) -> ThemeSources {
        parse_config(config).unwrap().ui().clone()
    }

    #[test]
    fn replacement_is_atomic_and_advances_one_generation() {
        let mut runtime = ThemeRuntime::new(ThemeSources::default()).unwrap();
        let replacement_sources = sources("ui.theme = double\n");
        let replacement = replacement_sources.resolve(None).unwrap();

        let generation = runtime.replace(replacement_sources, replacement);

        assert_eq!(generation, 2);
        assert_eq!(runtime.generation(), 2);
        assert_eq!(runtime.active().name, "double");
        assert_eq!(runtime.active().base.border.glyphs, BorderGlyphSet::DOUBLE);
    }

    #[test]
    fn invalid_candidate_does_not_mutate_the_active_generation() {
        let runtime = ThemeRuntime::new(ThemeSources::default()).unwrap();
        let before: UiTheme = runtime.active().clone();

        let error = sources("ui.theme = missing\n").resolve(None).unwrap_err();

        assert!(error.to_string().contains("missing"));
        assert_eq!(runtime.generation(), 1);
        assert_eq!(runtime.active(), &before);
    }
}
