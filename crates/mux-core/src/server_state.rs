use std::{
    collections::{BTreeMap, VecDeque},
    time::SystemTime,
};

use crate::commands::ParsedCommand;
use crate::ids::{ClientId, JobId, PaneId, PasteBufferId, SessionId, WindowId, WinlinkId};

#[derive(Debug, Default)]
pub struct ServerState {
    pub server_pid: u32,
    pub sessions: BTreeMap<SessionId, Session>,
    pub windows: BTreeMap<WindowId, Window>,
    pub winlinks: BTreeMap<WinlinkId, Winlink>,
    pub panes: BTreeMap<PaneId, Pane>,
    pub clients: BTreeMap<ClientId, Client>,
    pub paste_buffers: BTreeMap<PasteBufferId, Vec<u8>>,
    pub jobs: BTreeMap<JobId, Job>,
    pub messages: Vec<ServerMessage>,
    next_session_id: u32,
    next_window_id: u32,
    next_winlink_id: u32,
    next_pane_id: u32,
    next_client_id: u32,
    next_job_id: u32,
    next_paste_buffer_id: u32,
    next_command_sequence: u64,
}

impl ServerState {
    pub fn new(server_pid: u32) -> Self {
        Self {
            server_pid,
            next_session_id: 1,
            next_window_id: 1,
            next_winlink_id: 1,
            next_pane_id: 1,
            next_client_id: 1,
            next_job_id: 1,
            next_paste_buffer_id: 1,
            next_command_sequence: 1,
            ..Self::default()
        }
    }

    pub fn allocate_session_id(&mut self) -> SessionId {
        let id = SessionId::new(self.next_session_id);
        self.next_session_id += 1;
        id
    }

    pub fn allocate_window_id(&mut self) -> WindowId {
        let id = WindowId::new(self.next_window_id);
        self.next_window_id += 1;
        id
    }

    pub fn allocate_winlink_id(&mut self) -> WinlinkId {
        let id = WinlinkId::new(self.next_winlink_id);
        self.next_winlink_id += 1;
        id
    }

    pub fn allocate_pane_id(&mut self) -> PaneId {
        let id = PaneId::new(self.next_pane_id);
        self.next_pane_id += 1;
        id
    }

    pub fn allocate_client_id(&mut self) -> ClientId {
        let id = ClientId::new(self.next_client_id);
        self.next_client_id += 1;
        id
    }

    pub fn allocate_job_id(&mut self) -> JobId {
        let id = JobId::new(self.next_job_id);
        self.next_job_id += 1;
        id
    }

    pub fn allocate_paste_buffer_id(&mut self) -> PasteBufferId {
        let id = PasteBufferId::new(self.next_paste_buffer_id);
        self.next_paste_buffer_id += 1;
        id
    }

    pub fn register_client(&mut self, pid: u32) -> ClientId {
        let id = self.allocate_client_id();
        self.clients.insert(
            id,
            Client {
                id,
                pid,
                connected_at: SystemTime::now(),
                attached_session: None,
                command_queue: VecDeque::new(),
            },
        );
        self.record_message(format!("client {} connected pid={pid}", id.raw()));
        id
    }

    pub fn remove_client(&mut self, id: ClientId) {
        if self.clients.remove(&id).is_some() {
            self.record_message(format!("client {} disconnected", id.raw()));
        }
    }

    pub fn create_session_with_pane(
        &mut self,
        name: Option<String>,
        pane_title: impl Into<String>,
    ) -> CreatedSession {
        let session_id = self.allocate_session_id();
        let window_id = self.allocate_window_id();
        let winlink_id = self.allocate_winlink_id();
        let pane_id = self.allocate_pane_id();
        let session_name = name.unwrap_or_else(|| session_id.raw().to_string());

        self.sessions.insert(
            session_id,
            Session {
                id: session_id,
                name: session_name.clone(),
                winlinks: vec![winlink_id],
                active_winlink: Some(winlink_id),
            },
        );
        self.windows.insert(
            window_id,
            Window {
                id: window_id,
                name: "0".to_string(),
                panes: vec![pane_id],
                active_pane: Some(pane_id),
            },
        );
        self.winlinks.insert(
            winlink_id,
            Winlink {
                id: winlink_id,
                session_id,
                window_id,
                index: 0,
            },
        );
        self.panes.insert(
            pane_id,
            Pane {
                id: pane_id,
                window_id,
                title: pane_title.into(),
                process_id: None,
            },
        );
        self.record_message(format!(
            "session {session_name} created pane={}",
            pane_id.raw()
        ));

        CreatedSession {
            session_id,
            window_id,
            pane_id,
        }
    }

    pub fn set_pane_process_id(&mut self, pane_id: PaneId, process_id: Option<u32>) {
        if let Some(pane) = self.panes.get_mut(&pane_id) {
            pane.process_id = process_id;
        }
    }

    pub fn find_session(&self, target: Option<&str>) -> Option<SessionId> {
        match target {
            Some(target) => self
                .sessions
                .iter()
                .find(|(id, session)| session.name == target || id.raw().to_string() == target)
                .map(|(id, _)| *id),
            None => self.sessions.keys().next_back().copied(),
        }
    }

    pub fn active_pane_for_session(&self, session_id: SessionId) -> Option<PaneId> {
        let session = self.sessions.get(&session_id)?;
        let winlink_id = session.active_winlink?;
        let winlink = self.winlinks.get(&winlink_id)?;
        let window = self.windows.get(&winlink.window_id)?;
        window.active_pane
    }

    pub fn attach_client_to_session(
        &mut self,
        client_id: ClientId,
        session_id: SessionId,
    ) -> Result<(), ServerStateError> {
        let client = self
            .clients
            .get_mut(&client_id)
            .ok_or(ServerStateError::UnknownClient(client_id))?;
        client.attached_session = Some(session_id);
        self.record_message(format!(
            "client {} attached session={}",
            client_id.raw(),
            session_id.raw()
        ));
        Ok(())
    }

    pub fn detach_client(&mut self, client_id: ClientId) {
        if let Some(client) = self.clients.get_mut(&client_id) {
            client.attached_session = None;
            self.record_message(format!("client {} detached", client_id.raw()));
        }
    }

    pub fn enqueue_client_command(
        &mut self,
        client_id: ClientId,
        raw: String,
        parsed: ParsedCommand,
    ) -> Result<u64, ServerStateError> {
        let sequence = self.next_command_sequence;
        self.next_command_sequence += 1;
        let client = self
            .clients
            .get_mut(&client_id)
            .ok_or(ServerStateError::UnknownClient(client_id))?;
        client.command_queue.push_back(QueuedCommand {
            sequence,
            client_id,
            raw,
            parsed,
        });
        Ok(sequence)
    }

    pub fn pop_next_command(&mut self) -> Option<QueuedCommand> {
        let client_id = self
            .clients
            .iter()
            .filter_map(|(client_id, client)| {
                client
                    .command_queue
                    .front()
                    .map(|command| (*client_id, command.sequence))
            })
            .min_by_key(|(_, sequence)| *sequence)
            .map(|(client_id, _)| client_id)?;

        self.clients
            .get_mut(&client_id)
            .and_then(|client| client.command_queue.pop_front())
    }

    pub fn list_clients(&self, now: SystemTime) -> String {
        if self.clients.is_empty() {
            return "no clients".to_string();
        }

        self.clients
            .iter()
            .map(|(id, client)| {
                let age = now
                    .duration_since(client.connected_at)
                    .map(|duration| duration.as_secs())
                    .unwrap_or(0);
                format!("client {}: pid={} connected={}s", id.raw(), client.pid, age)
            })
            .collect::<Vec<_>>()
            .join("\n")
    }

    pub fn record_message(&mut self, message: impl Into<String>) {
        self.messages.push(ServerMessage {
            created_at: SystemTime::now(),
            message: message.into(),
        });
    }

    pub fn show_messages(&self, now: SystemTime) -> String {
        if self.messages.is_empty() {
            return "no messages".to_string();
        }

        self.messages
            .iter()
            .map(|message| {
                let age = now
                    .duration_since(message.created_at)
                    .map(|duration| duration.as_secs())
                    .unwrap_or(0);
                format!("[{}s ago] {}", age, message.message)
            })
            .collect::<Vec<_>>()
            .join("\n")
    }
}

#[derive(Debug)]
pub struct Session {
    pub id: SessionId,
    pub name: String,
    pub winlinks: Vec<WinlinkId>,
    pub active_winlink: Option<WinlinkId>,
}

#[derive(Debug)]
pub struct Window {
    pub id: WindowId,
    pub name: String,
    pub panes: Vec<PaneId>,
    pub active_pane: Option<PaneId>,
}

#[derive(Debug)]
pub struct Winlink {
    pub id: WinlinkId,
    pub session_id: SessionId,
    pub window_id: WindowId,
    pub index: u32,
}

#[derive(Debug)]
pub struct Pane {
    pub id: PaneId,
    pub window_id: WindowId,
    pub title: String,
    pub process_id: Option<u32>,
}

#[derive(Debug)]
pub struct Client {
    pub id: ClientId,
    pub pid: u32,
    pub connected_at: SystemTime,
    pub attached_session: Option<SessionId>,
    pub command_queue: VecDeque<QueuedCommand>,
}

#[derive(Debug)]
pub struct Job {
    pub id: JobId,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct QueuedCommand {
    pub sequence: u64,
    pub client_id: ClientId,
    pub raw: String,
    pub parsed: ParsedCommand,
}

#[derive(Debug)]
pub struct ServerMessage {
    pub created_at: SystemTime,
    pub message: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ServerStateError {
    UnknownClient(ClientId),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct CreatedSession {
    pub session_id: SessionId,
    pub window_id: WindowId,
    pub pane_id: PaneId,
}

#[cfg(test)]
mod tests {
    use super::ServerState;

    #[test]
    fn creates_session_window_winlink_and_pane() {
        let mut state = ServerState::new(123);

        let created = state.create_session_with_pane(Some("main".to_string()), "shell");

        let session = state.sessions.get(&created.session_id).unwrap();
        assert_eq!(session.name, "main");
        assert_eq!(session.winlinks.len(), 1);

        let winlink = state.winlinks.get(&session.winlinks[0]).unwrap();
        assert_eq!(winlink.session_id, created.session_id);
        assert_eq!(winlink.window_id, created.window_id);

        let window = state.windows.get(&created.window_id).unwrap();
        assert_eq!(window.panes, vec![created.pane_id]);
        assert_eq!(window.active_pane, Some(created.pane_id));

        let pane = state.panes.get(&created.pane_id).unwrap();
        assert_eq!(pane.window_id, created.window_id);
        assert_eq!(pane.title, "shell");
    }

    #[test]
    fn detach_and_disconnect_do_not_destroy_pane_state() {
        let mut state = ServerState::new(123);
        let client_id = state.register_client(456);
        let created = state.create_session_with_pane(Some("main".to_string()), "shell");

        state
            .attach_client_to_session(client_id, created.session_id)
            .unwrap();
        state.detach_client(client_id);
        state.remove_client(client_id);

        assert!(!state.clients.contains_key(&client_id));
        assert!(state.sessions.contains_key(&created.session_id));
        assert!(state.windows.contains_key(&created.window_id));
        assert!(state.panes.contains_key(&created.pane_id));
        assert_eq!(
            state.active_pane_for_session(created.session_id),
            Some(created.pane_id)
        );
    }

    #[test]
    fn attach_can_find_existing_session_by_name_or_id() {
        let mut state = ServerState::new(123);
        let created = state.create_session_with_pane(Some("main".to_string()), "shell");

        assert_eq!(state.find_session(Some("main")), Some(created.session_id));
        assert_eq!(
            state.find_session(Some(&created.session_id.raw().to_string())),
            Some(created.session_id)
        );
        assert_eq!(
            state.active_pane_for_session(created.session_id),
            Some(created.pane_id)
        );
    }
}
