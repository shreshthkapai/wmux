fn main() -> std::io::Result<()> {
    #[cfg(windows)]
    {
        let platform = wmux_windows::platform::WindowsServerPlatform::current_user()
            .map_err(wmux_platform::PlatformError::into_io)?;
        wmux_server::run_with_platform(Box::new(platform))
    }
    #[cfg(not(windows))]
    {
        Err(std::io::Error::new(
            std::io::ErrorKind::Unsupported,
            "the Unix backend is implemented in Phase 6",
        ))
    }
}
