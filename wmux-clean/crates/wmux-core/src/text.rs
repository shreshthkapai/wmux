use std::{fmt, sync::Arc};

use smallvec::SmallVec;
use unicode_segmentation::UnicodeSegmentation;
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

/// tmux's `UTF8_SIZE` is 32 bytes, including joined and combining scalars.
pub const MAX_CELL_TEXT_BYTES: usize = 32;

/// Text stored in one terminal grid cell.
///
/// The overwhelmingly common single-scalar case stays inline. Combined text
/// owns a growable string behind `Arc`, so cloned lines and render baselines
/// share the uncommon overflow allocation until a later mutation.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub enum CellText {
    Scalar(char),
    Combined(Arc<String>),
}

impl CellText {
    pub fn first_char(&self) -> char {
        match self {
            Self::Scalar(ch) => *ch,
            Self::Combined(text) => text
                .chars()
                .next()
                .expect("combined CellText always retains its base scalar"),
        }
    }

    pub const fn has_owned_overflow(&self) -> bool {
        matches!(self, Self::Combined(_))
    }

    pub fn byte_len(&self) -> usize {
        match self {
            Self::Scalar(ch) => ch.len_utf8(),
            Self::Combined(text) => text.len(),
        }
    }

    pub fn ends_with(&self, ch: char) -> bool {
        match self {
            Self::Scalar(current) => *current == ch,
            Self::Combined(text) => text.ends_with(ch),
        }
    }

    /// Append a scalar atomically when the cell's compatibility limit allows
    /// it. Grapheme-boundary decisions are deliberately made by the caller.
    pub fn try_append(&mut self, ch: char) -> bool {
        if self.byte_len().saturating_add(ch.len_utf8()) > MAX_CELL_TEXT_BYTES {
            return false;
        }

        match self {
            Self::Scalar(first) => {
                let first = *first;
                let mut combined = String::with_capacity(first.len_utf8() + ch.len_utf8());
                combined.push(first);
                combined.push(ch);
                *self = Self::Combined(Arc::new(combined));
            }
            Self::Combined(text) => Arc::make_mut(text).push(ch),
        }
        true
    }

    pub fn display_width(&self) -> u8 {
        let width = match self {
            Self::Scalar(ch) => UnicodeWidthChar::width(*ch).unwrap_or(0),
            Self::Combined(text) => UnicodeWidthStr::width(text.as_str()),
        };
        width.min(2) as u8
    }

    pub fn push_to(&self, out: &mut String) {
        match self {
            Self::Scalar(ch) => out.push(*ch),
            Self::Combined(text) => out.push_str(text),
        }
    }

    pub fn write_utf8(&self, out: &mut Vec<u8>) {
        match self {
            Self::Scalar(ch) => {
                let mut encoded = [0; 4];
                out.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
            }
            Self::Combined(text) => out.extend_from_slice(text.as_bytes()),
        }
    }

    fn append_bytes(&self, out: &mut SmallVec<[u8; MAX_CELL_TEXT_BYTES + 4]>) {
        match self {
            Self::Scalar(ch) => {
                let mut encoded = [0; 4];
                out.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
            }
            Self::Combined(text) => out.extend_from_slice(text.as_bytes()),
        }
    }
}

impl From<char> for CellText {
    fn from(ch: char) -> Self {
        Self::Scalar(ch)
    }
}

impl fmt::Display for CellText {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Scalar(ch) => formatter.write_fmt(format_args!("{ch}")),
            Self::Combined(text) => formatter.write_str(text),
        }
    }
}

pub fn scalar_width(ch: char) -> u8 {
    UnicodeWidthChar::width(ch).unwrap_or(0).min(2) as u8
}

/// Return whether `next` belongs to the same extended grapheme as `text`.
pub fn extends_grapheme(text: &CellText, next: char) -> bool {
    if text.byte_len().saturating_add(next.len_utf8()) > MAX_CELL_TEXT_BYTES {
        return false;
    }

    let mut candidate = SmallVec::<[u8; MAX_CELL_TEXT_BYTES + 4]>::new();
    text.append_bytes(&mut candidate);
    let mut encoded = [0; 4];
    candidate.extend_from_slice(next.encode_utf8(&mut encoded).as_bytes());
    let candidate = std::str::from_utf8(&candidate).expect("CellText always contains valid UTF-8");
    let mut graphemes = UnicodeSegmentation::graphemes(candidate, true);
    graphemes.next().is_some() && graphemes.next().is_none()
}

#[cfg(test)]
mod tests {
    use std::mem::size_of;

    use super::{extends_grapheme, scalar_width, CellText, MAX_CELL_TEXT_BYTES};

    #[test]
    fn scalar_uses_no_owned_overflow_until_a_combining_mark_arrives() {
        let mut text = CellText::from('e');

        assert!(size_of::<CellText>() <= 16);
        assert!(!text.has_owned_overflow());
        assert!(text.try_append('\u{301}'));
        assert!(text.has_owned_overflow());
        assert_eq!(text.to_string(), "e\u{301}");
        assert_eq!(text.first_char(), 'e');
    }

    #[test]
    fn emoji_sequences_use_string_width_not_scalar_width_sum() {
        let mut text = CellText::from('\u{1f469}');

        assert!(extends_grapheme(&text, '\u{200d}'));
        assert!(text.try_append('\u{200d}'));
        assert!(extends_grapheme(&text, '\u{1f4bb}'));
        assert!(text.try_append('\u{1f4bb}'));

        assert_eq!(text.to_string(), "\u{1f469}\u{200d}\u{1f4bb}");
        assert_eq!(text.display_width(), 2);
    }

    #[test]
    fn variation_selector_changes_the_complete_sequence_width() {
        let mut text = CellText::from('\u{2764}');

        assert_eq!(text.display_width(), 1);
        assert!(extends_grapheme(&text, '\u{fe0f}'));
        assert!(text.try_append('\u{fe0f}'));

        assert_eq!(text.to_string(), "\u{2764}\u{fe0f}");
        assert_eq!(text.display_width(), 2);
    }

    #[test]
    fn regional_indicators_pair_without_absorbing_a_third_flag_scalar() {
        let mut text = CellText::from('\u{1f1ec}');

        assert!(extends_grapheme(&text, '\u{1f1e7}'));
        assert!(text.try_append('\u{1f1e7}'));
        assert!(!extends_grapheme(&text, '\u{1f1fa}'));
        assert_eq!(text.display_width(), 2);
    }

    #[test]
    fn scalar_width_uses_reviewed_unicode_tables() {
        assert_eq!(scalar_width('a'), 1);
        assert_eq!(scalar_width('\u{301}'), 0);
        assert_eq!(scalar_width('\u{754c}'), 2);
    }

    #[test]
    fn cell_text_rejects_extensions_past_tmux_compatible_limit_atomically() {
        let mut text = CellText::from('a');
        for _ in 0..15 {
            assert!(text.try_append('\u{301}'));
        }
        assert_eq!(text.byte_len(), MAX_CELL_TEXT_BYTES - 1);

        let before = text.clone();
        assert!(!text.try_append('\u{301}'));
        assert_eq!(text, before);
    }
}
