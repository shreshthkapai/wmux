use std::{collections::BTreeMap, fmt, sync::OnceLock};

use crate::{parse_command_text, Client, ClientId, CommandList, ServerState};

const MODIFIER_SHIFT: u32 = 60;
const TAG_SHIFT: u32 = 32;
const VALUE_MASK: u64 = u32::MAX as u64;
const REPEAT_TIME_MS: u64 = 500;
const MAX_KEY_TABLES: usize = u16::MAX as usize + 1;
pub const MAX_KEY_NAME_BYTES: usize = 256;
pub const MAX_KEY_TABLE_NAME_BYTES: usize = 256;

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum BareKey {
    Char(char),
    Space,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    PageUp,
    PageDown,
    Backspace,
    Delete,
    Insert,
    Enter,
    Tab,
    BackTab,
    Escape,
    Function(u8),
}

#[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct KeyModifiers(u8);

impl KeyModifiers {
    pub const NONE: Self = Self(0);
    pub const SHIFT: Self = Self(1 << 0);
    pub const ALT: Self = Self(1 << 1);
    pub const CONTROL: Self = Self(1 << 2);
    pub const SUPER: Self = Self(1 << 3);
    pub const ALL: u8 = Self::SHIFT.0 | Self::ALT.0 | Self::CONTROL.0 | Self::SUPER.0;

    pub const fn from_bits(bits: u8) -> Option<Self> {
        if bits & !Self::ALL == 0 {
            Some(Self(bits))
        } else {
            None
        }
    }

    pub const fn bits(self) -> u8 {
        self.0
    }

    pub const fn contains(self, modifiers: Self) -> bool {
        self.0 & modifiers.0 == modifiers.0
    }
}

impl std::ops::BitOr for KeyModifiers {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl std::ops::BitOrAssign for KeyModifiers {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

#[derive(Clone, Copy, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct KeyCode(u64);

impl KeyCode {
    fn new(mut key: BareKey, mut modifiers: KeyModifiers) -> Self {
        if let BareKey::Char(character) = key {
            if character == ' ' {
                key = BareKey::Space;
            } else if character.is_ascii_uppercase() {
                key = BareKey::Char(character.to_ascii_lowercase());
                modifiers |= KeyModifiers::SHIFT;
            } else if character.is_ascii_punctuation() {
                // Terminal APIs may report both the produced symbol and the Shift used to type it.
                modifiers = KeyModifiers(modifiers.bits() & !KeyModifiers::SHIFT.bits());
            }
        }
        let (tag, value) = encode_bare_key(key);
        Self(
            (u64::from(modifiers.bits()) << MODIFIER_SHIFT)
                | (u64::from(tag) << TAG_SHIFT)
                | u64::from(value),
        )
    }

    pub fn character(character: char, modifiers: KeyModifiers) -> Self {
        Self::new(BareKey::Char(character), modifiers)
    }

    pub fn ctrl(character: char) -> Self {
        Self::character(character, KeyModifiers::CONTROL)
    }

    pub fn enter() -> Self {
        Self::new(BareKey::Enter, KeyModifiers::NONE)
    }

    pub fn escape() -> Self {
        Self::new(BareKey::Escape, KeyModifiers::NONE)
    }

    pub fn function(number: u8, modifiers: KeyModifiers) -> Result<Self, KeyParseError> {
        if !(1..=24).contains(&number) {
            return Err(KeyParseError::new(
                "function key must be between F1 and F24",
            ));
        }
        Ok(Self::new(BareKey::Function(number), modifiers))
    }

    pub fn try_new(key: BareKey, modifiers: KeyModifiers) -> Result<Self, KeyParseError> {
        if let BareKey::Function(number) = key {
            return Self::function(number, modifiers);
        }
        Ok(Self::new(key, modifiers))
    }

    pub fn parse(input: &str) -> Result<Self, KeyParseError> {
        if input.len() > MAX_KEY_NAME_BYTES {
            return Err(KeyParseError::new(format!(
                "key name exceeds {MAX_KEY_NAME_BYTES} bytes"
            )));
        }
        let mut modifiers = KeyModifiers::NONE;
        let mut key = input;
        loop {
            if let Some(rest) = key.strip_prefix("C-") {
                modifiers |= KeyModifiers::CONTROL;
                key = rest;
            } else if let Some(rest) = key.strip_prefix("M-") {
                modifiers |= KeyModifiers::ALT;
                key = rest;
            } else if let Some(rest) = key.strip_prefix("S-") {
                modifiers |= KeyModifiers::SHIFT;
                key = rest;
            } else if let Some(rest) = key.strip_prefix("Super-") {
                modifiers |= KeyModifiers::SUPER;
                key = rest;
            } else {
                break;
            }
        }
        if key.is_empty() {
            return Err(KeyParseError::new(format!("missing key in {input:?}")));
        }
        let bare = match key {
            "Space" => BareKey::Space,
            "Left" => BareKey::Left,
            "Right" => BareKey::Right,
            "Up" => BareKey::Up,
            "Down" => BareKey::Down,
            "Home" => BareKey::Home,
            "End" => BareKey::End,
            "PageUp" => BareKey::PageUp,
            "PageDown" => BareKey::PageDown,
            "BSpace" | "Backspace" => BareKey::Backspace,
            "DC" | "Delete" => BareKey::Delete,
            "IC" | "Insert" => BareKey::Insert,
            "Enter" => BareKey::Enter,
            "Tab" => BareKey::Tab,
            "BTab" => BareKey::BackTab,
            "Escape" | "Esc" => BareKey::Escape,
            key if key.starts_with('F') => {
                let number = key[1..]
                    .parse::<u8>()
                    .map_err(|_| KeyParseError::new(format!("unknown key: {input}")))?;
                return Self::function(number, modifiers);
            }
            key => {
                let mut characters = key.chars();
                let character = characters
                    .next()
                    .ok_or_else(|| KeyParseError::new(format!("unknown key: {input}")))?;
                if characters.next().is_some() {
                    return Err(KeyParseError::new(format!("unknown key: {input}")));
                }
                BareKey::Char(character)
            }
        };
        Ok(Self::new(bare, modifiers))
    }

    pub fn bare_key(self) -> BareKey {
        let tag = ((self.0 >> TAG_SHIFT) & 0xff) as u8;
        let value = (self.0 & VALUE_MASK) as u32;
        decode_bare_key(tag, value)
    }

    pub const fn modifiers(self) -> KeyModifiers {
        KeyModifiers(((self.0 >> MODIFIER_SHIFT) & 0x0f) as u8)
    }

    pub fn append_terminal_bytes(self, output: &mut Vec<u8>) {
        let modifiers = self.modifiers();
        let alt = modifiers.contains(KeyModifiers::ALT);
        let control = modifiers.contains(KeyModifiers::CONTROL);
        let shift = modifiers.contains(KeyModifiers::SHIFT);
        let special_modifiers = terminal_modifier_parameter(modifiers);

        match self.bare_key() {
            BareKey::Char(mut character) => {
                if alt {
                    output.push(0x1b);
                }
                if control {
                    if let Some(byte) = control_byte(character) {
                        output.push(byte);
                        return;
                    }
                }
                if shift && character.is_ascii_lowercase() {
                    character = character.to_ascii_uppercase();
                }
                let mut encoded = [0; 4];
                output.extend_from_slice(character.encode_utf8(&mut encoded).as_bytes());
            }
            BareKey::Space => {
                if alt {
                    output.push(0x1b);
                }
                output.push(if control { 0 } else { b' ' });
            }
            BareKey::Backspace => {
                if alt {
                    output.push(0x1b);
                }
                output.push(if control { 0x08 } else { 0x7f });
            }
            BareKey::Enter => append_simple_key(output, b'\r', alt),
            BareKey::Tab if shift => output.extend_from_slice(b"\x1b[Z"),
            BareKey::Tab => append_simple_key(output, b'\t', alt),
            BareKey::BackTab => output.extend_from_slice(b"\x1b[Z"),
            BareKey::Escape => output.push(0x1b),
            BareKey::Left => append_csi_final(output, b'D', special_modifiers),
            BareKey::Right => append_csi_final(output, b'C', special_modifiers),
            BareKey::Up => append_csi_final(output, b'A', special_modifiers),
            BareKey::Down => append_csi_final(output, b'B', special_modifiers),
            BareKey::Home => append_csi_final(output, b'H', special_modifiers),
            BareKey::End => append_csi_final(output, b'F', special_modifiers),
            BareKey::Insert => append_csi_tilde(output, 2, special_modifiers),
            BareKey::Delete => append_csi_tilde(output, 3, special_modifiers),
            BareKey::PageUp => append_csi_tilde(output, 5, special_modifiers),
            BareKey::PageDown => append_csi_tilde(output, 6, special_modifiers),
            BareKey::Function(number) => append_function_key(output, number, special_modifiers),
        }
    }
}

fn append_simple_key(output: &mut Vec<u8>, byte: u8, alt: bool) {
    if alt {
        output.push(0x1b);
    }
    output.push(byte);
}

fn control_byte(character: char) -> Option<u8> {
    if character.is_ascii_alphabetic() {
        return Some((character.to_ascii_lowercase() as u8) & 0x1f);
    }
    match character {
        ' ' | '@' => Some(0x00),
        '[' => Some(0x1b),
        '\\' => Some(0x1c),
        ']' => Some(0x1d),
        '^' => Some(0x1e),
        '_' => Some(0x1f),
        '?' => Some(0x7f),
        _ => None,
    }
}

fn terminal_modifier_parameter(modifiers: KeyModifiers) -> u8 {
    1 + u8::from(modifiers.contains(KeyModifiers::SHIFT))
        + 2 * u8::from(modifiers.contains(KeyModifiers::ALT))
        + 4 * u8::from(modifiers.contains(KeyModifiers::CONTROL))
        + 8 * u8::from(modifiers.contains(KeyModifiers::SUPER))
}

fn append_csi_final(output: &mut Vec<u8>, final_byte: u8, modifiers: u8) {
    output.extend_from_slice(b"\x1b[");
    if modifiers != 1 {
        output.extend_from_slice(b"1;");
        append_decimal(output, modifiers);
    }
    output.push(final_byte);
}

fn append_csi_tilde(output: &mut Vec<u8>, number: u8, modifiers: u8) {
    output.extend_from_slice(b"\x1b[");
    append_decimal(output, number);
    if modifiers != 1 {
        output.push(b';');
        append_decimal(output, modifiers);
    }
    output.push(b'~');
}

fn append_function_key(output: &mut Vec<u8>, number: u8, modifiers: u8) {
    let final_byte = match number {
        1 => Some(b'P'),
        2 => Some(b'Q'),
        3 => Some(b'R'),
        4 => Some(b'S'),
        _ => None,
    };
    if let Some(final_byte) = final_byte {
        if modifiers == 1 {
            output.extend_from_slice(b"\x1bO");
        } else {
            output.extend_from_slice(b"\x1b[1;");
            append_decimal(output, modifiers);
        }
        output.push(final_byte);
        return;
    }
    let number = match number {
        5 => 15,
        6 => 17,
        7 => 18,
        8 => 19,
        9 => 20,
        10 => 21,
        11 => 23,
        12 => 24,
        13 => 25,
        14 => 26,
        15 => 28,
        16 => 29,
        17 => 31,
        18 => 32,
        19 => 33,
        20 => 34,
        21 => 42,
        22 => 43,
        23 => 44,
        24 => 45,
        _ => unreachable!("validated function key range"),
    };
    append_csi_tilde(output, number, modifiers);
}

fn append_decimal(output: &mut Vec<u8>, value: u8) {
    if value >= 10 {
        output.push(b'0' + value / 10);
    }
    output.push(b'0' + value % 10);
}

impl fmt::Debug for KeyCode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "KeyCode({self})")
    }
}

impl fmt::Display for KeyCode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let modifiers = self.modifiers();
        if modifiers.contains(KeyModifiers::CONTROL) {
            formatter.write_str("C-")?;
        }
        if modifiers.contains(KeyModifiers::ALT) {
            formatter.write_str("M-")?;
        }
        if modifiers.contains(KeyModifiers::SHIFT) {
            formatter.write_str("S-")?;
        }
        if modifiers.contains(KeyModifiers::SUPER) {
            formatter.write_str("Super-")?;
        }
        match self.bare_key() {
            BareKey::Char(character) => write!(formatter, "{character}"),
            BareKey::Space => formatter.write_str("Space"),
            BareKey::Left => formatter.write_str("Left"),
            BareKey::Right => formatter.write_str("Right"),
            BareKey::Up => formatter.write_str("Up"),
            BareKey::Down => formatter.write_str("Down"),
            BareKey::Home => formatter.write_str("Home"),
            BareKey::End => formatter.write_str("End"),
            BareKey::PageUp => formatter.write_str("PageUp"),
            BareKey::PageDown => formatter.write_str("PageDown"),
            BareKey::Backspace => formatter.write_str("BSpace"),
            BareKey::Delete => formatter.write_str("DC"),
            BareKey::Insert => formatter.write_str("IC"),
            BareKey::Enter => formatter.write_str("Enter"),
            BareKey::Tab => formatter.write_str("Tab"),
            BareKey::BackTab => formatter.write_str("BTab"),
            BareKey::Escape => formatter.write_str("Escape"),
            BareKey::Function(number) => write!(formatter, "F{number}"),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyParseError {
    pub message: String,
}

impl KeyParseError {
    fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}

impl fmt::Display for KeyParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.message)
    }
}

impl std::error::Error for KeyParseError {}

fn encode_bare_key(key: BareKey) -> (u8, u32) {
    match key {
        BareKey::Char(character) => (0, character as u32),
        BareKey::Space => (1, 0),
        BareKey::Left => (2, 0),
        BareKey::Right => (3, 0),
        BareKey::Up => (4, 0),
        BareKey::Down => (5, 0),
        BareKey::Home => (6, 0),
        BareKey::End => (7, 0),
        BareKey::PageUp => (8, 0),
        BareKey::PageDown => (9, 0),
        BareKey::Backspace => (10, 0),
        BareKey::Delete => (11, 0),
        BareKey::Insert => (12, 0),
        BareKey::Enter => (13, 0),
        BareKey::Tab => (14, 0),
        BareKey::BackTab => (15, 0),
        BareKey::Escape => (16, 0),
        BareKey::Function(number) => (17, u32::from(number)),
    }
}

fn decode_bare_key(tag: u8, value: u32) -> BareKey {
    match tag {
        0 => BareKey::Char(char::from_u32(value).expect("packed character is valid")),
        1 => BareKey::Space,
        2 => BareKey::Left,
        3 => BareKey::Right,
        4 => BareKey::Up,
        5 => BareKey::Down,
        6 => BareKey::Home,
        7 => BareKey::End,
        8 => BareKey::PageUp,
        9 => BareKey::PageDown,
        10 => BareKey::Backspace,
        11 => BareKey::Delete,
        12 => BareKey::Insert,
        13 => BareKey::Enter,
        14 => BareKey::Tab,
        15 => BareKey::BackTab,
        16 => BareKey::Escape,
        17 => BareKey::Function(value as u8),
        _ => unreachable!("KeyCode has a private validated representation"),
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyEvent {
    pub code: KeyCode,
    pub raw: Vec<u8>,
}

impl KeyEvent {
    pub fn new(code: KeyCode, raw: Vec<u8>) -> Self {
        Self { code, raw }
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct KeyTableName(u16);

impl KeyTableName {
    pub const ROOT: Self = Self(0);
    pub const PREFIX: Self = Self(1);
    pub const COPY_MODE: Self = Self(2);

    pub const fn raw(self) -> u16 {
        self.0
    }
}

#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct KeyTableTarget(String);

impl KeyTableTarget {
    pub fn parse(name: impl Into<String>) -> Result<Self, KeyParseError> {
        let name = name.into();
        validate_table_name(&name)?;
        Ok(Self(name))
    }

    pub fn prefix() -> Self {
        Self("prefix".to_string())
    }

    pub fn root() -> Self {
        Self("root".to_string())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyBinding {
    pub key: KeyCode,
    pub repeatable: bool,
    pub commands: CommandList,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyTable {
    name: KeyTableName,
    bindings: Vec<KeyBinding>,
}

impl KeyTable {
    pub fn new(name: KeyTableName) -> Self {
        Self {
            name,
            bindings: Vec::new(),
        }
    }

    pub const fn name(&self) -> KeyTableName {
        self.name
    }

    pub fn bindings(&self) -> &[KeyBinding] {
        &self.bindings
    }

    pub fn get(&self, key: KeyCode) -> Option<&KeyBinding> {
        self.bindings
            .binary_search_by_key(&key, |binding| binding.key)
            .ok()
            .map(|index| &self.bindings[index])
    }

    pub fn bind(&mut self, binding: KeyBinding) {
        match self
            .bindings
            .binary_search_by_key(&binding.key, |candidate| candidate.key)
        {
            Ok(index) => self.bindings[index] = binding,
            Err(index) => self.bindings.insert(index, binding),
        }
    }

    pub fn unbind(&mut self, key: KeyCode) -> bool {
        let Ok(index) = self
            .bindings
            .binary_search_by_key(&key, |binding| binding.key)
        else {
            return false;
        };
        self.bindings.remove(index);
        true
    }

    pub fn clear(&mut self) {
        self.bindings.clear();
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct KeyTables {
    names: BTreeMap<String, KeyTableName>,
    tables: Vec<KeyTable>,
    prefix: KeyCode,
    repeat_time_ms: u64,
}

impl KeyTables {
    pub fn tmux_defaults() -> Self {
        static DEFAULTS: OnceLock<KeyTables> = OnceLock::new();
        DEFAULTS.get_or_init(build_tmux_defaults).clone()
    }

    pub fn table(&self, name: KeyTableName) -> Option<&KeyTable> {
        self.tables.get(usize::from(name.raw()))
    }

    pub fn table_mut(&mut self, name: KeyTableName) -> Option<&mut KeyTable> {
        self.tables.get_mut(usize::from(name.raw()))
    }

    pub fn named(&self, name: &str) -> Option<KeyTableName> {
        self.names.get(name).copied()
    }

    pub fn ensure_named(&mut self, name: &str) -> Result<KeyTableName, KeyParseError> {
        if let Some(table) = self.named(name) {
            return Ok(table);
        }
        validate_table_name(name)?;
        if self.tables.len() == MAX_KEY_TABLES {
            return Err(KeyParseError::new("too many key tables"));
        }
        let id = KeyTableName(self.tables.len() as u16);
        self.names.insert(name.to_string(), id);
        self.tables.push(KeyTable::new(id));
        Ok(id)
    }

    pub fn tables(&self) -> impl Iterator<Item = &KeyTable> {
        self.tables.iter()
    }

    pub fn name(&self, table: KeyTableName) -> Option<&str> {
        self.names
            .iter()
            .find_map(|(name, id)| (*id == table).then_some(name.as_str()))
    }

    pub const fn prefix(&self) -> KeyCode {
        self.prefix
    }

    pub const fn repeat_time_ms(&self) -> u64 {
        self.repeat_time_ms
    }
}

fn validate_table_name(name: &str) -> Result<(), KeyParseError> {
    if name.is_empty() || name.contains('\0') {
        return Err(KeyParseError::new("invalid key table name"));
    }
    if name.len() > MAX_KEY_TABLE_NAME_BYTES {
        return Err(KeyParseError::new(format!(
            "key table name exceeds {MAX_KEY_TABLE_NAME_BYTES} bytes"
        )));
    }
    Ok(())
}

fn build_tmux_defaults() -> KeyTables {
    let mut tables = KeyTables {
        names: BTreeMap::from([
            ("root".to_string(), KeyTableName::ROOT),
            ("prefix".to_string(), KeyTableName::PREFIX),
            ("copy-mode".to_string(), KeyTableName::COPY_MODE),
        ]),
        tables: vec![
            KeyTable::new(KeyTableName::ROOT),
            KeyTable::new(KeyTableName::PREFIX),
            KeyTable::new(KeyTableName::COPY_MODE),
        ],
        prefix: KeyCode::ctrl('b'),
        repeat_time_ms: REPEAT_TIME_MS,
    };

    for (key, command) in [
        ("%", "split-window -h"),
        ("\"", "split-window"),
        ("c", "new-window"),
        ("n", "next-window"),
        ("p", "previous-window"),
        ("l", "last-window"),
        ("z", "resize-pane -Z"),
        ("[", "copy-mode"),
        ("x", "confirm-before -p 'kill-pane? (y/n)' kill-pane"),
        ("&", "confirm-before -p 'kill-window? (y/n)' kill-window"),
        ("o", "select-pane -D"),
        (";", "last-pane"),
        ("Up", "select-pane -U"),
        ("Down", "select-pane -D"),
        ("Left", "select-pane -L"),
        ("Right", "select-pane -R"),
    ] {
        bind_default(&mut tables, key, command);
    }
    for index in 0..=9 {
        bind_default(
            &mut tables,
            &index.to_string(),
            &format!("select-window -t {index}"),
        );
    }
    tables
}

fn bind_default(tables: &mut KeyTables, key: &str, command: &str) {
    let binding = KeyBinding {
        key: KeyCode::parse(key).expect("default key is valid"),
        repeatable: false,
        commands: parse_command_text(command).expect("default binding command is valid"),
    };
    tables
        .table_mut(KeyTableName::PREFIX)
        .expect("prefix table exists")
        .bind(binding);
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ConfirmationState {
    pub prompt: String,
    pub commands: CommandList,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum InputMode {
    Normal,
    CopyMode,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum InputRoute {
    PaneBytes(Vec<u8>),
    Commands(CommandList),
    CopyModeKey(KeyEvent),
    Consumed,
}

pub fn route_key(
    state: &mut ServerState,
    client: ClientId,
    mode: InputMode,
    event: KeyEvent,
    now_ms: u64,
) -> InputRoute {
    let tables = &state.key_tables;
    let Some(client) = state.clients.get_mut(&client) else {
        return InputRoute::Consumed;
    };

    if client.confirmation.is_some() {
        return route_confirmation(client, event);
    }

    if mode == InputMode::CopyMode {
        return binding_route(tables, KeyTableName::COPY_MODE, event.code)
            .map_or(InputRoute::CopyModeKey(event), |(_, commands)| {
                InputRoute::Commands(commands)
            });
    }

    expire_key_state(client, now_ms);
    if client.key_table == KeyTableName::ROOT {
        return route_root(tables, client, event, now_ms);
    }

    if client.key_table == KeyTableName::PREFIX && event.code == tables.prefix() {
        reset_key_state(client);
        return InputRoute::PaneBytes(event.raw);
    }

    let repeat_active = client.repeat_deadline_ms.is_some();
    if let Some((repeatable, commands)) = binding_route(tables, client.key_table, event.code) {
        if !repeat_active || repeatable {
            if repeatable {
                client.prefix_deadline_ms = None;
                client.repeat_deadline_ms = Some(now_ms.saturating_add(tables.repeat_time_ms()));
                client.last_repeatable_key = Some(event.code);
            } else {
                reset_key_state(client);
            }
            return InputRoute::Commands(commands);
        }
    }

    reset_key_state(client);
    route_root(tables, client, event, now_ms)
}

fn route_confirmation(client: &mut Client, event: KeyEvent) -> InputRoute {
    if event.code.modifiers().bits() & !KeyModifiers::SHIFT.bits() != 0 {
        return InputRoute::Consumed;
    }
    match event.code.bare_key() {
        BareKey::Char('y') | BareKey::Enter => {
            let confirmation = client
                .confirmation
                .take()
                .expect("confirmation was checked above");
            InputRoute::Commands(confirmation.commands)
        }
        BareKey::Char('n') | BareKey::Escape => {
            client.confirmation = None;
            InputRoute::Consumed
        }
        _ => InputRoute::Consumed,
    }
}

fn route_root(tables: &KeyTables, client: &mut Client, event: KeyEvent, now_ms: u64) -> InputRoute {
    if event.code == tables.prefix() {
        client.key_table = KeyTableName::PREFIX;
        client.prefix_deadline_ms = Some(now_ms.saturating_add(tables.repeat_time_ms()));
        client.repeat_deadline_ms = None;
        client.last_repeatable_key = None;
        return InputRoute::Consumed;
    }
    binding_route(tables, KeyTableName::ROOT, event.code)
        .map_or(InputRoute::PaneBytes(event.raw), |(_, commands)| {
            InputRoute::Commands(commands)
        })
}

fn binding_route(
    tables: &KeyTables,
    table: KeyTableName,
    key: KeyCode,
) -> Option<(bool, CommandList)> {
    let binding = tables.table(table)?.get(key)?;
    Some((binding.repeatable, binding.commands.clone()))
}

fn expire_key_state(client: &mut Client, now_ms: u64) {
    let expired = client
        .repeat_deadline_ms
        .or(client.prefix_deadline_ms)
        .is_some_and(|deadline| now_ms > deadline);
    if expired {
        reset_key_state(client);
    }
}

fn reset_key_state(client: &mut Client) {
    client.key_table = KeyTableName::ROOT;
    client.prefix_deadline_ms = None;
    client.repeat_deadline_ms = None;
    client.last_repeatable_key = None;
}

#[cfg(test)]
mod tests {
    use std::mem::size_of;

    use super::{
        route_key, ConfirmationState, InputMode, InputRoute, KeyBinding, KeyCode, KeyEvent,
        KeyModifiers, KeyTable, KeyTableName, KeyTableTarget, KeyTables, MAX_KEY_NAME_BYTES,
        MAX_KEY_TABLE_NAME_BYTES,
    };
    use crate::{parse_command_text, ClientInput, Command, ServerState, SplitDirection};

    fn binding(key: KeyCode, repeatable: bool, command: &str) -> KeyBinding {
        KeyBinding {
            key,
            repeatable,
            commands: parse_command_text(command).unwrap(),
        }
    }

    fn character(character: char) -> KeyEvent {
        KeyEvent::new(
            KeyCode::character(character, KeyModifiers::NONE),
            character.to_string().into_bytes(),
        )
    }

    #[test]
    fn uppercase_characters_normalize_to_lowercase_plus_shift() {
        assert_eq!(size_of::<KeyCode>(), 8);
        assert_eq!(
            KeyCode::character('A', KeyModifiers::NONE),
            KeyCode::character('a', KeyModifiers::SHIFT)
        );
        assert_ne!(
            KeyCode::character('a', KeyModifiers::SHIFT),
            KeyCode::character('a', KeyModifiers::NONE)
        );
        assert_ne!(
            KeyCode::try_new(super::BareKey::Left, KeyModifiers::SHIFT).unwrap(),
            KeyCode::try_new(super::BareKey::Left, KeyModifiers::NONE).unwrap()
        );
        assert_eq!(KeyCode::parse("C-S-Left").unwrap().to_string(), "C-S-Left");
        assert_eq!(KeyCode::parse("F24").unwrap().to_string(), "F24");
        assert!(KeyCode::parse("F25").is_err());
        assert!(KeyCode::parse(&"x".repeat(MAX_KEY_NAME_BYTES + 1)).is_err());
    }

    #[test]
    fn shifted_printable_symbols_route_to_tmux_default_split_bindings() {
        for (symbol, direction) in [
            ('%', SplitDirection::LeftRight),
            ('"', SplitDirection::TopBottom),
        ] {
            let mut state = ServerState::new();
            let client = state.add_client();
            assert_eq!(
                route_key(
                    &mut state,
                    client,
                    InputMode::Normal,
                    KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
                    100,
                ),
                InputRoute::Consumed
            );

            let route = route_key(
                &mut state,
                client,
                InputMode::Normal,
                KeyEvent::new(
                    KeyCode::character(symbol, KeyModifiers::SHIFT),
                    symbol.to_string().into_bytes(),
                ),
                101,
            );
            assert!(
                matches!(
                    route,
                    InputRoute::Commands(commands)
                        if matches!(
                            &commands[0],
                            Command::SplitWindow {
                                direction: actual,
                                detached: false,
                            } if *actual == direction
                        )
                ),
                "shifted {symbol:?} did not route to the expected split binding"
            );
        }
    }

    #[test]
    fn table_lookup_is_sorted_and_bind_replaces_without_duplicates() {
        let mut table = KeyTable::new(KeyTableName::ROOT);
        table.bind(binding(
            KeyCode::character('z', KeyModifiers::NONE),
            false,
            "list-sessions",
        ));
        table.bind(binding(
            KeyCode::character('a', KeyModifiers::NONE),
            false,
            "list-clients",
        ));
        table.bind(binding(
            KeyCode::character('m', KeyModifiers::NONE),
            false,
            "list-windows",
        ));
        let replacement = binding(
            KeyCode::character('m', KeyModifiers::NONE),
            true,
            "list-panes",
        );
        table.bind(replacement.clone());

        assert_eq!(
            table
                .bindings()
                .iter()
                .map(|binding| binding.key)
                .collect::<Vec<_>>(),
            [
                KeyCode::character('a', KeyModifiers::NONE),
                KeyCode::character('m', KeyModifiers::NONE),
                KeyCode::character('z', KeyModifiers::NONE),
            ]
        );
        assert_eq!(
            table.get(KeyCode::character('m', KeyModifiers::NONE)),
            Some(&replacement)
        );
        assert!(table.unbind(KeyCode::character('m', KeyModifiers::NONE)));
        assert!(!table.unbind(KeyCode::character('m', KeyModifiers::NONE)));
        table.clear();
        assert!(table.bindings().is_empty());

        let mut tables = KeyTables::tmux_defaults();
        let custom = tables.ensure_named("custom").unwrap();
        assert_eq!(tables.ensure_named("custom").unwrap(), custom);
        assert_eq!(tables.table(custom).unwrap().name(), custom);
        assert!(tables
            .ensure_named(&"x".repeat(MAX_KEY_TABLE_NAME_BYTES + 1))
            .is_err());
    }

    #[test]
    fn binding_table_targets_are_bounded_without_changing_compact_runtime_ids() {
        assert_eq!(KeyTableTarget::parse("prefix").unwrap().as_str(), "prefix");
        assert!(KeyTableTarget::parse("").is_err());
        assert!(KeyTableTarget::parse("x".repeat(MAX_KEY_TABLE_NAME_BYTES + 1)).is_err());

        let mut tables = KeyTables::tmux_defaults();
        let custom = tables.ensure_named("custom").unwrap();
        assert_eq!(tables.name(custom), Some("custom"));
        assert_eq!(tables.tables().last().unwrap().name(), custom);
        assert_eq!(size_of::<KeyTableName>(), 2);
    }

    #[test]
    fn terminal_key_encoding_is_static_bounded_and_modifier_aware() {
        for (key, expected) in [
            ("C-a", b"\x01".as_slice()),
            ("S-a", b"A".as_slice()),
            ("M-x", b"\x1bx".as_slice()),
            ("Left", b"\x1b[D".as_slice()),
            ("C-Left", b"\x1b[1;5D".as_slice()),
            ("F12", b"\x1b[24~".as_slice()),
            ("S-F2", b"\x1b[1;2Q".as_slice()),
        ] {
            let mut encoded = Vec::new();
            KeyCode::parse(key)
                .unwrap()
                .append_terminal_bytes(&mut encoded);
            assert_eq!(encoded, expected, "wrong encoding for {key}");
        }
    }

    #[test]
    fn destructive_defaults_are_parsed_confirmation_commands() {
        let tables = KeyTables::tmux_defaults();
        for key in ["x", "&"] {
            let binding = tables
                .table(KeyTableName::PREFIX)
                .unwrap()
                .get(KeyCode::parse(key).unwrap())
                .unwrap();
            assert!(matches!(
                &binding.commands[0],
                Command::ConfirmBefore { commands, .. }
                    if matches!(&commands[0], Command::KillPane | Command::KillWindow)
            ));
        }
    }

    #[test]
    fn arbitrary_bounded_key_bytes_never_panic_or_emit_unbounded_errors() {
        let mut seed = 0x6b65_792d_7068_6173_u64;
        for len in 0..=4_096 {
            let mut bytes = Vec::with_capacity(len);
            for _ in 0..len {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                bytes.push(seed as u8);
            }
            let key = String::from_utf8_lossy(&bytes).into_owned();
            let result = std::panic::catch_unwind(|| KeyCode::parse(&key));
            let parsed = result.unwrap_or_else(|_| panic!("key parser panicked for length {len}"));
            if let Err(error) = parsed {
                assert!(error.to_string().len() <= 4 * 1024);
            }
        }
    }

    #[test]
    fn prefix_state_is_per_client_and_unbound_bytes_move_unchanged() {
        let mut state = ServerState::new();
        let first = state.add_client();
        let second = state.add_client();
        let prefix = KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]);

        assert_eq!(
            route_key(&mut state, first, InputMode::Normal, prefix, 100),
            InputRoute::Consumed
        );
        assert_eq!(state.clients[&first].key_table, KeyTableName::PREFIX);
        assert_eq!(state.clients[&second].key_table, KeyTableName::ROOT);

        let mut raw = Vec::with_capacity(16);
        raw.extend_from_slice(&[0xf0, 0x9f, 0x99, 0x82]);
        let pointer = raw.as_ptr();
        let route = route_key(
            &mut state,
            second,
            InputMode::Normal,
            KeyEvent::new(KeyCode::character('\u{1f642}', KeyModifiers::NONE), raw),
            100,
        );
        assert!(matches!(
            route,
            InputRoute::PaneBytes(bytes)
                if bytes == [0xf0, 0x9f, 0x99, 0x82] && bytes.as_ptr() == pointer
        ));
    }

    #[test]
    fn repeatable_binding_keeps_prefix_and_nonrepeatable_key_retries_root() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let repeat = KeyCode::character('r', KeyModifiers::NONE);
        let nonrepeat = KeyCode::character('a', KeyModifiers::NONE);
        let root_commands = parse_command_text("list-windows").unwrap();
        state
            .key_tables
            .table_mut(KeyTableName::PREFIX)
            .unwrap()
            .bind(binding(repeat, true, "list-sessions"));
        state
            .key_tables
            .table_mut(KeyTableName::PREFIX)
            .unwrap()
            .bind(binding(nonrepeat, false, "list-clients"));
        state
            .key_tables
            .table_mut(KeyTableName::ROOT)
            .unwrap()
            .bind(KeyBinding {
                key: nonrepeat,
                repeatable: false,
                commands: root_commands.clone(),
            });

        route_key(
            &mut state,
            client,
            InputMode::Normal,
            KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
            100,
        );
        assert!(matches!(
            route_key(&mut state, client, InputMode::Normal, character('r'), 110),
            InputRoute::Commands(_)
        ));
        assert_eq!(state.clients[&client].repeat_deadline_ms, Some(610));
        assert_eq!(state.clients[&client].last_repeatable_key, Some(repeat));
        assert_eq!(
            route_key(&mut state, client, InputMode::Normal, character('a'), 120),
            InputRoute::Commands(root_commands)
        );
        assert_eq!(state.clients[&client].key_table, KeyTableName::ROOT);
        assert_eq!(state.clients[&client].repeat_deadline_ms, None);
    }

    #[test]
    fn repeat_deadline_expiry_retries_the_key_from_root() {
        let mut state = ServerState::new();
        let client = state.add_client();
        state
            .key_tables
            .table_mut(KeyTableName::PREFIX)
            .unwrap()
            .bind(binding(
                KeyCode::character('r', KeyModifiers::NONE),
                true,
                "list-sessions",
            ));
        route_key(
            &mut state,
            client,
            InputMode::Normal,
            KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
            0,
        );
        route_key(&mut state, client, InputMode::Normal, character('r'), 1);

        assert_eq!(
            route_key(&mut state, client, InputMode::Normal, character('r'), 502),
            InputRoute::PaneBytes(b"r".to_vec())
        );
        assert_eq!(state.clients[&client].key_table, KeyTableName::ROOT);
    }

    #[test]
    fn expired_prefix_retries_root_and_root_bindings_precede_passthrough() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let key = KeyCode::character('a', KeyModifiers::NONE);
        state
            .key_tables
            .table_mut(KeyTableName::ROOT)
            .unwrap()
            .bind(binding(key, false, "list-sessions"));
        route_key(
            &mut state,
            client,
            InputMode::Normal,
            KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
            0,
        );

        assert!(matches!(
            route_key(&mut state, client, InputMode::Normal, character('a'), 501),
            InputRoute::Commands(_)
        ));
        assert_eq!(state.clients[&client].key_table, KeyTableName::ROOT);
    }

    #[test]
    fn copy_table_precedes_copy_handler_and_missing_binding_moves_the_event() {
        let mut state = ServerState::new();
        let client = state.add_client();
        state
            .key_tables
            .table_mut(KeyTableName::COPY_MODE)
            .unwrap()
            .bind(binding(
                KeyCode::character('g', KeyModifiers::NONE),
                false,
                "list-sessions",
            ));
        assert!(matches!(
            route_key(&mut state, client, InputMode::CopyMode, character('g'), 0),
            InputRoute::Commands(_)
        ));

        let mut raw = Vec::with_capacity(8);
        raw.push(b'q');
        let pointer = raw.as_ptr();
        assert!(matches!(
            route_key(
                &mut state,
                client,
                InputMode::CopyMode,
                KeyEvent::new(KeyCode::character('q', KeyModifiers::NONE), raw),
                0,
            ),
            InputRoute::CopyModeKey(event)
                if event.raw == b"q" && event.raw.as_ptr() == pointer
        ));
    }

    #[test]
    fn prefix_inside_prefix_sends_one_literal_prefix() {
        let mut state = ServerState::new();
        let client = state.add_client();
        route_key(
            &mut state,
            client,
            InputMode::Normal,
            KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
            0,
        );
        assert_eq!(
            route_key(
                &mut state,
                client,
                InputMode::Normal,
                KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
                1,
            ),
            InputRoute::PaneBytes(vec![0x02])
        );
        assert_eq!(state.clients[&client].key_table, KeyTableName::ROOT);
    }

    #[test]
    fn confirmation_consumes_input_and_only_affirmative_keys_release_commands() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let commands = parse_command_text("kill-pane").unwrap();
        state.clients.get_mut(&client).unwrap().confirmation = Some(ConfirmationState {
            prompt: "kill-pane?".to_string(),
            commands: commands.clone(),
        });
        assert_eq!(
            route_key(&mut state, client, InputMode::Normal, character('x'), 0),
            InputRoute::Consumed
        );
        assert!(state.clients[&client].confirmation.is_some());
        assert_eq!(
            route_key(&mut state, client, InputMode::Normal, character('y'), 1),
            InputRoute::Commands(commands.clone())
        );
        assert!(state.clients[&client].confirmation.is_none());

        for event in [character('n'), KeyEvent::new(KeyCode::escape(), vec![0x1b])] {
            state.clients.get_mut(&client).unwrap().confirmation = Some(ConfirmationState {
                prompt: "kill-pane?".to_string(),
                commands: commands.clone(),
            });
            assert_eq!(
                route_key(&mut state, client, InputMode::Normal, event, 2),
                InputRoute::Consumed
            );
            assert!(state.clients[&client].confirmation.is_none());
        }
        state.clients.get_mut(&client).unwrap().confirmation = Some(ConfirmationState {
            prompt: "kill-pane?".to_string(),
            commands: commands.clone(),
        });
        assert_eq!(
            route_key(
                &mut state,
                client,
                InputMode::Normal,
                KeyEvent::new(KeyCode::enter(), vec![b'\r']),
                3,
            ),
            InputRoute::Commands(commands)
        );
    }

    #[test]
    fn paste_is_a_distinct_semantic_input_and_client_removal_drops_key_state() {
        assert!(matches!(
            ClientInput::Paste(b"paste".to_vec()),
            ClientInput::Paste(_)
        ));
        let mut state = ServerState::new();
        let client = state.add_client();
        route_key(
            &mut state,
            client,
            InputMode::Normal,
            KeyEvent::new(KeyCode::ctrl('b'), vec![0x02]),
            0,
        );
        state.clients.get_mut(&client).unwrap().confirmation = Some(ConfirmationState {
            prompt: "test?".to_string(),
            commands: parse_command_text("list-sessions").unwrap(),
        });
        state.remove_client(client);
        assert!(!state.clients.contains_key(&client));
        assert_eq!(
            route_key(&mut state, client, InputMode::Normal, character('a'), 1),
            InputRoute::Consumed
        );
    }
}
