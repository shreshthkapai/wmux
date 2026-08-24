use std::{
    collections::{BTreeMap, VecDeque},
    fmt,
    sync::Arc,
};

pub const MAX_PASTE_BUFFER_BYTES: usize = 16 * 1024 * 1024;
pub const MAX_PASTE_TOTAL_BYTES: usize = 64 * 1024 * 1024;
pub const MAX_PASTE_BUFFER_NAME_BYTES: usize = 256;

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PasteBuffer {
    name: String,
    data: Arc<[u8]>,
    automatic: bool,
    order: u64,
}

impl PasteBuffer {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn data(&self) -> &[u8] {
        &self.data
    }

    pub fn shared_data(&self) -> Arc<[u8]> {
        Arc::clone(&self.data)
    }

    pub const fn automatic(&self) -> bool {
        self.automatic
    }

    pub const fn order(&self) -> u64 {
        self.order
    }

    pub fn sample(&self, width: usize) -> String {
        let mut sample = String::new();
        for byte in self.data.iter().take(width) {
            match byte {
                b'\n' => sample.push_str("\\n"),
                b'\r' => sample.push_str("\\r"),
                b'\t' => sample.push_str("\\t"),
                b'\\' => sample.push_str("\\\\"),
                0 => sample.push_str("\\0"),
                0x20..=0x7e => sample.push(char::from(*byte)),
                _ => sample.push_str(&format!("\\x{byte:02x}")),
            }
        }
        if self.data.len() > width {
            sample.push_str("...");
        }
        sample
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PasteBufferError(String);

impl PasteBufferError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for PasteBufferError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for PasteBufferError {}

#[derive(Debug)]
pub struct PasteBufferStore {
    by_name: BTreeMap<String, PasteBuffer>,
    newest: VecDeque<String>,
    next_name: u64,
    next_order: u64,
    automatic_count: usize,
    bytes: usize,
    limit: usize,
    max_buffer_bytes: usize,
    max_total_bytes: usize,
}

impl PasteBufferStore {
    pub fn new(limit: usize) -> Result<Self, PasteBufferError> {
        Self::build(limit, MAX_PASTE_BUFFER_BYTES, MAX_PASTE_TOTAL_BYTES)
    }

    fn build(
        limit: usize,
        max_buffer_bytes: usize,
        max_total_bytes: usize,
    ) -> Result<Self, PasteBufferError> {
        if limit == 0 {
            return Err(PasteBufferError::new("buffer limit must be at least one"));
        }
        if max_buffer_bytes == 0 || max_total_bytes < max_buffer_bytes {
            return Err(PasteBufferError::new("invalid paste buffer byte limits"));
        }
        Ok(Self {
            by_name: BTreeMap::new(),
            newest: VecDeque::new(),
            next_name: 0,
            next_order: 0,
            automatic_count: 0,
            bytes: 0,
            limit,
            max_buffer_bytes,
            max_total_bytes,
        })
    }

    #[cfg(test)]
    fn with_limits(limit: usize, max_buffer_bytes: usize, max_total_bytes: usize) -> Self {
        Self::build(limit, max_buffer_bytes, max_total_bytes).unwrap()
    }

    pub fn len(&self) -> usize {
        self.by_name.len()
    }

    pub fn is_empty(&self) -> bool {
        self.by_name.is_empty()
    }

    pub const fn bytes(&self) -> usize {
        self.bytes
    }

    pub const fn limit(&self) -> usize {
        self.limit
    }

    pub fn set_limit(&mut self, limit: usize) -> Result<(), PasteBufferError> {
        if limit == 0 {
            return Err(PasteBufferError::new("buffer limit must be at least one"));
        }
        self.limit = limit;
        while self.automatic_count > limit {
            let Some(name) = self.oldest_automatic_name() else {
                break;
            };
            self.remove_existing(&name);
        }
        Ok(())
    }

    pub fn set_named(
        &mut self,
        name: impl Into<String>,
        data: impl Into<Arc<[u8]>>,
    ) -> Result<(), PasteBufferError> {
        let name = name.into();
        validate_name(&name)?;
        let data = data.into();
        self.validate_data(&data)?;
        let replaced_bytes = self
            .by_name
            .get(&name)
            .map_or(0, |buffer| buffer.data.len());
        let prospective_bytes = self
            .bytes
            .checked_sub(replaced_bytes)
            .and_then(|bytes| bytes.checked_add(data.len()))
            .ok_or_else(|| PasteBufferError::new("paste buffer byte count overflow"))?;
        if prospective_bytes > self.max_total_bytes {
            return Err(PasteBufferError::new(
                "paste buffer storage exceeds 67108864 bytes",
            ));
        }

        self.remove_existing(&name);
        self.insert(name, data, false);
        Ok(())
    }

    pub fn add_automatic(
        &mut self,
        data: impl Into<Arc<[u8]>>,
    ) -> Result<Option<String>, PasteBufferError> {
        let data = data.into();
        if data.is_empty() {
            return Ok(None);
        }
        self.validate_data(&data)?;

        let mut evictions = Vec::new();
        let mut prospective_count = self.automatic_count + 1;
        let mut prospective_bytes = self
            .bytes
            .checked_add(data.len())
            .ok_or_else(|| PasteBufferError::new("paste buffer byte count overflow"))?;
        for name in self.newest.iter().rev() {
            if prospective_count <= self.limit && prospective_bytes <= self.max_total_bytes {
                break;
            }
            let Some(buffer) = self.by_name.get(name).filter(|buffer| buffer.automatic) else {
                continue;
            };
            prospective_count -= 1;
            prospective_bytes -= buffer.data.len();
            evictions.push(name.clone());
        }
        if prospective_count > self.limit || prospective_bytes > self.max_total_bytes {
            return Err(PasteBufferError::new(
                "paste buffer storage cannot fit without removing a named buffer",
            ));
        }

        let name = self.next_automatic_name();
        for eviction in evictions {
            self.remove_existing(&eviction);
        }
        self.insert(name.clone(), data, true);
        Ok(Some(name))
    }

    pub fn get(&self, name: Option<&str>) -> Option<&PasteBuffer> {
        match name {
            Some(name) => self.by_name.get(name),
            None => self
                .newest
                .iter()
                .filter_map(|name| self.by_name.get(name))
                .find(|buffer| buffer.automatic),
        }
    }

    pub fn remove(&mut self, name: &str) -> Option<PasteBuffer> {
        self.remove_existing(name)
    }

    pub fn list(&self) -> Vec<&PasteBuffer> {
        self.newest
            .iter()
            .filter_map(|name| self.by_name.get(name))
            .collect()
    }

    fn validate_data(&self, data: &[u8]) -> Result<(), PasteBufferError> {
        if data.len() > self.max_buffer_bytes {
            return Err(PasteBufferError::new(format!(
                "paste buffer exceeds {} bytes",
                self.max_buffer_bytes
            )));
        }
        Ok(())
    }

    fn insert(&mut self, name: String, data: Arc<[u8]>, automatic: bool) {
        let order = self.next_order;
        self.next_order = self.next_order.wrapping_add(1);
        self.bytes += data.len();
        self.automatic_count += usize::from(automatic);
        self.newest.push_front(name.clone());
        self.by_name.insert(
            name.clone(),
            PasteBuffer {
                name,
                data,
                automatic,
                order,
            },
        );
    }

    fn remove_existing(&mut self, name: &str) -> Option<PasteBuffer> {
        let removed = self.by_name.remove(name)?;
        self.newest.retain(|candidate| candidate != name);
        self.bytes -= removed.data.len();
        self.automatic_count -= usize::from(removed.automatic);
        Some(removed)
    }

    fn oldest_automatic_name(&self) -> Option<String> {
        self.newest.iter().rev().find_map(|name| {
            self.by_name
                .get(name)
                .filter(|buffer| buffer.automatic)
                .map(|buffer| buffer.name.clone())
        })
    }

    fn next_automatic_name(&mut self) -> String {
        loop {
            let name = format!("buffer{}", self.next_name);
            self.next_name = self.next_name.wrapping_add(1);
            if !self.by_name.contains_key(&name) {
                return name;
            }
        }
    }
}

impl Default for PasteBufferStore {
    fn default() -> Self {
        Self::new(50).expect("default paste buffer limit is valid")
    }
}

fn validate_name(name: &str) -> Result<(), PasteBufferError> {
    if name.is_empty() {
        return Err(PasteBufferError::new("paste buffer name is empty"));
    }
    if name.len() > MAX_PASTE_BUFFER_NAME_BYTES {
        return Err(PasteBufferError::new("paste buffer name exceeds 256 bytes"));
    }
    if name.chars().any(char::is_control) {
        return Err(PasteBufferError::new(
            "paste buffer name contains a control character",
        ));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::PasteBufferStore;

    #[test]
    fn preserves_binary_bytes_and_lists_newest_first() {
        let mut store = PasteBufferStore::with_limits(4, 16, 64);
        let first = store.add_automatic(vec![0, 0xff, b'\n']).unwrap().unwrap();
        let second = store.add_automatic(b"two".to_vec()).unwrap().unwrap();

        assert_eq!(first, "buffer0");
        assert_eq!(second, "buffer1");
        assert_eq!(store.get(Some(&first)).unwrap().data(), &[0, 0xff, b'\n']);
        assert_eq!(
            store
                .list()
                .iter()
                .map(|buffer| buffer.name())
                .collect::<Vec<_>>(),
            vec!["buffer1", "buffer0"]
        );
    }

    #[test]
    fn automatic_names_are_monotonic_and_empty_copies_are_ignored() {
        let mut store = PasteBufferStore::with_limits(1, 16, 64);
        assert_eq!(store.add_automatic(Vec::new()).unwrap(), None);
        assert_eq!(
            store.add_automatic(b"one".to_vec()).unwrap().as_deref(),
            Some("buffer0")
        );
        assert_eq!(
            store.add_automatic(b"two".to_vec()).unwrap().as_deref(),
            Some("buffer1")
        );
        assert!(store.get(Some("buffer0")).is_none());
        assert_eq!(store.get(None).unwrap().name(), "buffer1");
    }

    #[test]
    fn named_replacement_becomes_newest_and_is_never_automatically_evicted() {
        let mut store = PasteBufferStore::with_limits(1, 16, 64);
        store.set_named("keep", b"old".to_vec()).unwrap();
        store.add_automatic(b"one".to_vec()).unwrap();
        store.set_named("keep", b"new".to_vec()).unwrap();
        store.add_automatic(b"two".to_vec()).unwrap();

        assert_eq!(store.get(Some("keep")).unwrap().data(), b"new");
        assert!(!store.get(Some("keep")).unwrap().automatic());
        assert_eq!(
            store
                .list()
                .iter()
                .map(|buffer| buffer.name())
                .collect::<Vec<_>>(),
            vec!["buffer1", "keep"]
        );
    }

    #[test]
    fn shrinking_limit_evicts_only_oldest_automatic_buffers() {
        let mut store = PasteBufferStore::with_limits(3, 16, 64);
        store.set_named("named", b"n".to_vec()).unwrap();
        for value in [b"a", b"b", b"c"] {
            store.add_automatic(value.to_vec()).unwrap();
        }

        store.set_limit(1).unwrap();

        assert!(store.get(Some("named")).is_some());
        assert!(store.get(Some("buffer0")).is_none());
        assert!(store.get(Some("buffer1")).is_none());
        assert_eq!(store.get(None).unwrap().name(), "buffer2");
    }

    #[test]
    fn rejects_invalid_names_and_limits_without_partial_mutation() {
        let mut store = PasteBufferStore::with_limits(2, 4, 6);
        for name in ["", "bad\0name", "bad\nname"] {
            assert!(store.set_named(name, b"x".to_vec()).is_err());
        }
        assert!(store.set_limit(0).is_err());
        store.set_named("first", b"1234".to_vec()).unwrap();
        assert!(store.set_named("large", b"12345".to_vec()).is_err());
        assert!(store.set_named("second", b"789".to_vec()).is_err());
        assert_eq!(store.len(), 1);
        assert_eq!(store.bytes(), 4);
        assert_eq!(store.get(Some("first")).unwrap().data(), b"1234");
    }

    #[test]
    fn aggregate_check_accounts_for_named_replacement() {
        let mut store = PasteBufferStore::with_limits(2, 8, 8);
        store.set_named("a", b"12345".to_vec()).unwrap();
        store.set_named("b", b"678".to_vec()).unwrap();

        store.set_named("a", b"12".to_vec()).unwrap();
        assert_eq!(store.bytes(), 5);
        assert_eq!(store.get(Some("a")).unwrap().data(), b"12");
    }

    #[test]
    fn samples_escape_binary_data_deterministically() {
        let mut store = PasteBufferStore::with_limits(2, 256, 512);
        store
            .set_named("sample", b"a\n\t\\\0\xff".to_vec())
            .unwrap();

        assert_eq!(
            store.get(Some("sample")).unwrap().sample(200),
            "a\\n\\t\\\\\\0\\xff"
        );
    }

    #[test]
    fn remove_updates_bytes_and_default_lookup() {
        let mut store = PasteBufferStore::with_limits(2, 16, 64);
        store.set_named("named", b"abc".to_vec()).unwrap();
        store.add_automatic(b"xy".to_vec()).unwrap();

        assert_eq!(store.remove("buffer0").unwrap().data(), b"xy");
        assert_eq!(store.bytes(), 3);
        assert!(store.get(None).is_none());
        assert_eq!(store.remove("named").unwrap().data(), b"abc");
        assert!(store.is_empty());
    }
}
