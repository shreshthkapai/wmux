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

#[cfg(unix)]
fn run() -> io::Result<()> {
    use std::sync::Arc;

    let transport = wmux_unix::UnixClientTransport::current_user()
        .map_err(wmux_platform::PlatformError::into_io)?;
    let terminal = wmux_unix::UnixTerminalBackend;
    wmux_client::run_with_platform(Arc::new(transport), Arc::new(terminal))
}

#[cfg(not(any(windows, unix)))]
fn run() -> io::Result<()> {
    Err(io::Error::new(
        io::ErrorKind::Unsupported,
        "wmux has no platform backend for this operating system",
    ))
}
