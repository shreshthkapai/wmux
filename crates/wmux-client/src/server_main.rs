fn main() -> std::io::Result<()> {
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--version")) {
        println!("wmux-server {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }

    run()
}

#[cfg(windows)]
fn run() -> std::io::Result<()> {
    let platform = wmux_windows::platform::WindowsServerPlatform::current_user()
        .map_err(wmux_platform::PlatformError::into_io)?;
    wmux_server::run_with_platform(Box::new(platform))
}

#[cfg(unix)]
fn run() -> std::io::Result<()> {
    let platform = wmux_unix::UnixServerPlatform::current_user()
        .map_err(wmux_platform::PlatformError::into_io)?;
    wmux_server::run_with_platform(Box::new(platform))
}

#[cfg(not(any(windows, unix)))]
fn run() -> std::io::Result<()> {
    Err(std::io::Error::new(
        std::io::ErrorKind::Unsupported,
        "wmux has no platform backend for this operating system",
    ))
}
