use std::{collections::BTreeMap, fmt, str::FromStr};

use crate::{CommandList, OptionTarget};

pub const MAX_HOOK_REGISTRATIONS: usize = 256;
pub const MAX_HOOK_DEPTH: u8 = 16;

#[derive(Clone, Copy, Debug, Eq, Ord, PartialEq, PartialOrd)]
pub enum HookEvent {
    ClientAttached,
    ClientDetached,
    SessionCreated,
    SessionClosed,
    WindowCreated,
    WindowClosed,
    PaneCreated,
    PaneClosed,
    BufferChanged,
    BufferDeleted,
    JobFinished,
}

impl HookEvent {
    pub const ALL: &'static [Self] = &[
        Self::ClientAttached,
        Self::ClientDetached,
        Self::SessionCreated,
        Self::SessionClosed,
        Self::WindowCreated,
        Self::WindowClosed,
        Self::PaneCreated,
        Self::PaneClosed,
        Self::BufferChanged,
        Self::BufferDeleted,
        Self::JobFinished,
    ];

    pub const fn as_str(self) -> &'static str {
        match self {
            Self::ClientAttached => "client-attached",
            Self::ClientDetached => "client-detached",
            Self::SessionCreated => "session-created",
            Self::SessionClosed => "session-closed",
            Self::WindowCreated => "window-created",
            Self::WindowClosed => "window-closed",
            Self::PaneCreated => "pane-created",
            Self::PaneClosed => "pane-closed",
            Self::BufferChanged => "buffer-changed",
            Self::BufferDeleted => "buffer-deleted",
            Self::JobFinished => "job-finished",
        }
    }
}

impl fmt::Display for HookEvent {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

impl FromStr for HookEvent {
    type Err = HookError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        Self::ALL
            .iter()
            .copied()
            .find(|event| event.as_str() == value)
            .ok_or_else(|| HookError::new(format!("unknown hook: {value}")))
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HookError(String);

impl HookError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for HookError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl std::error::Error for HookError {}

#[derive(Debug)]
pub struct HookStore {
    hooks: BTreeMap<OptionTarget, BTreeMap<HookEvent, Vec<CommandList>>>,
    registrations: usize,
    limit: usize,
}

impl HookStore {
    pub fn new() -> Self {
        Self::build(MAX_HOOK_REGISTRATIONS)
    }

    fn build(limit: usize) -> Self {
        Self {
            hooks: BTreeMap::new(),
            registrations: 0,
            limit,
        }
    }

    #[cfg(test)]
    fn with_limit(limit: usize) -> Self {
        Self::build(limit)
    }

    pub const fn registrations(&self) -> usize {
        self.registrations
    }

    pub fn set(
        &mut self,
        target: OptionTarget,
        event: HookEvent,
        commands: CommandList,
        append: bool,
    ) -> Result<(), HookError> {
        let existing = self
            .hooks
            .get(&target)
            .and_then(|events| events.get(&event))
            .map_or(0, Vec::len);
        let prospective = if append {
            self.registrations.saturating_add(1)
        } else {
            self.registrations
                .saturating_sub(existing)
                .saturating_add(1)
        };
        if prospective > self.limit {
            return Err(HookError::new(format!(
                "hook registrations exceed {}",
                self.limit
            )));
        }
        let registrations = self
            .hooks
            .entry(target)
            .or_default()
            .entry(event)
            .or_default();
        if append {
            registrations.push(commands);
        } else {
            registrations.clear();
            registrations.push(commands);
        }
        self.registrations = prospective;
        Ok(())
    }

    pub fn unset(&mut self, target: OptionTarget, event: HookEvent) -> bool {
        let removed = self
            .hooks
            .get_mut(&target)
            .and_then(|events| events.remove(&event));
        let Some(removed) = removed else {
            return false;
        };
        self.registrations -= removed.len();
        if self.hooks.get(&target).is_some_and(BTreeMap::is_empty) {
            self.hooks.remove(&target);
        }
        true
    }

    pub fn resolve(&self, path: &[OptionTarget], event: HookEvent) -> &[CommandList] {
        path.iter()
            .find_map(|target| self.hooks.get(target).and_then(|events| events.get(&event)))
            .map(Vec::as_slice)
            .unwrap_or_default()
    }

    pub fn list(
        &self,
        target: OptionTarget,
        event: Option<HookEvent>,
    ) -> Vec<(HookEvent, &[CommandList])> {
        self.hooks
            .get(&target)
            .into_iter()
            .flat_map(|events| events.iter())
            .filter(|(candidate, _)| event.is_none_or(|event| **candidate == event))
            .map(|(event, registrations)| (*event, registrations.as_slice()))
            .collect()
    }

    pub fn remove_target(&mut self, target: OptionTarget) {
        if let Some(events) = self.hooks.remove(&target) {
            self.registrations -= events.values().map(Vec::len).sum::<usize>();
        }
    }
}

impl Default for HookStore {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::{HookEvent, HookStore};
    use crate::{parse_command_text, OptionTarget, PaneId, SessionId, WindowId};

    fn commands(text: &str) -> crate::CommandList {
        parse_command_text(text).unwrap()
    }

    #[test]
    fn resolves_only_the_most_specific_inherited_registration() {
        let mut hooks = HookStore::new();
        hooks
            .set(
                OptionTarget::Server,
                HookEvent::PaneCreated,
                commands("display-message global"),
                false,
            )
            .unwrap();
        hooks
            .set(
                OptionTarget::Window(WindowId::new(2)),
                HookEvent::PaneCreated,
                commands("display-message window"),
                false,
            )
            .unwrap();

        let resolved = hooks.resolve(
            &[
                OptionTarget::Pane(PaneId::new(3)),
                OptionTarget::Window(WindowId::new(2)),
                OptionTarget::Session(SessionId::new(1)),
                OptionTarget::Server,
            ],
            HookEvent::PaneCreated,
        );
        assert_eq!(resolved, &[commands("display-message window")]);
    }

    #[test]
    fn append_preserves_order_while_replace_and_unset_are_exact() {
        let mut hooks = HookStore::new();
        for text in ["display-message one", "display-message two"] {
            hooks
                .set(
                    OptionTarget::Server,
                    HookEvent::BufferChanged,
                    commands(text),
                    true,
                )
                .unwrap();
        }
        assert_eq!(
            hooks.resolve(&[OptionTarget::Server], HookEvent::BufferChanged),
            &[
                commands("display-message one"),
                commands("display-message two")
            ]
        );

        hooks
            .set(
                OptionTarget::Server,
                HookEvent::BufferChanged,
                commands("display-message replacement"),
                false,
            )
            .unwrap();
        assert_eq!(hooks.registrations(), 1);
        assert!(hooks.unset(OptionTarget::Server, HookEvent::BufferChanged));
        assert!(hooks
            .resolve(&[OptionTarget::Server], HookEvent::BufferChanged)
            .is_empty());
    }

    #[test]
    fn listing_is_stable_and_scope_local() {
        let mut hooks = HookStore::new();
        hooks
            .set(
                OptionTarget::Server,
                HookEvent::WindowClosed,
                commands("display-message closed"),
                false,
            )
            .unwrap();
        hooks
            .set(
                OptionTarget::Server,
                HookEvent::ClientAttached,
                commands("display-message attached"),
                false,
            )
            .unwrap();
        hooks
            .set(
                OptionTarget::Session(SessionId::new(9)),
                HookEvent::PaneClosed,
                commands("display-message private"),
                false,
            )
            .unwrap();

        let listed = hooks.list(OptionTarget::Server, None);
        assert_eq!(listed.len(), 2);
        assert_eq!(listed[0].0, HookEvent::ClientAttached);
        assert_eq!(listed[1].0, HookEvent::WindowClosed);
    }

    #[test]
    fn registration_limit_rejects_before_mutation() {
        let mut hooks = HookStore::with_limit(2);
        for event in [HookEvent::PaneCreated, HookEvent::PaneClosed] {
            hooks
                .set(
                    OptionTarget::Server,
                    event,
                    commands("display-message ok"),
                    true,
                )
                .unwrap();
        }
        assert!(hooks
            .set(
                OptionTarget::Server,
                HookEvent::WindowCreated,
                commands("display-message rejected"),
                true,
            )
            .is_err());
        assert_eq!(hooks.registrations(), 2);
    }

    #[test]
    fn event_names_round_trip_and_reject_unknown_values() {
        for event in HookEvent::ALL {
            assert_eq!(event.to_string().parse::<HookEvent>().unwrap(), *event);
        }
        assert!("after-magic".parse::<HookEvent>().is_err());
    }
}
