//! Windows platform backend.
//!
//! This crate will own ConPTY, CreateProcessW, Job Objects, named pipes, and
//! Windows console mode integration. It intentionally exposes only semantic
//! backend types to the rest of the workspace.

pub mod conpty;
pub mod console;
pub mod job;
pub mod named_pipe;
pub mod overlapped;
pub mod process;
pub mod terminal_features;

#[derive(Debug, Default)]
pub struct WindowsPlatform;
