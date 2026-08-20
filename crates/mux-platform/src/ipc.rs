use mux_protocol::ProtocolMessage;

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ConnectionId(pub u64);

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ServerEndpoint {
    pub name: String,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PeerIdentity {
    pub user: Option<String>,
    pub process_id: Option<u32>,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum IpcEvent {
    ClientConnected {
        connection: ConnectionId,
        peer: PeerIdentity,
    },
    Message {
        connection: ConnectionId,
        message: ProtocolMessage,
    },
    Disconnected {
        connection: ConnectionId,
    },
    Error {
        connection: Option<ConnectionId>,
        message: String,
    },
}

pub trait IpcBackend {
    type Error;
    type Listener;

    fn listen(&mut self, endpoint: ServerEndpoint) -> Result<Self::Listener, Self::Error>;
    fn connect(&mut self, endpoint: ServerEndpoint) -> Result<ConnectionId, Self::Error>;
    fn send(
        &mut self,
        connection: ConnectionId,
        message: ProtocolMessage,
    ) -> Result<(), Self::Error>;
    fn close(&mut self, connection: ConnectionId) -> Result<(), Self::Error>;
}
