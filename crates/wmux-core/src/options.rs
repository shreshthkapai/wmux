use std::{collections::BTreeMap, fmt};

use crate::{ClientId, PaneId, SessionId, WindowId};

pub const MAX_OPTION_NAME_BYTES: usize = 256;
pub const MAX_OPTION_STRING_BYTES: usize = 64 * 1024;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum OptionScope {
    Server,
    Session,
    Window,
    Pane,
    Client,
}

impl fmt::Display for OptionScope {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Server => "server",
            Self::Session => "session",
            Self::Window => "window",
            Self::Pane => "pane",
            Self::Client => "client",
        })
    }
}

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum OptionTarget {
    Server,
    Session(SessionId),
    Window(WindowId),
    Pane(PaneId),
    Client(ClientId),
}

impl OptionTarget {
    pub const fn scope(self) -> OptionScope {
        match self {
            Self::Server => OptionScope::Server,
            Self::Session(_) => OptionScope::Session,
            Self::Window(_) => OptionScope::Window,
            Self::Pane(_) => OptionScope::Pane,
            Self::Client(_) => OptionScope::Client,
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum OptionValue {
    Flag(bool),
    Number(i64),
    String(String),
}

impl fmt::Display for OptionValue {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Flag(value) => formatter.write_str(if *value { "on" } else { "off" }),
            Self::Number(value) => write!(formatter, "{value}"),
            Self::String(value) => formatter.write_str(value),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct OptionError(String);

impl OptionError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for OptionError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for OptionError {}

#[derive(Clone, Copy)]
enum OptionKind {
    Flag { default: bool },
    Number { default: i64, min: i64, max: i64 },
}

struct OptionDefinition {
    name: &'static str,
    scopes: &'static [OptionScope],
    kind: OptionKind,
}

const ALL_INHERITED: &[OptionScope] = &[
    OptionScope::Server,
    OptionScope::Session,
    OptionScope::Window,
    OptionScope::Pane,
];
const SERVER_ONLY: &[OptionScope] = &[OptionScope::Server];
const SERVER_SESSION_CLIENT: &[OptionScope] = &[
    OptionScope::Server,
    OptionScope::Session,
    OptionScope::Client,
];

const DEFINITIONS: &[OptionDefinition] = &[
    OptionDefinition {
        name: "buffer-limit",
        scopes: SERVER_ONLY,
        kind: OptionKind::Number {
            default: 50,
            min: 1,
            max: 1_000,
        },
    },
    OptionDefinition {
        name: "exit-empty",
        scopes: SERVER_ONLY,
        kind: OptionKind::Flag { default: false },
    },
    OptionDefinition {
        name: "history-limit",
        scopes: ALL_INHERITED,
        kind: OptionKind::Number {
            default: 10_000,
            min: 0,
            max: 10_000_000,
        },
    },
    OptionDefinition {
        name: "remain-on-exit",
        scopes: ALL_INHERITED,
        kind: OptionKind::Flag { default: false },
    },
    OptionDefinition {
        name: "repeat-time",
        scopes: SERVER_SESSION_CLIENT,
        kind: OptionKind::Number {
            default: 500,
            min: 0,
            max: 60_000,
        },
    },
    OptionDefinition {
        name: "set-clipboard",
        scopes: SERVER_SESSION_CLIENT,
        kind: OptionKind::Flag { default: true },
    },
];

#[derive(Debug, Default)]
pub struct OptionStore {
    values: BTreeMap<OptionTarget, BTreeMap<String, OptionValue>>,
}

impl OptionStore {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn set(
        &mut self,
        target: OptionTarget,
        name: &str,
        raw: &str,
    ) -> Result<OptionValue, OptionError> {
        validate_name(name)?;
        let value = if name.starts_with('@') {
            if name.len() == 1 {
                return Err(OptionError::new("user option name is empty"));
            }
            if raw.len() > MAX_OPTION_STRING_BYTES {
                return Err(OptionError::new("option value exceeds 65536 bytes"));
            }
            OptionValue::String(raw.to_string())
        } else {
            let definition = definition(name)
                .ok_or_else(|| OptionError::new(format!("unknown option: {name}")))?;
            if !definition.scopes.contains(&target.scope()) {
                return Err(OptionError::new(format!(
                    "option {name} cannot be set at {} scope",
                    target.scope()
                )));
            }
            parse_value(definition, raw)?
        };
        self.values
            .entry(target)
            .or_default()
            .insert(name.to_string(), value.clone());
        Ok(value)
    }

    pub fn unset(&mut self, target: OptionTarget, name: &str) -> Result<bool, OptionError> {
        validate_known_name(name)?;
        let removed = self
            .values
            .get_mut(&target)
            .and_then(|values| values.remove(name))
            .is_some();
        if self.values.get(&target).is_some_and(BTreeMap::is_empty) {
            self.values.remove(&target);
        }
        Ok(removed)
    }

    pub fn get(
        &self,
        inheritance_path: &[OptionTarget],
        name: &str,
    ) -> Result<OptionValue, OptionError> {
        validate_known_name(name)?;
        for target in inheritance_path {
            if let Some(value) = self.values.get(target).and_then(|values| values.get(name)) {
                return Ok(value.clone());
            }
        }
        if let Some(definition) = definition(name) {
            return Ok(default_value(definition));
        }
        Err(OptionError::new(format!("option not set: {name}")))
    }

    pub fn list_local(&self, target: OptionTarget) -> Vec<(String, OptionValue)> {
        self.values
            .get(&target)
            .map(|values| {
                values
                    .iter()
                    .map(|(name, value)| (name.clone(), value.clone()))
                    .collect()
            })
            .unwrap_or_default()
    }

    pub fn list_effective(&self, inheritance_path: &[OptionTarget]) -> Vec<(String, OptionValue)> {
        let mut names = DEFINITIONS
            .iter()
            .map(|definition| definition.name.to_string())
            .collect::<Vec<_>>();
        for target in inheritance_path.iter().rev() {
            if let Some(values) = self.values.get(target) {
                for name in values.keys().filter(|name| name.starts_with('@')) {
                    if !names.contains(name) {
                        names.push(name.clone());
                    }
                }
            }
        }
        names.sort();
        names
            .into_iter()
            .filter_map(|name| {
                self.get(inheritance_path, &name)
                    .ok()
                    .map(|value| (name, value))
            })
            .collect()
    }

    pub fn remove_target(&mut self, target: OptionTarget) {
        self.values.remove(&target);
    }
}

fn validate_name(name: &str) -> Result<(), OptionError> {
    if name.is_empty() {
        return Err(OptionError::new("option name is empty"));
    }
    if name.len() > MAX_OPTION_NAME_BYTES {
        return Err(OptionError::new("option name exceeds 256 bytes"));
    }
    if name.contains('\0') {
        return Err(OptionError::new("option name contains NUL"));
    }
    Ok(())
}

fn validate_known_name(name: &str) -> Result<(), OptionError> {
    validate_name(name)?;
    if name.starts_with('@') || definition(name).is_some() {
        Ok(())
    } else {
        Err(OptionError::new(format!("unknown option: {name}")))
    }
}

fn definition(name: &str) -> Option<&'static OptionDefinition> {
    DEFINITIONS
        .binary_search_by_key(&name, |definition| definition.name)
        .ok()
        .map(|index| &DEFINITIONS[index])
}

fn parse_value(definition: &OptionDefinition, raw: &str) -> Result<OptionValue, OptionError> {
    match definition.kind {
        OptionKind::Flag { .. } => match raw {
            "1" | "on" | "true" | "yes" => Ok(OptionValue::Flag(true)),
            "0" | "off" | "false" | "no" => Ok(OptionValue::Flag(false)),
            _ => Err(OptionError::new(format!(
                "option {} expects on or off",
                definition.name
            ))),
        },
        OptionKind::Number { min, max, .. } => {
            let value = raw.parse::<i64>().map_err(|_| {
                OptionError::new(format!("option {} expects a number", definition.name))
            })?;
            if !(min..=max).contains(&value) {
                return Err(OptionError::new(format!(
                    "option {} must be between {min} and {max}",
                    definition.name
                )));
            }
            Ok(OptionValue::Number(value))
        }
    }
}

fn default_value(definition: &OptionDefinition) -> OptionValue {
    match definition.kind {
        OptionKind::Flag { default } => OptionValue::Flag(default),
        OptionKind::Number { default, .. } => OptionValue::Number(default),
    }
}

#[cfg(test)]
mod tests {
    use super::{OptionScope, OptionStore, OptionTarget, OptionValue};
    use crate::{PaneId, ServerState, SessionId, WindowId};

    #[test]
    fn more_specific_value_wins_and_unset_reveals_parent() {
        let mut options = OptionStore::new();
        let session = OptionTarget::Session(SessionId::new(1));
        let window = OptionTarget::Window(WindowId::new(2));
        let pane = OptionTarget::Pane(PaneId::new(3));
        let path = [pane, window, session, OptionTarget::Server];

        options
            .set(OptionTarget::Server, "history-limit", "2000")
            .unwrap();
        options.set(session, "history-limit", "3000").unwrap();
        options.set(pane, "history-limit", "4000").unwrap();

        assert_eq!(
            options.get(&path, "history-limit").unwrap(),
            OptionValue::Number(4000)
        );
        options.unset(pane, "history-limit").unwrap();
        assert_eq!(
            options.get(&path, "history-limit").unwrap(),
            OptionValue::Number(3000)
        );
    }

    #[test]
    fn definitions_enforce_scope_type_and_bounds() {
        let mut options = OptionStore::new();
        let pane = OptionTarget::Pane(PaneId::new(1));

        assert_eq!(
            options
                .get(&[OptionTarget::Server], "buffer-limit")
                .unwrap(),
            OptionValue::Number(50)
        );
        assert!(options.set(pane, "buffer-limit", "10").is_err());
        assert!(options
            .set(OptionTarget::Server, "buffer-limit", "0")
            .is_err());
        assert!(options
            .set(OptionTarget::Server, "exit-empty", "sometimes")
            .is_err());
        options
            .set(OptionTarget::Server, "exit-empty", "off")
            .unwrap();
        assert_eq!(
            options.get(&[OptionTarget::Server], "exit-empty").unwrap(),
            OptionValue::Flag(false)
        );
        assert!(options
            .set(OptionTarget::Server, "unknown", "value")
            .is_err());
    }

    #[test]
    fn user_options_are_bounded_strings_and_listing_is_stable() {
        let mut options = OptionStore::new();
        let target = OptionTarget::Server;

        options.set(target, "@zeta", "last").unwrap();
        options.set(target, "@alpha", "first").unwrap();
        assert!(options.set(target, "@", "empty name").is_err());
        assert!(options
            .set(target, "@huge", &"x".repeat(64 * 1024 + 1))
            .is_err());

        let listed = options.list_local(target);
        assert_eq!(
            listed
                .iter()
                .map(|(name, value)| (name.as_str(), value.to_string()))
                .collect::<Vec<_>>(),
            vec![
                ("@alpha", "first".to_string()),
                ("@zeta", "last".to_string())
            ]
        );
    }

    #[test]
    fn removing_an_object_drops_only_its_local_values() {
        let mut options = OptionStore::new();
        let first = OptionTarget::Pane(PaneId::new(1));
        let second = OptionTarget::Pane(PaneId::new(2));
        options.set(first, "@name", "first").unwrap();
        options.set(second, "@name", "second").unwrap();

        options.remove_target(first);

        assert!(options.list_local(first).is_empty());
        assert_eq!(
            options
                .get(&[second, OptionTarget::Server], "@name")
                .unwrap(),
            OptionValue::String("second".to_string())
        );
    }

    #[test]
    fn server_state_builds_the_documented_inheritance_path() {
        let mut state = ServerState::new();
        let created = state.create_session("work", 80, 24);
        state
            .options
            .set(OptionTarget::Server, "@level", "server")
            .unwrap();
        state
            .options
            .set(OptionTarget::Session(created.session), "@level", "session")
            .unwrap();
        state
            .options
            .set(OptionTarget::Window(created.window), "@level", "window")
            .unwrap();

        assert_eq!(
            state
                .option(OptionTarget::Pane(created.pane), "@level")
                .unwrap(),
            OptionValue::String("window".to_string())
        );
        assert_eq!(OptionScope::Pane.to_string(), "pane");
    }
}
