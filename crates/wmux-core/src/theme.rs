use crate::Style;

pub const UP: u8 = 1;
pub const RIGHT: u8 = 2;
pub const DOWN: u8 = 4;
pub const LEFT: u8 = 8;
pub const MAX_THEME_FRAMES: usize = 64;
pub const MAX_THEME_FPS: u8 = 30;

const MIN_FRAME_DURATION_MS: u32 = 1_000_u32.div_ceil(MAX_THEME_FPS as u32);
const VERTICAL: u8 = UP | DOWN;
const HORIZONTAL: u8 = LEFT | RIGHT;
const DOWN_RIGHT: u8 = RIGHT | DOWN;
const DOWN_LEFT: u8 = LEFT | DOWN;
const UP_RIGHT: u8 = RIGHT | UP;
const UP_LEFT: u8 = LEFT | UP;
const VERTICAL_RIGHT: u8 = UP | RIGHT | DOWN;
const VERTICAL_LEFT: u8 = UP | LEFT | DOWN;
const HORIZONTAL_DOWN: u8 = LEFT | RIGHT | DOWN;
const HORIZONTAL_UP: u8 = LEFT | RIGHT | UP;
const CROSS: u8 = UP | RIGHT | DOWN | LEFT;

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

impl BorderGlyphSet {
    pub const SINGLE: Self = Self {
        vertical: '│',
        horizontal: '─',
        down_right: '┌',
        down_left: '┐',
        up_right: '└',
        up_left: '┘',
        vertical_right: '├',
        vertical_left: '┤',
        horizontal_down: '┬',
        horizontal_up: '┴',
        cross: '┼',
    };

    pub const HEAVY: Self = Self {
        vertical: '┃',
        horizontal: '━',
        down_right: '┏',
        down_left: '┓',
        up_right: '┗',
        up_left: '┛',
        vertical_right: '┣',
        vertical_left: '┫',
        horizontal_down: '┳',
        horizontal_up: '┻',
        cross: '╋',
    };

    pub const DOUBLE: Self = Self {
        vertical: '║',
        horizontal: '═',
        down_right: '╔',
        down_left: '╗',
        up_right: '╚',
        up_left: '╝',
        vertical_right: '╠',
        vertical_left: '╣',
        horizontal_down: '╦',
        horizontal_up: '╩',
        cross: '╬',
    };

    pub const ASCII: Self = Self {
        vertical: '|',
        horizontal: '-',
        down_right: '+',
        down_left: '+',
        up_right: '+',
        up_left: '+',
        vertical_right: '+',
        vertical_left: '+',
        horizontal_down: '+',
        horizontal_up: '+',
        cross: '+',
    };

    pub const fn glyph(self, directions: u8) -> char {
        match directions & (UP | RIGHT | DOWN | LEFT) {
            VERTICAL => self.vertical,
            HORIZONTAL => self.horizontal,
            DOWN_RIGHT => self.down_right,
            DOWN_LEFT => self.down_left,
            UP_RIGHT => self.up_right,
            UP_LEFT => self.up_left,
            VERTICAL_RIGHT => self.vertical_right,
            VERTICAL_LEFT => self.vertical_left,
            HORIZONTAL_DOWN => self.horizontal_down,
            HORIZONTAL_UP => self.horizontal_up,
            CROSS => self.cross,
            _ => ' ',
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BorderTheme {
    pub style: Style,
    pub glyphs: BorderGlyphSet,
    pub visible: bool,
}

impl Default for BorderTheme {
    fn default() -> Self {
        Self {
            style: Style::default(),
            glyphs: BorderGlyphSet::SINGLE,
            visible: true,
        }
    }
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

impl Default for StatusTheme {
    fn default() -> Self {
        let reversed = Style {
            reverse: true,
            ..Style::default()
        };
        Self {
            base: reversed,
            left_style: reversed,
            center_style: reversed,
            window_style: reversed,
            active_window_style: Style {
                bold: true,
                ..Style::default()
            },
            right_style: reversed,
            prompt_style: Style::default(),
            left: " wmux · {session} ".to_owned(),
            center: "{windows}".to_owned(),
            window: " {window_index}:{window_name} ".to_owned(),
            active_window: " [{window_index}:{window_name}] ".to_owned(),
            right: " pane {pane_index} · {pane_title} ".to_owned(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UiFrame {
    pub duration_ms: u32,
    pub border: BorderTheme,
    pub active_border: BorderTheme,
    pub status: StatusTheme,
}

impl Default for UiFrame {
    fn default() -> Self {
        Self {
            duration_ms: 0,
            border: BorderTheme::default(),
            active_border: BorderTheme {
                style: Style {
                    bold: true,
                    ..Style::default()
                },
                glyphs: BorderGlyphSet::HEAVY,
                visible: true,
            },
            status: StatusTheme::default(),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct UiTheme {
    pub name: String,
    pub base: UiFrame,
    pub animation: Option<AnimationSpec>,
}

impl Default for UiTheme {
    fn default() -> Self {
        Self {
            name: "default".to_owned(),
            base: UiFrame::default(),
            animation: None,
        }
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum AnimationTarget {
    Borders,
    Status,
    Both,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Playback {
    Once,
    Loop,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct FrameSelection {
    pub index: usize,
    pub next_in_ms: Option<u32>,
}

impl FrameSelection {
    pub const fn new(index: usize, next_in_ms: Option<u32>) -> Self {
        Self { index, next_in_ms }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct AnimationSpec {
    pub target: AnimationTarget,
    pub playback: Playback,
    pub frames: Vec<UiFrame>,
    total_duration_ms: u64,
}

impl AnimationSpec {
    pub fn new(
        target: AnimationTarget,
        playback: Playback,
        frames: Vec<UiFrame>,
    ) -> Result<Self, String> {
        if frames.is_empty() {
            return Err("animation must contain at least one frame".to_owned());
        }
        if frames.len() > MAX_THEME_FRAMES {
            return Err(format!(
                "animation has {} frames; maximum is {MAX_THEME_FRAMES}",
                frames.len()
            ));
        }
        if let Some((index, frame)) = frames
            .iter()
            .enumerate()
            .find(|(_, frame)| frame.duration_ms < MIN_FRAME_DURATION_MS)
        {
            return Err(format!(
                "animation frame {index} lasts {}ms; minimum is {MIN_FRAME_DURATION_MS}ms",
                frame.duration_ms
            ));
        }

        let total_duration_ms = frames
            .iter()
            .map(|frame| u64::from(frame.duration_ms))
            .sum();
        Ok(Self {
            target,
            playback,
            frames,
            total_duration_ms,
        })
    }

    pub fn select(&self, elapsed_ms: u64) -> FrameSelection {
        if self.playback == Playback::Once && elapsed_ms >= self.total_duration_ms {
            return FrameSelection::new(self.frames.len() - 1, None);
        }

        let offset = match self.playback {
            Playback::Once => elapsed_ms,
            Playback::Loop => elapsed_ms % self.total_duration_ms,
        };
        let mut frame_start = 0_u64;
        for (index, frame) in self.frames.iter().enumerate() {
            let frame_end = frame_start + u64::from(frame.duration_ms);
            if offset < frame_end {
                let remaining = frame_end - offset;
                return FrameSelection::new(index, Some(remaining as u32));
            }
            frame_start = frame_end;
        }

        FrameSelection::new(self.frames.len() - 1, None)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::Style;

    fn test_frame(duration_ms: u32) -> UiFrame {
        UiFrame {
            duration_ms,
            ..UiFrame::default()
        }
    }

    #[test]
    fn default_theme_preserves_current_terminal_native_ui() {
        let theme = UiTheme::default();

        assert_eq!(theme.base.border.style, Style::default());
        assert_eq!(theme.base.border.glyphs, BorderGlyphSet::SINGLE);
        assert!(theme.base.border.visible);
        assert_eq!(theme.base.active_border.glyphs, BorderGlyphSet::HEAVY);
        assert!(theme.base.active_border.style.bold);
        assert!(theme.base.active_border.visible);
        assert!(theme.base.status.base.reverse);
        assert!(theme.animation.is_none());
    }

    #[test]
    fn glyph_sets_resolve_every_connected_topology() {
        let cases = [
            (UP | DOWN, '│'),
            (LEFT | RIGHT, '─'),
            (RIGHT | DOWN, '┌'),
            (LEFT | DOWN, '┐'),
            (RIGHT | UP, '└'),
            (LEFT | UP, '┘'),
            (UP | RIGHT | DOWN, '├'),
            (UP | LEFT | DOWN, '┤'),
            (LEFT | RIGHT | DOWN, '┬'),
            (LEFT | RIGHT | UP, '┴'),
            (UP | RIGHT | DOWN | LEFT, '┼'),
        ];

        for (directions, expected) in cases {
            assert_eq!(BorderGlyphSet::SINGLE.glyph(directions), expected);
        }
        assert_eq!(BorderGlyphSet::DOUBLE.glyph(UP | RIGHT | DOWN | LEFT), '╬');
        assert_eq!(BorderGlyphSet::ASCII.glyph(UP | RIGHT | DOWN | LEFT), '+');
    }

    #[test]
    fn once_and_loop_frame_selection_are_deterministic() {
        let frames = vec![test_frame(100), test_frame(200)];
        let once =
            AnimationSpec::new(AnimationTarget::Both, Playback::Once, frames.clone()).unwrap();

        assert_eq!(once.select(0), FrameSelection::new(0, Some(100)));
        assert_eq!(once.select(99), FrameSelection::new(0, Some(1)));
        assert_eq!(once.select(100), FrameSelection::new(1, Some(200)));
        assert_eq!(once.select(299), FrameSelection::new(1, Some(1)));
        assert_eq!(once.select(300), FrameSelection::new(1, None));

        let looping = AnimationSpec::new(AnimationTarget::Both, Playback::Loop, frames).unwrap();
        assert_eq!(looping.select(300), FrameSelection::new(0, Some(100)));
        assert_eq!(looping.select(350), FrameSelection::new(0, Some(50)));
    }

    #[test]
    fn animation_limits_reject_unsafe_frame_sets() {
        let too_fast = AnimationSpec::new(
            AnimationTarget::Borders,
            Playback::Loop,
            vec![test_frame(33)],
        );
        assert!(too_fast.is_err());

        let too_many = AnimationSpec::new(
            AnimationTarget::Status,
            Playback::Once,
            vec![test_frame(100); MAX_THEME_FRAMES + 1],
        );
        assert!(too_many.is_err());
    }
}
