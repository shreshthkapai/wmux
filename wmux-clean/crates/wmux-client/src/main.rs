use std::io;

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

#[cfg(windows)]
fn run() -> io::Result<()> {
    use std::sync::Arc;

    let transport = wmux_windows::platform::WindowsClientTransport::current_user()
        .map_err(wmux_platform::PlatformError::into_io)?;
    let terminal = wmux_windows::platform::WindowsTerminalBackend;
    wmux_client::run_with_platform(Arc::new(transport), Arc::new(terminal))
}

#[cfg(not(windows))]
fn run() -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "the Unix client composition root is implemented in phase 6",
    ))
}
