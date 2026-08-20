use std::{
    fmt,
    hash::{Hash, Hasher},
    num::NonZeroUsize,
    sync::Arc,
};

use smallvec::SmallVec;
use unicode_segmentation::UnicodeSegmentation;
use unicode_width::{UnicodeWidthChar, UnicodeWidthStr};

/// tmux's `UTF8_SIZE` is 32 bytes, including joined and combining scalars.
pub const MAX_CELL_TEXT_BYTES: usize = 32;

/// Text stored in one terminal grid cell.
///
/// The overwhelmingly common single-scalar case stays inline. Combined text
/// owns a string behind `Arc`, so cloned lines and render baselines share the
/// uncommon overflow allocation.
///
/// The low bit tags an `Arc<String>` raw pointer. Inline scalars encode
/// `(scalar + 1) << 1`, leaving zero unavailable so the entire value remains
/// one word while retaining the `NonZeroUsize` niche.
#[repr(transparent)]
pub struct CellText(NonZeroUsize);

impl CellText {
    const POINTER_TAG: usize = 1;

    pub const fn from_char_const(ch: char) -> Self {
        let encoded = ((ch as usize) + 1) << 1;
        // SAFETY: adding one before shifting makes every scalar encoding nonzero.
        Self(unsafe { NonZeroUsize::new_unchecked(encoded) })
    }

    pub fn first_char(&self) -> char {
        match self.scalar() {
            Some(ch) => ch,
            None => self
                .combined()
                .chars()
                .next()
                .expect("combined CellText always retains its base scalar"),
        }
    }

    pub const fn has_owned_overflow(&self) -> bool {
        self.0.get() & Self::POINTER_TAG != 0
    }

    pub fn is_single_char(&self, expected: char) -> bool {
        self.scalar() == Some(expected)
    }

    pub fn byte_len(&self) -> usize {
        match self.scalar() {
            Some(ch) => ch.len_utf8(),
            None => self.combined().len(),
        }
    }

    pub fn ends_with(&self, ch: char) -> bool {
        match self.scalar() {
            Some(current) => current == ch,
            None => self.combined().ends_with(ch),
        }
    }

    /// Append a scalar atomically when the cell's compatibility limit allows
    /// it. Grapheme-boundary decisions are deliberately made by the caller.
    pub fn try_append(&mut self, ch: char) -> bool {
        if self.byte_len().saturating_add(ch.len_utf8()) > MAX_CELL_TEXT_BYTES {
            return false;
        }

        let mut combined = String::with_capacity(self.byte_len() + ch.len_utf8());
        self.push_to(&mut combined);
        combined.push(ch);
        *self = Self::from_combined(combined);
        true
    }

    pub fn display_width(&self) -> u8 {
        let width = match self.scalar() {
            Some(ch) => UnicodeWidthChar::width(ch).unwrap_or(0),
            None => UnicodeWidthStr::width(self.combined().as_str()),
        };
        width.min(2) as u8
    }

    pub fn push_to(&self, out: &mut String) {
        match self.scalar() {
            Some(ch) => out.push(ch),
            None => out.push_str(self.combined()),
        }
    }

    pub fn write_utf8(&self, out: &mut Vec<u8>) {
        match self.scalar() {
            Some(ch) => {
                let mut encoded = [0; 4];
                out.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
            }
            None => out.extend_from_slice(self.combined().as_bytes()),
        }
    }

    fn append_bytes(&self, out: &mut SmallVec<[u8; MAX_CELL_TEXT_BYTES + 4]>) {
        match self.scalar() {
            Some(ch) => {
                let mut encoded = [0; 4];
                out.extend_from_slice(ch.encode_utf8(&mut encoded).as_bytes());
            }
            None => out.extend_from_slice(self.combined().as_bytes()),
        }
    }

    fn scalar(&self) -> Option<char> {
        if self.has_owned_overflow() {
            return None;
        }
        let scalar = (self.0.get() >> 1) - 1;
        // SAFETY: the only non-pointer constructor accepts a valid `char` and
        // stores that scalar unchanged in the tagged word.
        Some(unsafe { char::from_u32_unchecked(scalar as u32) })
    }

    fn from_combined(text: String) -> Self {
        let raw = Arc::into_raw(Arc::new(text));
        let address = raw.expose_provenance();
        debug_assert_eq!(address & Self::POINTER_TAG, 0);
        // SAFETY: `Arc<String>` is aligned to at least two bytes, so tagging its
        // non-null allocation pointer leaves a nonzero value.
        Self(unsafe { NonZeroUsize::new_unchecked(address | Self::POINTER_TAG) })
    }

    fn combined_ptr(&self) -> *const String {
        debug_assert!(self.has_owned_overflow());
        std::ptr::with_exposed_provenance(self.0.get() & !Self::POINTER_TAG)
    }

    fn combined(&self) -> &String {
        // SAFETY: combined values originate exclusively from `Arc::into_raw`.
        // Clone and Drop maintain one strong count for every tagged owner.
        unsafe { &*self.combined_ptr() }
    }
}

impl Clone for CellText {
    fn clone(&self) -> Self {
        if self.has_owned_overflow() {
            // SAFETY: `combined_ptr` is a live pointer produced by
            // `Arc::into_raw`; the clone owns the incremented strong count.
            unsafe { Arc::increment_strong_count(self.combined_ptr()) };
        }
        Self(self.0)
    }
}

impl Drop for CellText {
    fn drop(&mut self) {
        if self.has_owned_overflow() {
            // SAFETY: this value owns exactly one strong count established by
            // `from_combined` or `clone`, and Drop consumes that count once.
            unsafe { Arc::decrement_strong_count(self.combined_ptr()) };
        }
    }
}

impl fmt::Debug for CellText {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("CellText")
            .field(&self.to_string())
            .finish()
    }
}

impl PartialEq for CellText {
    fn eq(&self, other: &Self) -> bool {
        match (self.scalar(), other.scalar()) {
            (Some(left), Some(right)) => left == right,
            (None, None) => self.combined() == other.combined(),
            _ => false,
        }
    }
}

impl Eq for CellText {}

impl Hash for CellText {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match self.scalar() {
            Some(ch) => ch.hash(state),
            None => self.combined().hash(state),
        }
    }
}

impl From<char> for CellText {
    fn from(ch: char) -> Self {
        Self::from_char_const(ch)
    }
}

impl fmt::Display for CellText {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.scalar() {
            Some(ch) => formatter.write_fmt(format_args!("{ch}")),
            None => formatter.write_str(self.combined()),
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

        assert_eq!(size_of::<CellText>(), size_of::<usize>());
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

    #[test]
    fn cloned_combined_text_keeps_its_allocation_alive_and_mutates_independently() {
        let mut original = CellText::from('e');
        assert!(original.try_append('\u{301}'));
        let mut clone = original.clone();
        drop(original);

        assert_eq!(clone.to_string(), "e\u{301}");
        assert!(clone.try_append('\u{301}'));
        assert_eq!(clone.to_string(), "e\u{301}\u{301}");
    }
}
