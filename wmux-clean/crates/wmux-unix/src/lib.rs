#![cfg(unix)]

// The private endpoint adapter is consumed by the Unix composition root added
// later in Phase 6. Keep intermediate commits warning-free until that caller
// lands, then remove this allowance.
#[allow(dead_code)]
mod ipc;
#[allow(dead_code)]
mod process;
#[allow(dead_code)]
mod pty;
mod terminal;

pub use terminal::UnixTerminalBackend;
