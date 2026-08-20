//! Future Unix backend.
//!
//! This crate exists to keep the workspace boundary honest while Windows is
//! built first.

pub mod ipc;
pub mod process;
pub mod pty;
pub mod signals;
pub mod terminal;

#[derive(Debug, Default)]
pub struct UnixPlatform;
