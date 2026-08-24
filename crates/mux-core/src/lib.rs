//! OS-neutral multiplexer core.
//!
//! This crate owns wmux semantics: sessions, windows, panes, layouts,
//! screens, command queues, options, key tables, paste buffers, and hooks.
//! It must not import platform APIs.

pub mod commands;
pub mod grid;
pub mod ids;
pub mod input;
pub mod redraw;
pub mod screen;
pub mod server_state;
pub mod vt_parser;

pub use commands::{CommandParseError, CommandParser, ParsedCommand};
pub use grid::{Grid, GridCell, GridLine};
pub use ids::{ClientId, JobId, PaneId, PasteBufferId, SessionId, WindowId, WinlinkId};
pub use input::{encode_pane_input, parse_terminal_input, TerminalInput, TerminalKey};
pub use redraw::{render_diff, render_full, render_full_into_state, RenderState};
pub use screen::Screen;
pub use server_state::{
    Client, CreatedSession, Job, Pane, QueuedCommand, ServerMessage, ServerState, ServerStateError,
    Session, Window, Winlink,
};
pub use vt_parser::VtParser;
