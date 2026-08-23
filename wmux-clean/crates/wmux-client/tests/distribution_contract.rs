use std::process::Command;

#[test]
fn package_builds_versioned_client_and_server_binaries() {
    for (binary, name) in [
        (env!("CARGO_BIN_EXE_wmux"), "wmux"),
        (env!("CARGO_BIN_EXE_wmux-server"), "wmux-server"),
    ] {
        let output = Command::new(binary)
            .arg("--version")
            .output()
            .expect("packaged binary starts");
        assert!(output.status.success(), "{name} --version failed");
        assert_eq!(
            String::from_utf8(output.stdout).unwrap(),
            format!("{name} {}\n", env!("CARGO_PKG_VERSION"))
        );
    }
}
