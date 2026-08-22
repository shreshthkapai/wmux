fn main() -> std::io::Result<()> {
    #[cfg(windows)]
    {
        let platform = wmux_windows::platform::WindowsServerPlatform::current_user()
            .map_err(wmux_platform::PlatformError::into_io)?;
        wmux_server::run_with_platform(Box::new(platform))
    }
    #[cfg(unix)]
    {
        let platform = wmux_unix::UnixServerPlatform::current_user()
            .map_err(wmux_platform::PlatformError::into_io)?;
        wmux_server::run_with_platform(Box::new(platform))
    }
    #[cfg(not(any(windows, unix)))]
    {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "wmux has no platform backend for this operating system",
        ))
    }
}
