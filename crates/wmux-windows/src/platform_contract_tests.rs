use crate::platform::{classify_windows_io_error, WindowsClientTransport};
use std::io;
use wmux_platform::{ClientTransport, PlatformErrorKind};

#[test]
fn busy_named_pipe_error_is_classified_inside_windows_adapter() {
    let error = classify_windows_io_error("connect IPC", io::Error::from_raw_os_error(231));
    assert_eq!(error.kind(), PlatformErrorKind::Busy);
}

#[test]
fn current_user_transport_exposes_only_semantic_endpoint() {
    let transport = WindowsClientTransport::current_user().unwrap();
    let display = transport.endpoint().display();
    let endpoint_suffix = display
        .strip_prefix(r"\\.\pipe\wmux-")
        .expect("endpoint uses the wmux product prefix");
    assert!(endpoint_suffix.starts_with("S-1-"));
}
