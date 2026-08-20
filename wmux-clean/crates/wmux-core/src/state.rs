use std::collections::BTreeMap;

use crate::{
    ClientId, LayoutNode, PaneId, Rect, ResizeDirection, Screen, SessionGroupId, SessionId,
    SplitDirection, TerminalEngine, WindowId, WinlinkId,
};

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Client {
    pub id: ClientId,
    pub attached_session: Option<SessionId>,
    pub attached_pane: Option<PaneId>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Session {
    pub id: SessionId,
    pub name: String,
    pub group: Option<SessionGroupId>,
    pub winlinks: Vec<WinlinkId>,
    pub active_winlink: WinlinkId,
    pub previous_winlink: Option<WinlinkId>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SessionGroup {
    pub id: SessionGroupId,
    pub name: String,
    pub sessions: Vec<SessionId>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Winlink {
    pub id: WinlinkId,
    pub session: SessionId,
    pub index: u16,
    pub window: WindowId,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Window {
    pub id: WindowId,
    pub name: String,
    pub panes: Vec<PaneId>,
    pub active_pane: PaneId,
    pub previous_pane: Option<PaneId>,
    pub layout: LayoutNode,
    pub zoomed: Option<PaneId>,
    pub cols: u16,
    pub rows: u16,
}

#[derive(Debug)]
pub struct Pane {
    pub id: PaneId,
    pub window: WindowId,
    pub rect: Rect,
    pub screen: Screen,
    pub terminal: TerminalEngine,
    pub dead: bool,
    pub remain_on_exit: bool,
}

impl Pane {
    pub const fn generation(&self) -> u64 {
        self.screen.generation()
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CreatedSession {
    pub session: SessionId,
    pub winlink: WinlinkId,
    pub window: WindowId,
    pub pane: PaneId,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct CreatedWindow {
    pub winlink: WinlinkId,
    pub window: WindowId,
    pub pane: PaneId,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct PaneResize {
    pub pane: PaneId,
    pub old: Rect,
    pub new: Rect,
}

#[derive(Debug)]
pub struct ServerState {
    next_id: u64,
    pub clients: BTreeMap<ClientId, Client>,
    pub sessions: BTreeMap<SessionId, Session>,
    pub session_groups: BTreeMap<SessionGroupId, SessionGroup>,
    pub winlinks: BTreeMap<WinlinkId, Winlink>,
    pub windows: BTreeMap<WindowId, Window>,
    pub panes: BTreeMap<PaneId, Pane>,
    pending_pane_resizes: BTreeMap<PaneId, PaneResize>,
}

impl ServerState {
    pub fn new() -> Self {
        Self {
            next_id: 1,
            clients: BTreeMap::new(),
            sessions: BTreeMap::new(),
            session_groups: BTreeMap::new(),
            winlinks: BTreeMap::new(),
            windows: BTreeMap::new(),
            panes: BTreeMap::new(),
            pending_pane_resizes: BTreeMap::new(),
        }
    }

    pub fn add_client(&mut self) -> ClientId {
        let id = ClientId::new(self.alloc());
        self.clients.insert(
            id,
            Client {
                id,
                attached_session: None,
                attached_pane: None,
            },
        );
        id
    }

    pub fn remove_client(&mut self, client: ClientId) {
        self.clients.remove(&client);
    }

    pub fn create_session(
        &mut self,
        name: impl Into<String>,
        cols: u16,
        rows: u16,
    ) -> CreatedSession {
        let session = SessionId::new(self.alloc());
        let window = WindowId::new(self.alloc());
        let winlink = WinlinkId::new(self.alloc());
        let pane = self.create_pane_for_window(window, cols, rows);

        self.windows.insert(
            window,
            Window {
                id: window,
                name: "0".to_string(),
                panes: vec![pane],
                active_pane: pane,
                previous_pane: None,
                layout: LayoutNode::leaf(pane),
                zoomed: None,
                cols,
                rows,
            },
        );
        self.winlinks.insert(
            winlink,
            Winlink {
                id: winlink,
                session,
                index: 0,
                window,
            },
        );
        self.sessions.insert(
            session,
            Session {
                id: session,
                name: name.into(),
                group: None,
                winlinks: vec![winlink],
                active_winlink: winlink,
                previous_winlink: None,
            },
        );
        self.recompute_window_layout_and_resize_screens(window);

        CreatedSession {
            session,
            winlink,
            window,
            pane,
        }
    }

    pub fn create_grouped_session(
        &mut self,
        name: impl Into<String>,
        target: SessionId,
    ) -> Option<CreatedSession> {
        let source = self.sessions.get(&target)?.clone();
        let session = SessionId::new(self.alloc());
        let mut winlinks = Vec::with_capacity(source.winlinks.len());
        let mut active_winlink = None;
        let mut first_window = None;
        let mut first_pane = None;

        for source_winlink_id in &source.winlinks {
            let source_winlink = self.winlinks.get(source_winlink_id)?.clone();
            let winlink = WinlinkId::new(self.alloc());
            self.winlinks.insert(
                winlink,
                Winlink {
                    id: winlink,
                    session,
                    index: source_winlink.index,
                    window: source_winlink.window,
                },
            );
            if first_window.is_none() {
                first_window = Some(source_winlink.window);
                first_pane = self
                    .windows
                    .get(&source_winlink.window)
                    .map(|window| window.active_pane);
            }
            if source_winlink_id == &source.active_winlink {
                active_winlink = Some(winlink);
            }
            winlinks.push(winlink);
        }

        let active_winlink = active_winlink.or_else(|| winlinks.first().copied())?;
        self.sessions.insert(
            session,
            Session {
                id: session,
                name: name.into(),
                group: None,
                winlinks,
                active_winlink,
                previous_winlink: None,
            },
        );
        self.add_sessions_to_group(target, session);

        let active = self.winlinks.get(&active_winlink)?;
        let window = active.window;
        let pane = self.windows.get(&window)?.active_pane;
        Some(CreatedSession {
            session,
            winlink: active_winlink,
            window: first_window.unwrap_or(window),
            pane: first_pane.unwrap_or(pane),
        })
    }

    pub fn create_window(
        &mut self,
        session: SessionId,
        name: Option<String>,
        cols: u16,
        rows: u16,
    ) -> Option<CreatedWindow> {
        let index = self.next_window_index(session)?;
        let window = WindowId::new(self.alloc());
        let winlink = WinlinkId::new(self.alloc());
        let pane = self.create_pane_for_window(window, cols, rows);

        self.windows.insert(
            window,
            Window {
                id: window,
                name: name.unwrap_or_else(|| index.to_string()),
                panes: vec![pane],
                active_pane: pane,
                previous_pane: None,
                layout: LayoutNode::leaf(pane),
                zoomed: None,
                cols,
                rows,
            },
        );
        self.winlinks.insert(
            winlink,
            Winlink {
                id: winlink,
                session,
                index,
                window,
            },
        );
        let session_state = self.sessions.get_mut(&session)?;
        session_state.winlinks.push(winlink);
        session_state.active_winlink = winlink;
        self.synchronize_group_from_new_window(session, index, window);
        self.recompute_window_layout_and_resize_screens(window);

        Some(CreatedWindow {
            winlink,
            window,
            pane,
        })
    }

    pub fn create_pane(&mut self, window: WindowId, cols: u16, rows: u16) -> Option<PaneId> {
        self.split_pane(window, None, SplitDirection::TopBottom, cols, rows)
    }

    pub fn split_pane(
        &mut self,
        window: WindowId,
        target: Option<PaneId>,
        direction: SplitDirection,
        cols: u16,
        rows: u16,
    ) -> Option<PaneId> {
        if !self.windows.contains_key(&window) {
            return None;
        }
        let pane = self.create_pane_for_window(window, cols, rows);
        let window_state = self.windows.get_mut(&window)?;
        let target = target.unwrap_or(window_state.active_pane);
        if !window_state.layout.split_leaf(target, pane, direction) {
            return None;
        }
        window_state.panes.push(pane);
        window_state.previous_pane = Some(window_state.active_pane);
        window_state.active_pane = pane;
        window_state.zoomed = None;
        self.recompute_window_layout(window);
        Some(pane)
    }

    pub fn attach_client(&mut self, client: ClientId, session: SessionId) -> Option<PaneId> {
        let pane = self.active_pane_for_session(session)?;
        let client = self.clients.get_mut(&client)?;
        client.attached_session = Some(session);
        client.attached_pane = Some(pane);
        Some(pane)
    }

    pub fn detach_client(&mut self, client: ClientId) {
        if let Some(client) = self.clients.get_mut(&client) {
            client.attached_session = None;
            client.attached_pane = None;
        }
    }

    pub fn select_window(&mut self, session: SessionId, index: u16) -> Option<PaneId> {
        let winlink = self
            .winlinks
            .values()
            .find(|winlink| winlink.session == session && winlink.index == index)?
            .id;
        let session_state = self.sessions.get_mut(&session)?;
        if session_state.active_winlink != winlink {
            session_state.previous_winlink = Some(session_state.active_winlink);
        }
        session_state.active_winlink = winlink;
        self.active_pane_for_session(session)
    }

    pub fn select_next_window(&mut self, session: SessionId, reverse: bool) -> Option<PaneId> {
        let session_state = self.sessions.get(&session)?.clone();
        let mut winlinks = session_state.winlinks.clone();
        winlinks.sort_by_key(|winlink| {
            self.winlinks
                .get(winlink)
                .map(|winlink| winlink.index)
                .unwrap_or(u16::MAX)
        });
        let current = winlinks
            .iter()
            .position(|winlink| *winlink == session_state.active_winlink)?;
        let next = if reverse {
            if current == 0 {
                winlinks.len() - 1
            } else {
                current - 1
            }
        } else {
            (current + 1) % winlinks.len()
        };
        let index = self.winlinks.get(&winlinks[next])?.index;
        self.select_window(session, index)
    }

    pub fn select_last_window(&mut self, session: SessionId) -> Option<PaneId> {
        let previous = self.sessions.get(&session)?.previous_winlink?;
        let index = self.winlinks.get(&previous)?.index;
        self.select_window(session, index)
    }

    pub fn select_pane(&mut self, window: WindowId, pane: PaneId) -> Option<PaneId> {
        let window_state = self.windows.get_mut(&window)?;
        if !window_state.panes.contains(&pane) {
            return None;
        }
        if window_state.active_pane != pane {
            window_state.previous_pane = Some(window_state.active_pane);
        }
        window_state.active_pane = pane;
        Some(pane)
    }

    pub fn select_last_pane(&mut self, window: WindowId) -> Option<PaneId> {
        let pane = self.windows.get(&window)?.previous_pane?;
        self.select_pane(window, pane)
    }

    pub fn select_adjacent_pane(
        &mut self,
        window: WindowId,
        direction: ResizeDirection,
    ) -> Option<PaneId> {
        let active = self.windows.get(&window)?.active_pane;
        let active_rect = self.panes.get(&active)?.rect;
        let mut candidates = self
            .windows
            .get(&window)?
            .panes
            .iter()
            .filter_map(|pane| {
                let rect = self.panes.get(pane)?.rect;
                let score = match direction {
                    ResizeDirection::Left if rect.x.saturating_add(rect.cols) <= active_rect.x => {
                        Some(
                            active_rect
                                .x
                                .saturating_sub(rect.x.saturating_add(rect.cols)),
                        )
                    }
                    ResizeDirection::Right
                        if rect.x >= active_rect.x.saturating_add(active_rect.cols) =>
                    {
                        Some(
                            rect.x
                                .saturating_sub(active_rect.x.saturating_add(active_rect.cols)),
                        )
                    }
                    ResizeDirection::Up if rect.y.saturating_add(rect.rows) <= active_rect.y => {
                        Some(
                            active_rect
                                .y
                                .saturating_sub(rect.y.saturating_add(rect.rows)),
                        )
                    }
                    ResizeDirection::Down
                        if rect.y >= active_rect.y.saturating_add(active_rect.rows) =>
                    {
                        Some(
                            rect.y
                                .saturating_sub(active_rect.y.saturating_add(active_rect.rows)),
                        )
                    }
                    _ => None,
                }?;
                Some((*pane, score))
            })
            .collect::<Vec<_>>();
        candidates.sort_by_key(|(_, score)| *score);
        self.select_pane(window, candidates.first()?.0)
    }

    pub fn toggle_zoom(&mut self, window: WindowId) -> Option<PaneId> {
        let window_state = self.windows.get_mut(&window)?;
        let pane = window_state.active_pane;
        window_state.zoomed = match window_state.zoomed {
            Some(_) => None,
            None => Some(pane),
        };
        self.recompute_window_layout(window);
        Some(pane)
    }

    pub fn rename_window(&mut self, window: WindowId, name: impl Into<String>) -> Option<()> {
        self.windows.get_mut(&window)?.name = name.into();
        Some(())
    }

    pub fn resize_window(&mut self, window: WindowId, cols: u16, rows: u16) -> Option<()> {
        let window_state = self.windows.get_mut(&window)?;
        if window_state.cols == cols.max(1) && window_state.rows == rows.max(1) {
            return Some(());
        }
        window_state.cols = cols.max(1);
        window_state.rows = rows.max(1);
        self.recompute_window_layout(window);
        Some(())
    }

    pub fn resize_active_pane(
        &mut self,
        window: WindowId,
        direction: ResizeDirection,
        amount: u16,
    ) -> Option<()> {
        let window_state = self.windows.get_mut(&window)?;
        let active = window_state.active_pane;
        let split = match direction {
            ResizeDirection::Left | ResizeDirection::Right => SplitDirection::LeftRight,
            ResizeDirection::Up | ResizeDirection::Down => SplitDirection::TopBottom,
        };
        window_state
            .layout
            .resize_leaf(active, split, direction, amount.max(1));
        self.recompute_window_layout(window);
        Some(())
    }

    pub fn rotate_window(&mut self, window: WindowId, reverse: bool) -> Option<()> {
        let window_state = self.windows.get_mut(&window)?;
        if window_state.layout.rotate(reverse) {
            self.recompute_window_layout(window);
        }
        Some(())
    }

    pub fn swap_active_pane(&mut self, window: WindowId, direction: ResizeDirection) -> Option<()> {
        let active = self.windows.get(&window)?.active_pane;
        let other = self.select_adjacent_pane(window, direction)?;
        let window_state = self.windows.get_mut(&window)?;
        window_state.layout.swap_panes(active, other);
        window_state.active_pane = active;
        self.recompute_window_layout(window);
        Some(())
    }

    pub fn kill_pane(&mut self, pane: PaneId) -> Option<Vec<PaneId>> {
        let window = self.panes.get(&pane)?.window;
        if self.windows.get(&window)?.panes.len() <= 1 {
            return self.kill_window(window);
        }
        {
            let window_state = self.windows.get_mut(&window)?;
            window_state.panes.retain(|candidate| *candidate != pane);
            window_state.layout.remove_leaf(pane);
            if window_state.active_pane == pane {
                window_state.active_pane = *window_state.panes.first()?;
            }
            if window_state.previous_pane == Some(pane) {
                window_state.previous_pane = None;
            }
            if window_state.zoomed == Some(pane) {
                window_state.zoomed = None;
            }
        }
        self.panes.remove(&pane);
        self.pending_pane_resizes.remove(&pane);
        self.recompute_window_layout(window);
        self.refresh_attached_panes();
        Some(vec![pane])
    }

    pub fn kill_window(&mut self, window: WindowId) -> Option<Vec<PaneId>> {
        let killed = self.windows.remove(&window)?.panes;
        for pane in &killed {
            self.panes.remove(pane);
            self.pending_pane_resizes.remove(pane);
        }
        let winlinks = self
            .winlinks
            .values()
            .filter(|winlink| winlink.window == window)
            .map(|winlink| winlink.id)
            .collect::<Vec<_>>();
        for winlink in &winlinks {
            self.winlinks.remove(winlink);
        }
        for session in self.sessions.values_mut() {
            session
                .winlinks
                .retain(|winlink| !winlinks.contains(winlink));
            if session
                .previous_winlink
                .is_some_and(|previous| winlinks.contains(&previous))
            {
                session.previous_winlink = None;
            }
            if winlinks.contains(&session.active_winlink) {
                if let Some(next) = session.winlinks.first().copied() {
                    session.active_winlink = next;
                    session.previous_winlink = None;
                }
            }
        }
        let empty_sessions = self
            .sessions
            .values()
            .filter_map(|session| session.winlinks.is_empty().then_some(session.id))
            .collect::<Vec<_>>();
        for session in empty_sessions {
            self.remove_session_record(session);
        }
        self.refresh_attached_panes();
        Some(killed)
    }

    pub fn kill_session(&mut self, session: SessionId) -> Option<Vec<PaneId>> {
        let session_state = self.remove_session_record(session)?;
        let windows = session_state
            .winlinks
            .iter()
            .filter_map(|winlink| self.winlinks.remove(winlink))
            .map(|winlink| winlink.window)
            .collect::<Vec<_>>();
        let mut killed = Vec::new();
        for window in windows {
            let still_linked = self
                .winlinks
                .values()
                .any(|winlink| winlink.window == window);
            if !still_linked {
                if let Some(mut panes) = self.kill_window(window) {
                    killed.append(&mut panes);
                }
            }
        }
        self.refresh_attached_panes();
        Some(killed)
    }

    pub fn refresh_attached_panes(&mut self) {
        let panes = self
            .clients
            .iter()
            .map(|(client_id, client)| {
                let pane = client
                    .attached_session
                    .and_then(|session| self.active_pane_for_session(session));
                (*client_id, pane)
            })
            .collect::<Vec<_>>();
        for (client_id, pane) in panes {
            if let Some(client) = self.clients.get_mut(&client_id) {
                client.attached_pane = pane;
            }
        }
    }

    fn remove_session_record(&mut self, session: SessionId) -> Option<Session> {
        let removed = self.sessions.remove(&session)?;
        for client in self.clients.values_mut() {
            if client.attached_session == Some(session) {
                client.attached_session = None;
                client.attached_pane = None;
            }
        }
        if let Some(group_id) = removed.group {
            let remove_group = if let Some(group) = self.session_groups.get_mut(&group_id) {
                group.sessions.retain(|candidate| *candidate != session);
                group.sessions.is_empty()
            } else {
                false
            };
            if remove_group {
                self.session_groups.remove(&group_id);
            }
        }
        Some(removed)
    }

    pub fn active_pane_for_session(&self, session: SessionId) -> Option<PaneId> {
        let session = self.sessions.get(&session)?;
        let winlink = self.winlinks.get(&session.active_winlink)?;
        self.windows
            .get(&winlink.window)
            .map(|window| window.active_pane)
    }

    pub fn active_window_for_session(&self, session: SessionId) -> Option<WindowId> {
        let session = self.sessions.get(&session)?;
        self.winlinks
            .get(&session.active_winlink)
            .map(|winlink| winlink.window)
    }

    pub fn find_session(&self, target: Option<&str>) -> Option<SessionId> {
        match target {
            Some(target) => self
                .sessions
                .values()
                .find(|session| session.name == target || session.id.raw().to_string() == target)
                .map(|session| session.id),
            None => self.sessions.keys().next().copied(),
        }
    }

    pub fn pane_mut(&mut self, pane: PaneId) -> Option<&mut Pane> {
        self.panes.get_mut(&pane)
    }

    pub fn pane(&self, pane: PaneId) -> Option<&Pane> {
        self.panes.get(&pane)
    }

    pub fn take_pending_pane_resizes(&mut self) -> Vec<PaneResize> {
        std::mem::take(&mut self.pending_pane_resizes)
            .into_values()
            .collect()
    }

    pub fn window(&self, window: WindowId) -> Option<&Window> {
        self.windows.get(&window)
    }

    pub fn active_window_and_pane_for_client(
        &self,
        client: ClientId,
    ) -> Option<(SessionId, WindowId, PaneId)> {
        let session = self.clients.get(&client)?.attached_session?;
        let window = self.active_window_for_session(session)?;
        let pane = self.windows.get(&window)?.active_pane;
        Some((session, window, pane))
    }

    pub fn list_sessions(&self) -> String {
        let mut lines = self
            .sessions
            .values()
            .map(|session| {
                let attached = self
                    .clients
                    .values()
                    .filter(|client| client.attached_session == Some(session.id))
                    .count();
                let group = session
                    .group
                    .and_then(|group| self.session_groups.get(&group))
                    .map(|group| format!(" group={} size={}", group.name, group.sessions.len()))
                    .unwrap_or_default();
                format!(
                    "{}: {} windows attached={}{}",
                    session.name,
                    session.winlinks.len(),
                    attached,
                    group
                )
            })
            .collect::<Vec<_>>();
        lines.sort();
        lines.join("\n")
    }

    pub fn list_windows(&self, session: SessionId) -> Option<String> {
        let session = self.sessions.get(&session)?;
        let mut lines = Vec::new();
        for winlink in &session.winlinks {
            let winlink = self.winlinks.get(winlink)?;
            let window = self.windows.get(&winlink.window)?;
            let active = if winlink.id == session.active_winlink {
                "*"
            } else {
                "-"
            };
            lines.push(format!(
                "{}{}: {} panes={}",
                active,
                winlink.index,
                window.name,
                window.panes.len()
            ));
        }
        Some(lines.join("\n"))
    }

    pub fn list_panes(&self, window: WindowId) -> Option<String> {
        let window = self.windows.get(&window)?;
        let mut lines = Vec::new();
        for pane in &window.panes {
            let active = if *pane == window.active_pane {
                "*"
            } else {
                "-"
            };
            lines.push(format!(
                "{}{}: window={}",
                active,
                pane.raw(),
                window.id.raw()
            ));
        }
        Some(lines.join("\n"))
    }

    fn create_pane_for_window(&mut self, window: WindowId, cols: u16, rows: u16) -> PaneId {
        let pane = PaneId::new(self.alloc());
        self.panes.insert(
            pane,
            Pane {
                id: pane,
                window,
                rect: Rect::new(0, 0, cols.max(1), rows.max(1)),
                screen: Screen::new(cols, rows),
                terminal: TerminalEngine::new(),
                dead: false,
                remain_on_exit: true,
            },
        );
        pane
    }

    fn recompute_window_layout(&mut self, window: WindowId) {
        self.recompute_window_layout_inner(window);
    }

    fn recompute_window_layout_and_resize_screens(&mut self, window: WindowId) {
        self.recompute_window_layout_inner(window);
    }

    fn recompute_window_layout_inner(&mut self, window: WindowId) {
        let rects = {
            let Some(window_state) = self.windows.get(&window) else {
                return;
            };
            let full = Rect::new(0, 0, window_state.cols.max(1), window_state.rows.max(1));
            if let Some(zoomed) = window_state.zoomed {
                vec![(zoomed, full)]
            } else {
                window_state.layout.rects(full)
            }
        };

        let mut resizes = Vec::new();
        for (pane, rect) in rects {
            if let Some(pane_state) = self.panes.get_mut(&pane) {
                let old = pane_state.rect;
                if old == rect {
                    continue;
                }
                pane_state.rect = rect;
                if old.cols != rect.cols || old.rows != rect.rows {
                    pane_state.screen.resize(rect.cols, rect.rows);
                    resizes.push(PaneResize {
                        pane,
                        old,
                        new: rect,
                    });
                }
            }
        }
        for resize in resizes {
            self.record_pane_resize(resize);
        }
    }

    fn record_pane_resize(&mut self, resize: PaneResize) {
        let entry = self
            .pending_pane_resizes
            .entry(resize.pane)
            .or_insert(resize);
        entry.new = resize.new;
        if entry.old.cols == entry.new.cols && entry.old.rows == entry.new.rows {
            self.pending_pane_resizes.remove(&resize.pane);
        }
    }

    fn next_window_index(&self, session: SessionId) -> Option<u16> {
        let session = self.sessions.get(&session)?;
        let mut index = 0;
        loop {
            let exists = session.winlinks.iter().any(|winlink| {
                self.winlinks
                    .get(winlink)
                    .is_some_and(|winlink| winlink.index == index)
            });
            if !exists {
                return Some(index);
            }
            index += 1;
        }
    }

    fn add_sessions_to_group(&mut self, first: SessionId, second: SessionId) {
        let group = self
            .sessions
            .get(&first)
            .and_then(|session| session.group)
            .unwrap_or_else(|| {
                let id = SessionGroupId::new(self.alloc());
                let name = self
                    .sessions
                    .get(&first)
                    .map(|session| session.name.clone())
                    .unwrap_or_else(|| id.raw().to_string());
                self.session_groups.insert(
                    id,
                    SessionGroup {
                        id,
                        name,
                        sessions: vec![first],
                    },
                );
                if let Some(session) = self.sessions.get_mut(&first) {
                    session.group = Some(id);
                }
                id
            });

        if let Some(session) = self.sessions.get_mut(&second) {
            session.group = Some(group);
        }
        if let Some(group_state) = self.session_groups.get_mut(&group) {
            if !group_state.sessions.contains(&second) {
                group_state.sessions.push(second);
            }
        }
    }

    fn synchronize_group_from_new_window(
        &mut self,
        source_session: SessionId,
        index: u16,
        window: WindowId,
    ) {
        let Some(group) = self
            .sessions
            .get(&source_session)
            .and_then(|session| session.group)
        else {
            return;
        };
        let Some(sessions) = self
            .session_groups
            .get(&group)
            .map(|group| group.sessions.clone())
        else {
            return;
        };

        for session in sessions {
            if session == source_session {
                continue;
            }
            let already_linked = self.sessions.get(&session).is_some_and(|session_state| {
                session_state.winlinks.iter().any(|winlink| {
                    self.winlinks
                        .get(winlink)
                        .is_some_and(|winlink| winlink.index == index)
                })
            });
            if already_linked {
                continue;
            }

            let winlink = WinlinkId::new(self.alloc());
            self.winlinks.insert(
                winlink,
                Winlink {
                    id: winlink,
                    session,
                    index,
                    window,
                },
            );
            if let Some(session_state) = self.sessions.get_mut(&session) {
                session_state.winlinks.push(winlink);
                session_state.winlinks.sort_by_key(|winlink| {
                    self.winlinks
                        .get(winlink)
                        .map(|winlink| winlink.index)
                        .unwrap_or(u16::MAX)
                });
            }
        }
    }

    fn alloc(&mut self) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        id
    }
}

impl Default for ServerState {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::ServerState;
    use crate::SplitDirection;

    #[test]
    fn creates_session_window_winlink_and_pane() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);

        assert!(state.sessions.contains_key(&created.session));
        assert!(state.winlinks.contains_key(&created.winlink));
        assert!(state.windows.contains_key(&created.window));
        assert!(state.panes.contains_key(&created.pane));
        assert_eq!(
            state.active_pane_for_session(created.session),
            Some(created.pane)
        );
    }

    #[test]
    fn creates_and_selects_windows_in_session() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let second = state
            .create_window(created.session, Some("edit".to_string()), 80, 24)
            .unwrap();

        assert_eq!(
            state.active_window_for_session(created.session),
            Some(second.window)
        );
        assert_eq!(state.select_window(created.session, 0), Some(created.pane));
        assert_eq!(
            state.active_window_for_session(created.session),
            Some(created.window)
        );
    }

    #[test]
    fn creates_panes_inside_window() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let second = state.create_pane(created.window, 80, 24).unwrap();

        let window = state.windows.get(&created.window).unwrap();
        assert_eq!(window.panes, vec![created.pane, second]);
        assert_eq!(window.active_pane, second);
    }

    #[test]
    fn split_resizes_existing_pane_screen_to_new_rect() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let original = created.pane;

        let second = state.create_pane(created.window, 80, 24).unwrap();

        let original_pane = state.pane(original).unwrap();
        let second_pane = state.pane(second).unwrap();
        assert_eq!(
            (original_pane.screen.cols(), original_pane.screen.rows()),
            (original_pane.rect.cols, original_pane.rect.rows)
        );
        assert!(original_pane.rect.rows < 24);
        assert_eq!(
            (second_pane.screen.cols(), second_pane.screen.rows()),
            (second_pane.rect.cols, second_pane.rect.rows)
        );
    }

    #[test]
    fn layout_transaction_reports_only_dimension_changes() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 122, 24);
        let second = state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                122,
                24,
            )
            .unwrap();
        let third = state
            .split_pane(
                created.window,
                Some(second),
                SplitDirection::LeftRight,
                122,
                24,
            )
            .unwrap();
        state.take_pending_pane_resizes();

        state.resize_window(created.window, 123, 24).unwrap();
        let resizes = state.take_pending_pane_resizes();

        assert_eq!(resizes.len(), 1);
        assert!(!resizes.iter().any(|resize| resize.pane == created.pane));
        assert!(!resizes.iter().any(|resize| resize.pane == second));
        assert!(resizes.iter().any(|resize| resize.pane == third));
        assert!(resizes.iter().all(|resize| {
            resize.old.cols != resize.new.cols || resize.old.rows != resize.new.rows
        }));
    }

    #[test]
    fn repeated_layout_changes_coalesce_to_the_final_pane_size() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let second = state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                80,
                24,
            )
            .unwrap();
        state.take_pending_pane_resizes();
        let original = state.pane(second).unwrap().rect;

        state.resize_window(created.window, 100, 24).unwrap();
        state.resize_window(created.window, 120, 24).unwrap();
        let resizes = state.take_pending_pane_resizes();
        let second_resize = resizes.iter().find(|resize| resize.pane == second).unwrap();

        assert_eq!(second_resize.old, original);
        assert_eq!(second_resize.new, state.pane(second).unwrap().rect);
    }

    #[test]
    fn unchanged_window_size_produces_no_resize_transaction() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let generation = state.pane(created.pane).unwrap().generation();

        state.resize_window(created.window, 80, 24).unwrap();

        assert!(state.take_pending_pane_resizes().is_empty());
        assert_eq!(state.pane(created.pane).unwrap().generation(), generation);
    }

    #[test]
    fn split_resizes_existing_alternate_screen_panes_to_new_rect() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        state
            .pane_mut(created.pane)
            .unwrap()
            .screen
            .set_alternate(true);

        let _second = state
            .split_pane(
                created.window,
                Some(created.pane),
                SplitDirection::LeftRight,
                80,
                24,
            )
            .unwrap();

        let original_pane = state.pane(created.pane).unwrap();
        assert_eq!(
            (original_pane.screen.cols(), original_pane.screen.rows()),
            (original_pane.rect.cols, original_pane.rect.rows)
        );
    }

    #[test]
    fn killing_one_of_multiple_panes_keeps_session_and_selects_survivor() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let client = state.add_client();
        state.attach_client(client, created.session).unwrap();
        let second = state.create_pane(created.window, 80, 24).unwrap();

        assert_eq!(state.kill_pane(second), Some(vec![second]));
        assert!(state.sessions.contains_key(&created.session));
        assert!(!state.panes.contains_key(&second));
        assert_eq!(
            state.active_pane_for_session(created.session),
            Some(created.pane)
        );
        assert_eq!(state.clients[&client].attached_pane, Some(created.pane));
    }

    #[test]
    fn killing_final_pane_destroys_empty_session_and_detaches_clients() {
        let mut state = ServerState::new();
        let created = state.create_session("test", 80, 24);
        let client = state.add_client();
        state.attach_client(client, created.session).unwrap();

        assert_eq!(state.kill_pane(created.pane), Some(vec![created.pane]));
        assert!(!state.sessions.contains_key(&created.session));
        assert!(!state.windows.contains_key(&created.window));
        assert!(!state.panes.contains_key(&created.pane));
        assert_eq!(state.clients[&client].attached_session, None);
        assert_eq!(state.clients[&client].attached_pane, None);
    }

    #[test]
    fn detach_and_reattach_preserve_complete_authoritative_cell_text() {
        let mut state = ServerState::new();
        let created = state.create_session("persistent", 12, 2);
        let first = state.add_client();
        state.attach_client(first, created.session).unwrap();
        {
            let pane = state.pane_mut(created.pane).unwrap();
            pane.terminal.feed(&mut pane.screen, "e\u{301}x".as_bytes());
        }

        state.detach_client(first);
        state.remove_client(first);
        let second = state.add_client();
        assert_eq!(
            state.attach_client(second, created.session),
            Some(created.pane)
        );

        let line = state
            .pane(created.pane)
            .unwrap()
            .screen
            .grid()
            .line(0)
            .unwrap();
        assert_eq!(line.text(), "e\u{301}x");
        assert_eq!(line.cell(0).unwrap().text().to_string(), "e\u{301}");
    }
}
