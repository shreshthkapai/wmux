use std::fmt;

use crate::{ClientId, PaneId, ServerState, SessionId, WindowId, WinlinkId};

const MAX_TARGET_BYTES: usize = 4 * 1024;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum TargetKind {
    Client,
    Session,
    Window,
    Pane,
}

impl fmt::Display for TargetKind {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(match self {
            Self::Client => "client",
            Self::Session => "session",
            Self::Window => "window",
            Self::Pane => "pane",
        })
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TargetSpec(String);

impl TargetSpec {
    pub fn parse(raw: impl Into<String>) -> Result<Self, TargetError> {
        let raw = raw.into();
        if raw.len() > MAX_TARGET_BYTES {
            return Err(TargetError::Invalid {
                target: raw,
                reason: format!("target exceeds {MAX_TARGET_BYTES} bytes"),
            });
        }
        if raw.contains('\0') {
            return Err(TargetError::Invalid {
                target: raw,
                reason: "target contains a NUL byte".to_string(),
            });
        }
        Ok(Self(raw))
    }

    pub fn current() -> Self {
        Self("{current}".to_string())
    }

    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl fmt::Display for TargetSpec {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum TargetError {
    Invalid {
        target: String,
        reason: String,
    },
    NotFound {
        kind: TargetKind,
        target: String,
    },
    Ambiguous {
        kind: TargetKind,
        target: String,
        candidates: Vec<String>,
    },
}

impl fmt::Display for TargetError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Invalid { target, reason } => {
                write!(formatter, "invalid target {target:?}: {reason}")
            }
            Self::NotFound { kind, target } => write!(formatter, "no matching {kind}: {target}"),
            Self::Ambiguous {
                kind,
                target,
                candidates,
            } => write!(
                formatter,
                "ambiguous {kind}: {target}, could be: {}",
                candidates.join(", ")
            ),
        }
    }
}

impl std::error::Error for TargetError {}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResolveContext {
    pub client: ClientId,
    pub current_session: Option<SessionId>,
    pub current_winlink: Option<WinlinkId>,
    pub current_window: Option<WindowId>,
    pub current_pane: Option<PaneId>,
}

impl ResolveContext {
    pub fn for_client(state: &ServerState, client: ClientId) -> Result<Self, TargetError> {
        let client_state = state
            .clients
            .get(&client)
            .ok_or_else(|| TargetError::NotFound {
                kind: TargetKind::Client,
                target: client.raw().to_string(),
            })?;
        let current_session = client_state
            .attached_session
            .filter(|session| state.sessions.contains_key(session))
            .or_else(|| state.sessions.keys().next().copied());
        let current_winlink = current_session
            .and_then(|session| state.sessions.get(&session))
            .map(|session| session.active_winlink)
            .filter(|winlink| state.winlinks.contains_key(winlink));
        let current_window = current_winlink
            .and_then(|winlink| state.winlinks.get(&winlink))
            .map(|winlink| winlink.window)
            .filter(|window| state.windows.contains_key(window));
        let current_pane = current_window
            .and_then(|window| state.windows.get(&window))
            .map(|window| window.active_pane)
            .filter(|pane| state.panes.contains_key(pane));

        Ok(Self {
            client,
            current_session,
            current_winlink,
            current_window,
            current_pane,
        })
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct ResolvedTarget {
    pub client: ClientId,
    pub session: Option<SessionId>,
    pub winlink: Option<WinlinkId>,
    pub window: Option<WindowId>,
    pub pane: Option<PaneId>,
}

pub struct TargetResolver<'a> {
    state: &'a ServerState,
}

impl<'a> TargetResolver<'a> {
    pub const fn new(state: &'a ServerState) -> Self {
        Self { state }
    }

    pub fn resolve(
        &self,
        context: &ResolveContext,
        kind: TargetKind,
        spec: &TargetSpec,
    ) -> Result<ResolvedTarget, TargetError> {
        match kind {
            TargetKind::Client => self.resolve_client(context, spec),
            TargetKind::Session => self.resolve_session(context, spec),
            TargetKind::Window => self.resolve_window(context, spec),
            TargetKind::Pane => self.resolve_pane(context, spec),
        }
    }

    pub fn find_exact_session(&self, name: &str) -> Option<SessionId> {
        self.state
            .sessions
            .values()
            .find(|session| session.name == name)
            .map(|session| session.id)
    }

    fn resolve_client(
        &self,
        context: &ResolveContext,
        spec: &TargetSpec,
    ) -> Result<ResolvedTarget, TargetError> {
        let raw = spec.as_str();
        if is_current(raw) {
            return Ok(ResolvedTarget::from_context(*context));
        }
        if raw.contains([':', '.']) || raw.starts_with(['$', '@', '%']) {
            return Err(invalid(
                raw,
                "client targets cannot contain a session, window, or pane",
            ));
        }
        let id = parse_raw_id(raw).ok_or_else(|| not_found(TargetKind::Client, raw))?;
        let client = ClientId::new(id);
        let client = self
            .state
            .clients
            .contains_key(&client)
            .then_some(client)
            .ok_or_else(|| not_found(TargetKind::Client, raw))?;
        let resolved = ResolveContext::for_client(self.state, client)?;
        Ok(ResolvedTarget::from_context(resolved))
    }

    fn resolve_session(
        &self,
        context: &ResolveContext,
        spec: &TargetSpec,
    ) -> Result<ResolvedTarget, TargetError> {
        let raw = spec.as_str();
        if !is_current(raw) && (raw.contains([':', '.']) || raw.starts_with(['@', '%'])) {
            return Err(invalid(raw, "session target includes a window or pane"));
        }
        let session = self.session_id(context, raw)?;
        self.resolve_session_path(context.client, session, raw)
    }

    fn resolve_window(
        &self,
        context: &ResolveContext,
        spec: &TargetSpec,
    ) -> Result<ResolvedTarget, TargetError> {
        let raw = spec.as_str();
        if !is_current(raw) && (raw.starts_with('%') || raw.contains('.')) {
            return Err(invalid(raw, "window target includes a pane"));
        }
        let (session, winlink) = if is_current(raw) {
            (
                required(context.current_session, TargetKind::Session, raw)?,
                required(context.current_winlink, TargetKind::Window, raw)?,
            )
        } else if let Some(id) = raw.strip_prefix('@') {
            let window =
                WindowId::new(parse_raw_id(id).ok_or_else(|| not_found(TargetKind::Window, raw))?);
            if !self.state.windows.contains_key(&window) {
                return Err(not_found(TargetKind::Window, raw));
            }
            let winlink = self
                .winlink_for_window(window, context.current_session)
                .ok_or_else(|| not_found(TargetKind::Window, raw))?;
            (self.state.winlinks[&winlink].session, winlink)
        } else if is_last(raw) {
            let session = required(context.current_session, TargetKind::Session, raw)?;
            let winlink = self.state.sessions[&session]
                .previous_winlink
                .filter(|winlink| self.state.winlinks.contains_key(winlink))
                .ok_or_else(|| not_found(TargetKind::Window, raw))?;
            (session, winlink)
        } else if let Some((session_spec, window_spec)) = raw.split_once(':') {
            if window_spec.is_empty() {
                return Err(invalid(raw, "qualified window target is incomplete"));
            }
            let session = if session_spec.is_empty() {
                required(context.current_session, TargetKind::Session, raw)?
            } else {
                self.session_id(context, session_spec)?
            };
            (session, self.winlink_in_session(session, window_spec, raw)?)
        } else {
            let session = required(context.current_session, TargetKind::Session, raw)?;
            (session, self.winlink_in_session(session, raw, raw)?)
        };
        self.resolve_winlink_path(context.client, session, winlink, raw)
    }

    fn resolve_pane(
        &self,
        context: &ResolveContext,
        spec: &TargetSpec,
    ) -> Result<ResolvedTarget, TargetError> {
        let raw = spec.as_str();
        if is_current(raw) {
            return self.resolve_pane_path(
                context.client,
                required(context.current_session, TargetKind::Session, raw)?,
                required(context.current_winlink, TargetKind::Window, raw)?,
                required(context.current_window, TargetKind::Window, raw)?,
                required(context.current_pane, TargetKind::Pane, raw)?,
                raw,
            );
        }
        if raw.starts_with(['$', '@']) {
            return Err(invalid(raw, "pane target has the wrong stable ID prefix"));
        }
        if let Some(id) = raw.strip_prefix('%') {
            let pane =
                PaneId::new(parse_raw_id(id).ok_or_else(|| not_found(TargetKind::Pane, raw))?);
            let window = self
                .state
                .panes
                .get(&pane)
                .map(|pane| pane.window)
                .ok_or_else(|| not_found(TargetKind::Pane, raw))?;
            let winlink = self
                .winlink_for_window(window, context.current_session)
                .ok_or_else(|| not_found(TargetKind::Pane, raw))?;
            let session = self.state.winlinks[&winlink].session;
            return Ok(ResolvedTarget {
                client: context.client,
                session: Some(session),
                winlink: Some(winlink),
                window: Some(window),
                pane: Some(pane),
            });
        }
        if is_last(raw) {
            let session = required(context.current_session, TargetKind::Session, raw)?;
            let winlink = required(context.current_winlink, TargetKind::Window, raw)?;
            let window = required(context.current_window, TargetKind::Window, raw)?;
            let pane = self.state.windows[&window]
                .previous_pane
                .filter(|pane| self.state.panes.contains_key(pane))
                .ok_or_else(|| not_found(TargetKind::Pane, raw))?;
            return self.resolve_pane_path(context.client, session, winlink, window, pane, raw);
        }

        let (session, winlink, window, pane_spec) =
            if let Some((session_spec, rest)) = raw.split_once(':') {
                let (window_spec, pane_spec) = rest
                    .rsplit_once('.')
                    .ok_or_else(|| invalid(raw, "qualified pane target has no pane"))?;
                if window_spec.is_empty() || pane_spec.is_empty() {
                    return Err(invalid(raw, "qualified pane target is incomplete"));
                }
                let session = if session_spec.is_empty() {
                    required(context.current_session, TargetKind::Session, raw)?
                } else {
                    self.session_id(context, session_spec)?
                };
                let winlink = self.winlink_in_session(session, window_spec, raw)?;
                let window = self.state.winlinks[&winlink].window;
                (session, winlink, window, pane_spec)
            } else if let Some((window_spec, pane_spec)) = raw.rsplit_once('.') {
                if window_spec.is_empty() || pane_spec.is_empty() {
                    return Err(invalid(raw, "qualified pane target is incomplete"));
                }
                let session = required(context.current_session, TargetKind::Session, raw)?;
                let winlink = self.winlink_in_session(session, window_spec, raw)?;
                let window = self.state.winlinks[&winlink].window;
                (session, winlink, window, pane_spec)
            } else {
                (
                    required(context.current_session, TargetKind::Session, raw)?,
                    required(context.current_winlink, TargetKind::Window, raw)?,
                    required(context.current_window, TargetKind::Window, raw)?,
                    raw,
                )
            };
        let pane = self.pane_in_window(window, pane_spec, raw)?;
        self.resolve_pane_path(context.client, session, winlink, window, pane, raw)
    }

    fn session_id(&self, context: &ResolveContext, raw: &str) -> Result<SessionId, TargetError> {
        if is_current(raw) {
            return required(context.current_session, TargetKind::Session, raw);
        }
        if is_last(raw) {
            return self.state.clients[&context.client]
                .previous_session
                .filter(|session| self.state.sessions.contains_key(session))
                .ok_or_else(|| not_found(TargetKind::Session, raw));
        }
        if let Some(id) = raw.strip_prefix('$') {
            let session = SessionId::new(
                parse_raw_id(id).ok_or_else(|| not_found(TargetKind::Session, raw))?,
            );
            return self
                .state
                .sessions
                .contains_key(&session)
                .then_some(session)
                .ok_or_else(|| not_found(TargetKind::Session, raw));
        }
        if let Some(session) = self.find_exact_session(raw) {
            return Ok(session);
        }

        let mut matches = self
            .state
            .sessions
            .values()
            .filter(|session| session.name.starts_with(raw))
            .collect::<Vec<_>>();
        if matches.len() == 1 {
            return Ok(matches[0].id);
        }
        if matches.len() > 1 {
            matches.sort_by_key(|session| session.id);
            return Err(TargetError::Ambiguous {
                kind: TargetKind::Session,
                target: raw.to_string(),
                candidates: matches
                    .into_iter()
                    .map(|session| session.name.clone())
                    .collect(),
            });
        }
        if let Some(id) = parse_raw_id(raw) {
            let session = SessionId::new(id);
            if self.state.sessions.contains_key(&session) {
                return Ok(session);
            }
        }
        Err(not_found(TargetKind::Session, raw))
    }

    fn winlink_in_session(
        &self,
        session: SessionId,
        selector: &str,
        original: &str,
    ) -> Result<WinlinkId, TargetError> {
        let session_state = self
            .state
            .sessions
            .get(&session)
            .ok_or_else(|| not_found(TargetKind::Session, original))?;
        if is_current(selector) {
            return Ok(session_state.active_winlink);
        }
        if is_last(selector) {
            return session_state
                .previous_winlink
                .filter(|winlink| self.state.winlinks.contains_key(winlink))
                .ok_or_else(|| not_found(TargetKind::Window, original));
        }
        if let Some(id) = selector.strip_prefix('@') {
            let window = WindowId::new(
                parse_raw_id(id).ok_or_else(|| not_found(TargetKind::Window, original))?,
            );
            return session_state
                .winlinks
                .iter()
                .copied()
                .find(|winlink| {
                    self.state
                        .winlinks
                        .get(winlink)
                        .is_some_and(|link| link.window == window)
                })
                .ok_or_else(|| not_found(TargetKind::Window, original));
        }
        if let Some(relative) = parse_relative(selector) {
            let mut winlinks = session_state
                .winlinks
                .iter()
                .filter_map(|id| self.state.winlinks.get(id))
                .collect::<Vec<_>>();
            winlinks.sort_by_key(|winlink| (winlink.index, winlink.id));
            let current = winlinks
                .iter()
                .position(|winlink| winlink.id == session_state.active_winlink)
                .ok_or_else(|| not_found(TargetKind::Window, original))?;
            let len = i128::try_from(winlinks.len()).expect("window count fits i128");
            let next = (i128::try_from(current).expect("window index fits i128") + relative)
                .rem_euclid(len);
            return Ok(winlinks[usize::try_from(next).expect("wrapped index fits usize")].id);
        }
        if let Ok(index) = selector.parse::<u16>() {
            return session_state
                .winlinks
                .iter()
                .filter_map(|id| self.state.winlinks.get(id))
                .find(|winlink| winlink.index == index)
                .map(|winlink| winlink.id)
                .ok_or_else(|| not_found(TargetKind::Window, original));
        }

        let mut exact = Vec::new();
        let mut prefix = Vec::new();
        for winlink in session_state
            .winlinks
            .iter()
            .filter_map(|id| self.state.winlinks.get(id))
        {
            let Some(window) = self.state.windows.get(&winlink.window) else {
                continue;
            };
            if window.name == selector {
                exact.push((winlink, window));
            } else if window.name.starts_with(selector) {
                prefix.push((winlink, window));
            }
        }
        let matches = if exact.is_empty() { prefix } else { exact };
        match matches.as_slice() {
            [(winlink, _)] => Ok(winlink.id),
            [] => Err(not_found(TargetKind::Window, original)),
            _ => Err(TargetError::Ambiguous {
                kind: TargetKind::Window,
                target: original.to_string(),
                candidates: matches
                    .iter()
                    .map(|(winlink, window)| format!("{}:{}", winlink.index, window.name))
                    .collect(),
            }),
        }
    }

    fn pane_in_window(
        &self,
        window: WindowId,
        selector: &str,
        original: &str,
    ) -> Result<PaneId, TargetError> {
        let window_state = self
            .state
            .windows
            .get(&window)
            .ok_or_else(|| not_found(TargetKind::Window, original))?;
        if is_current(selector) {
            return Ok(window_state.active_pane);
        }
        if is_last(selector) {
            return window_state
                .previous_pane
                .filter(|pane| self.state.panes.contains_key(pane))
                .ok_or_else(|| not_found(TargetKind::Pane, original));
        }
        let index = selector
            .parse::<usize>()
            .map_err(|_| not_found(TargetKind::Pane, original))?;
        window_state
            .panes
            .get(index)
            .copied()
            .filter(|pane| self.state.panes.contains_key(pane))
            .ok_or_else(|| not_found(TargetKind::Pane, original))
    }

    fn winlink_for_window(
        &self,
        window: WindowId,
        preferred_session: Option<SessionId>,
    ) -> Option<WinlinkId> {
        self.state
            .winlinks
            .values()
            .filter(|winlink| winlink.window == window)
            .min_by_key(|winlink| {
                (
                    u8::from(Some(winlink.session) != preferred_session),
                    winlink.session,
                    winlink.id,
                )
            })
            .map(|winlink| winlink.id)
    }

    fn resolve_session_path(
        &self,
        client: ClientId,
        session: SessionId,
        original: &str,
    ) -> Result<ResolvedTarget, TargetError> {
        let session_state = self
            .state
            .sessions
            .get(&session)
            .ok_or_else(|| not_found(TargetKind::Session, original))?;
        self.resolve_winlink_path(client, session, session_state.active_winlink, original)
    }

    fn resolve_winlink_path(
        &self,
        client: ClientId,
        session: SessionId,
        winlink: WinlinkId,
        original: &str,
    ) -> Result<ResolvedTarget, TargetError> {
        let link = self
            .state
            .winlinks
            .get(&winlink)
            .filter(|link| link.session == session)
            .ok_or_else(|| not_found(TargetKind::Window, original))?;
        let window = self
            .state
            .windows
            .get(&link.window)
            .ok_or_else(|| not_found(TargetKind::Window, original))?;
        self.resolve_pane_path(
            client,
            session,
            winlink,
            window.id,
            window.active_pane,
            original,
        )
    }

    fn resolve_pane_path(
        &self,
        client: ClientId,
        session: SessionId,
        winlink: WinlinkId,
        window: WindowId,
        pane: PaneId,
        original: &str,
    ) -> Result<ResolvedTarget, TargetError> {
        self.state
            .winlinks
            .get(&winlink)
            .filter(|link| link.session == session && link.window == window)
            .ok_or_else(|| not_found(TargetKind::Window, original))?;
        self.state
            .panes
            .get(&pane)
            .filter(|pane_state| pane_state.window == window)
            .ok_or_else(|| not_found(TargetKind::Pane, original))?;
        Ok(ResolvedTarget {
            client,
            session: Some(session),
            winlink: Some(winlink),
            window: Some(window),
            pane: Some(pane),
        })
    }
}

impl ResolvedTarget {
    const fn from_context(context: ResolveContext) -> Self {
        Self {
            client: context.client,
            session: context.current_session,
            winlink: context.current_winlink,
            window: context.current_window,
            pane: context.current_pane,
        }
    }
}

fn is_current(raw: &str) -> bool {
    raw.is_empty() || matches!(raw, "@" | "{current}")
}

fn is_last(raw: &str) -> bool {
    matches!(raw, "!" | "{last}")
}

fn parse_raw_id(raw: &str) -> Option<u64> {
    (!raw.is_empty()).then(|| raw.parse().ok()).flatten()
}

fn parse_relative(raw: &str) -> Option<i128> {
    (raw.starts_with('+') || raw.starts_with('-'))
        .then(|| raw.parse().ok())
        .flatten()
}

fn required<T: Copy>(value: Option<T>, kind: TargetKind, raw: &str) -> Result<T, TargetError> {
    value.ok_or_else(|| not_found(kind, raw))
}

fn invalid(raw: &str, reason: &str) -> TargetError {
    TargetError::Invalid {
        target: raw.to_string(),
        reason: reason.to_string(),
    }
}

fn not_found(kind: TargetKind, raw: &str) -> TargetError {
    TargetError::NotFound {
        kind,
        target: raw.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::{
        ResolveContext, TargetError, TargetKind, TargetResolver, TargetSpec, MAX_TARGET_BYTES,
    };
    use crate::{ServerState, SplitDirection};

    struct Fixture {
        state: ServerState,
        client: crate::ClientId,
        session: crate::SessionId,
        first_winlink: crate::WinlinkId,
        first_window: crate::WindowId,
        first_pane: crate::PaneId,
        second_winlink: crate::WinlinkId,
        second_window: crate::WindowId,
        second_first_pane: crate::PaneId,
        second_active_pane: crate::PaneId,
    }

    fn fixture() -> Fixture {
        let mut state = ServerState::new();
        let client = state.add_client();
        let first = state.create_session("work", 80, 24);
        let second = state
            .create_window(first.session, Some("logs".to_string()), 80, 24)
            .unwrap();
        let second_active_pane = state
            .split_pane(
                second.window,
                Some(second.pane),
                SplitDirection::LeftRight,
                80,
                24,
            )
            .unwrap();
        state.select_window(first.session, 0).unwrap();
        state.select_window(first.session, 1).unwrap();
        state.attach_client(client, first.session).unwrap();
        Fixture {
            state,
            client,
            session: first.session,
            first_winlink: first.winlink,
            first_window: first.window,
            first_pane: first.pane,
            second_winlink: second.winlink,
            second_window: second.window,
            second_first_pane: second.pane,
            second_active_pane,
        }
    }

    #[test]
    fn resolves_current_stable_qualified_relative_and_last_targets() {
        let fixture = fixture();
        let resolver = TargetResolver::new(&fixture.state);
        let context = ResolveContext::for_client(&fixture.state, fixture.client).unwrap();

        let current = resolver
            .resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse("{current}").unwrap(),
            )
            .unwrap();
        assert_eq!(current.session, Some(fixture.session));
        assert_eq!(current.winlink, Some(fixture.second_winlink));
        assert_eq!(current.window, Some(fixture.second_window));
        assert_eq!(current.pane, Some(fixture.second_active_pane));

        let stable = resolver
            .resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse(format!("%{}", fixture.first_pane.raw())).unwrap(),
            )
            .unwrap();
        assert_eq!(stable.window, Some(fixture.first_window));
        assert_eq!(stable.pane, Some(fixture.first_pane));

        let qualified = resolver
            .resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse("work:0.0").unwrap(),
            )
            .unwrap();
        assert_eq!(qualified.winlink, Some(fixture.first_winlink));
        assert_eq!(qualified.pane, Some(fixture.first_pane));

        let current_qualified = resolver
            .resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse(":0.0").unwrap(),
            )
            .unwrap();
        assert_eq!(current_qualified.session, Some(fixture.session));
        assert_eq!(current_qualified.pane, Some(fixture.first_pane));

        let relative = resolver
            .resolve(
                &context,
                TargetKind::Window,
                &TargetSpec::parse("-1").unwrap(),
            )
            .unwrap();
        assert_eq!(relative.winlink, Some(fixture.first_winlink));

        let last_pane = resolver
            .resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse("{last}").unwrap(),
            )
            .unwrap();
        assert_eq!(last_pane.pane, Some(fixture.second_first_pane));
    }

    #[test]
    fn exact_name_wins_and_nonunique_prefix_is_ambiguous() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let work = state.create_session("work", 80, 24);
        state.create_session("worker", 80, 24);
        state.attach_client(client, work.session).unwrap();
        let resolver = TargetResolver::new(&state);
        let context = ResolveContext::for_client(&state, client).unwrap();

        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Session,
                    &TargetSpec::parse("work").unwrap(),
                )
                .unwrap()
                .session,
            Some(work.session)
        );
        assert!(matches!(
            resolver.resolve(
                &context,
                TargetKind::Session,
                &TargetSpec::parse("wor").unwrap(),
            ),
            Err(TargetError::Ambiguous { .. })
        ));
        assert!(matches!(
            resolver.resolve(
                &context,
                TargetKind::Session,
                &TargetSpec::parse("missing").unwrap(),
            ),
            Err(TargetError::NotFound { .. })
        ));
    }

    #[test]
    fn last_session_and_client_ids_are_resolved_per_client() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let work = state.create_session("work", 80, 24);
        let other = state.create_session("other", 80, 24);
        state.attach_client(client, work.session).unwrap();
        state.attach_client(client, other.session).unwrap();
        let resolver = TargetResolver::new(&state);
        let context = ResolveContext::for_client(&state, client).unwrap();

        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Session,
                    &TargetSpec::parse("{last}").unwrap(),
                )
                .unwrap()
                .session,
            Some(work.session)
        );
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Client,
                    &TargetSpec::parse(client.raw().to_string()).unwrap(),
                )
                .unwrap()
                .client,
            client
        );
    }

    #[test]
    fn detach_does_not_erase_previous_session_history() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let work = state.create_session("work", 80, 24);
        let other = state.create_session("other", 80, 24);
        state.attach_client(client, work.session).unwrap();
        state.attach_client(client, other.session).unwrap();
        state.detach_client(client);
        state.attach_client(client, other.session).unwrap();

        let resolver = TargetResolver::new(&state);
        let context = ResolveContext::for_client(&state, client).unwrap();
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Session,
                    &TargetSpec::parse("{last}").unwrap(),
                )
                .unwrap()
                .session,
            Some(work.session)
        );
    }

    #[test]
    fn stale_ids_and_wrong_target_shapes_are_rejected() {
        let mut fixture = fixture();
        fixture.state.kill_pane(fixture.first_pane).unwrap();
        let resolver = TargetResolver::new(&fixture.state);
        let context = ResolveContext::for_client(&fixture.state, fixture.client).unwrap();

        assert!(matches!(
            resolver.resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse(format!("%{}", fixture.first_pane.raw())).unwrap(),
            ),
            Err(TargetError::NotFound { .. })
        ));
        assert!(matches!(
            resolver.resolve(
                &context,
                TargetKind::Session,
                &TargetSpec::parse("work:0.0").unwrap(),
            ),
            Err(TargetError::Invalid { .. })
        ));
    }

    #[test]
    fn current_alias_stable_ids_prefixes_and_positive_wrap_are_type_aware() {
        let mut fixture = fixture();
        fixture.state.create_session("worker", 80, 24);
        let resolver = TargetResolver::new(&fixture.state);
        let context = ResolveContext::for_client(&fixture.state, fixture.client).unwrap();

        for current in ["", "@", "{current}"] {
            assert_eq!(
                resolver
                    .resolve(
                        &context,
                        TargetKind::Pane,
                        &TargetSpec::parse(current).unwrap(),
                    )
                    .unwrap()
                    .pane,
                Some(fixture.second_active_pane)
            );
        }
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Session,
                    &TargetSpec::parse(format!("${}", fixture.session.raw())).unwrap(),
                )
                .unwrap()
                .session,
            Some(fixture.session)
        );
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Window,
                    &TargetSpec::parse(format!("@{}", fixture.first_window.raw())).unwrap(),
                )
                .unwrap()
                .winlink,
            Some(fixture.first_winlink)
        );
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Window,
                    &TargetSpec::parse("+1").unwrap(),
                )
                .unwrap()
                .winlink,
            Some(fixture.first_winlink)
        );
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Session,
                    &TargetSpec::parse("worke").unwrap(),
                )
                .unwrap()
                .session,
            TargetResolver::new(&fixture.state).find_exact_session("worker")
        );
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Client,
                    &TargetSpec::parse("{current}").unwrap(),
                )
                .unwrap()
                .client,
            fixture.client
        );
    }

    #[test]
    fn target_size_is_bounded_before_resolution() {
        assert!(matches!(
            TargetSpec::parse("x".repeat(MAX_TARGET_BYTES + 1)),
            Err(TargetError::Invalid { .. })
        ));
        assert!(TargetSpec::parse("x".repeat(MAX_TARGET_BYTES)).is_ok());
    }

    #[test]
    fn window_names_prefer_exact_matches_then_require_a_unique_prefix() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let created = state.create_session("work", 80, 24);
        state.rename_window(created.window, "log").unwrap();
        let logger = state
            .create_window(created.session, Some("logger".to_string()), 80, 24)
            .unwrap();
        state.attach_client(client, created.session).unwrap();
        let resolver = TargetResolver::new(&state);
        let context = ResolveContext::for_client(&state, client).unwrap();

        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Window,
                    &TargetSpec::parse("log").unwrap(),
                )
                .unwrap()
                .winlink,
            Some(created.winlink)
        );
        assert_eq!(
            resolver
                .resolve(
                    &context,
                    TargetKind::Window,
                    &TargetSpec::parse("logg").unwrap(),
                )
                .unwrap()
                .winlink,
            Some(logger.winlink)
        );
        assert!(matches!(
            resolver.resolve(
                &context,
                TargetKind::Window,
                &TargetSpec::parse("lo").unwrap(),
            ),
            Err(TargetError::Ambiguous { .. })
        ));
    }

    #[test]
    fn stable_shared_pane_preserves_the_invoking_sessions_winlink() {
        let mut state = ServerState::new();
        let client = state.add_client();
        let work = state.create_session("work", 80, 24);
        let grouped = state.create_grouped_session("other", work.session).unwrap();
        state.attach_client(client, grouped.session).unwrap();
        let resolver = TargetResolver::new(&state);
        let context = ResolveContext::for_client(&state, client).unwrap();

        let resolved = resolver
            .resolve(
                &context,
                TargetKind::Pane,
                &TargetSpec::parse(format!("%{}", work.pane.raw())).unwrap(),
            )
            .unwrap();
        assert_eq!(resolved.session, Some(grouped.session));
        assert_eq!(resolved.winlink, Some(grouped.winlink));
        assert_eq!(resolved.window, Some(work.window));
    }
}
