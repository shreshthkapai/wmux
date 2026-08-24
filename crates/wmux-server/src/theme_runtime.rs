use std::{
    sync::Arc,
    time::{Duration, Instant},
};

use wmux_config::{ThemeError, ThemeSources, WmuxConfig};
use wmux_core::{JobId, UiFrame, UiTheme};

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
    requested_generation: u64,
    active: Arc<UiTheme>,
    sources: ThemeSources,
    pending_provider: Option<PendingProvider>,
}

pub(crate) struct PendingProvider {
    pub(crate) generation: u64,
    pub(crate) job: JobId,
    pub(crate) deadline: Instant,
    pub(crate) sources: ThemeSources,
    pub(crate) static_candidate: Arc<UiTheme>,
    pub(crate) failure: Option<String>,
    pub(crate) termination_requested: bool,
}

pub(crate) struct ClientThemeState {
    generation: u64,
    started_at: Instant,
    rendered_frame: usize,
    next_frame: Option<Instant>,
}

impl ClientThemeState {
    pub(crate) const fn new(generation: u64, started_at: Instant) -> Self {
        Self {
            generation,
            started_at,
            rendered_frame: 0,
            next_frame: None,
        }
    }

    pub(crate) fn select<'a>(
        &mut self,
        theme: &'a UiTheme,
        generation: u64,
        now: Instant,
    ) -> (&'a UiFrame, Option<Instant>) {
        if self.generation != generation {
            self.generation = generation;
            self.started_at = now;
            self.rendered_frame = 0;
            self.next_frame = None;
        }
        let Some(animation) = &theme.animation else {
            self.rendered_frame = 0;
            self.next_frame = None;
            return (&theme.base, None);
        };
        let elapsed_ms = now
            .saturating_duration_since(self.started_at)
            .as_millis()
            .min(u128::from(u64::MAX)) as u64;
        let selection = animation.select(elapsed_ms);
        self.rendered_frame = selection.index;
        self.next_frame = selection
            .next_in_ms
            .and_then(|millis| now.checked_add(Duration::from_millis(u64::from(millis))));
        (&animation.frames[selection.index], self.next_frame)
    }

    pub(crate) fn next_deadline(&self, theme: &UiTheme) -> Option<Instant> {
        theme.animation.as_ref()?;
        self.next_frame
    }
}

impl ThemeRuntime {
    pub(crate) fn new(sources: ThemeSources) -> Result<Self, ThemeError> {
        let active = Arc::new(sources.resolve(None)?);
        Ok(Self {
            generation: 1,
            requested_generation: 1,
            active,
            sources,
            pending_provider: None,
        })
    }

    pub(crate) fn active(&self) -> &UiTheme {
        &self.active
    }

    pub(crate) const fn generation(&self) -> u64 {
        self.generation
    }

    pub(crate) fn replace(&mut self, sources: ThemeSources, theme: UiTheme) -> u64 {
        let generation = self.begin_request();
        let replaced = self.commit(generation, sources, theme);
        debug_assert!(replaced);
        generation
    }

    pub(crate) fn begin_request(&mut self) -> u64 {
        self.requested_generation = self.requested_generation.saturating_add(1);
        self.requested_generation
    }

    pub(crate) fn commit(
        &mut self,
        generation: u64,
        sources: ThemeSources,
        theme: UiTheme,
    ) -> bool {
        if generation != self.requested_generation {
            return false;
        }
        self.generation = generation;
        self.active = Arc::new(theme);
        self.sources = sources;
        true
    }

    pub(crate) fn install_provider(&mut self, pending: PendingProvider) -> Option<PendingProvider> {
        self.pending_provider.replace(pending)
    }

    pub(crate) fn pending_provider(&self) -> Option<&PendingProvider> {
        self.pending_provider.as_ref()
    }

    pub(crate) fn pending_provider_mut(&mut self, job: JobId) -> Option<&mut PendingProvider> {
        self.pending_provider
            .as_mut()
            .filter(|pending| pending.job == job)
    }

    pub(crate) fn take_pending_provider(
        &mut self,
        job: JobId,
        generation: u64,
    ) -> Option<PendingProvider> {
        let matches = self
            .pending_provider
            .as_ref()
            .is_some_and(|pending| pending.job == job && pending.generation == generation);
        matches.then(|| self.pending_provider.take()).flatten()
    }

    pub(crate) fn cancel_pending_provider(&mut self) -> Option<PendingProvider> {
        self.pending_provider.take()
    }

    pub(crate) fn provider_deadline(&self) -> Option<Instant> {
        self.pending_provider
            .as_ref()
            .filter(|pending| !pending.termination_requested)
            .map(|pending| pending.deadline)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};
    use wmux_config::{parse_config, ThemeSources};
    use wmux_core::{AnimationSpec, AnimationTarget, BorderGlyphSet, Playback, UiFrame, UiTheme};

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

    fn looping_theme() -> UiTheme {
        let first = UiFrame {
            duration_ms: 200,
            ..UiFrame::default()
        };
        let mut second = first.clone();
        second.active_border.style.dim = true;
        UiTheme {
            name: "looping".to_owned(),
            base: UiFrame::default(),
            animation: Some(
                AnimationSpec::new(AnimationTarget::Both, Playback::Loop, vec![first, second])
                    .unwrap(),
            ),
        }
    }

    #[test]
    fn static_theme_never_schedules_an_animation_deadline() {
        let start = Instant::now();
        let mut state = ClientThemeState::new(1, start);
        let theme = UiTheme::default();

        let (frame, deadline) = state.select(&theme, 1, start);

        assert_eq!(frame, &UiFrame::default());
        assert_eq!(deadline, None);
        assert_eq!(state.next_deadline(&theme), None);
    }

    #[test]
    fn looping_theme_schedules_one_boundary_and_skips_backlog() {
        let theme = looping_theme();
        let start = Instant::now();
        let mut state = ClientThemeState::new(7, start);

        let (first, first_deadline) = state.select(&theme, 7, start);
        assert_eq!(first, &theme.animation.as_ref().unwrap().frames[0]);
        assert_eq!(first_deadline, Some(start + Duration::from_millis(200)));

        let late = start + Duration::from_millis(650);
        let (current, current_deadline) = state.select(&theme, 7, late);
        assert_eq!(current, &theme.animation.as_ref().unwrap().frames[1]);
        assert_eq!(current_deadline, Some(start + Duration::from_millis(800)));
        assert_eq!(state.next_deadline(&theme), current_deadline);
    }

    #[test]
    fn new_generation_restarts_the_client_clock_at_frame_zero() {
        let theme = looping_theme();
        let start = Instant::now();
        let mut state = ClientThemeState::new(1, start);
        state.select(&theme, 1, start + Duration::from_millis(250));

        let reload = start + Duration::from_secs(3);
        let (frame, deadline) = state.select(&theme, 2, reload);

        assert_eq!(frame, &theme.animation.as_ref().unwrap().frames[0]);
        assert_eq!(deadline, Some(reload + Duration::from_millis(200)));
    }
}
