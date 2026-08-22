#![cfg(unix)]

use std::{
    fs, io,
    path::{Path, PathBuf},
    process,
    sync::mpsc,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use wmux_platform::{BoxedIpcStream, ClientTransport};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    FRAME_HEADER_LEN, VERSION,
};
use wmux_unix::{UnixClientTransport, UnixServerPlatform};

struct TestDirectory(PathBuf);

impl TestDirectory {
    fn new() -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("test clock is after Unix epoch")
            .as_nanos();
        Self(std::env::temp_dir().join(format!("wmux-native-lifecycle-{}-{nonce}", process::id())))
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TestDirectory {
    fn drop(&mut self) {
        if self
            .0
            .file_name()
            .is_some_and(|name| name.to_string_lossy().starts_with("wmux-native-lifecycle-"))
        {
            let _ = fs::remove_dir_all(&self.0);
        }
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn real_unix_lifecycle_preserves_detached_output_and_cleans_up() {
    let runtime = TestDirectory::new();
    let platform = UnixServerPlatform::from_runtime_directory(runtime.path().to_path_buf())
        .expect("server platform is constructed");
    let transport = UnixClientTransport::from_runtime_directory(runtime.path().to_path_buf())
        .expect("client transport is constructed");
    let (server_result_tx, server_result_rx) = mpsc::sync_channel(1);
    let server = thread::spawn(move || {
        let result = wmux_server::run_with_platform_and_config(
            Box::new(platform),
            wmux_config::WmuxConfig::default(),
        );
        let _ = server_result_tx.send(result);
    });

    let mut attached = connect_and_handshake(&transport).await;
    command(&mut attached, "new-session -s native").await;
    write_message(
        &mut attached,
        Message::Input(b"printf 'WMUX_%s' TYPED\n".to_vec()),
    )
    .await;
    wait_for_output(&mut attached, b"WMUX_TYPED").await;

    command(&mut attached, "split-window").await;
    write_message(
        &mut attached,
        Message::Resize {
            cols: 100,
            rows: 30,
        },
    )
    .await;
    write_message(
        &mut attached,
        Message::Input(b"set -- $(stty size); printf 'WMUX_SIZE_%sx%s' \"$1\" \"$2\"\n".to_vec()),
    )
    .await;
    // The default top/bottom split gives the active (second) pane the
    // remainder after the one-row separator: (30 - 1) / 2 rounded up.
    wait_for_output(&mut attached, b"WMUX_SIZE_15x100").await;

    write_message(
        &mut attached,
        Message::Input(b"(sleep 1; printf 'WMUX_%s' BACKGROUND) &\n".to_vec()),
    )
    .await;
    write_message(&mut attached, Message::Detach).await;
    wait_for_command_ok(&mut attached).await;
    drop(attached);
    tokio::time::sleep(Duration::from_millis(1_200)).await;

    let mut reattached = connect_and_handshake(&transport).await;
    command(&mut reattached, "attach-session -t native").await;
    wait_for_output(&mut reattached, b"WMUX_BACKGROUND").await;
    command(&mut reattached, "kill-pane").await;

    let mut controller = connect_and_handshake(&transport).await;
    command(&mut controller, "kill-session -t native").await;
    wait_for_shutdown(&mut reattached).await;
    command(&mut controller, "kill-server").await;
    drop(controller);

    let server_result = server_result_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("server exits after kill-server");
    server_result.expect("server exits cleanly");
    server.join().expect("server thread joins");
    assert!(!runtime.path().join("wmux.sock").exists());
    assert!(!runtime.path().join("wmux.lock").exists());
}

async fn connect_and_handshake(transport: &UnixClientTransport) -> BoxedIpcStream {
    let deadline = Instant::now() + Duration::from_secs(5);
    let mut stream = loop {
        match transport.connect().await {
            Ok(stream) => break stream,
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
            Err(error) => panic!("client could not connect: {error}"),
        }
    };
    write_message(
        &mut stream,
        Message::Hello {
            version: VERSION,
            pid: process::id(),
            capabilities: TerminalCapabilities::default(),
        },
    )
    .await;
    assert!(matches!(
        read_message(&mut stream).await,
        Some(Message::HelloOk {
            version: VERSION,
            ..
        })
    ));
    stream
}

async fn command(stream: &mut BoxedIpcStream, command: &str) {
    write_message(stream, Message::Command(command.to_string())).await;
    wait_for_command_ok(stream).await;
}

async fn wait_for_command_ok(stream: &mut BoxedIpcStream) {
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match read_message(stream).await {
                Some(Message::CommandOk(_)) => return,
                Some(Message::CommandErr(error)) => panic!("command failed: {error}"),
                Some(Message::Output(_) | Message::Clipboard(_)) => {}
                Some(message) => panic!("unexpected command response: {message:?}"),
                None => panic!("server closed before command completion"),
            }
        }
    })
    .await
    .expect("command completes");
}

async fn wait_for_output(stream: &mut BoxedIpcStream, marker: &[u8]) {
    let mut output = Vec::new();
    let outcome = tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match read_message(stream).await {
                Some(Message::Output(bytes)) => {
                    output.extend_from_slice(&bytes);
                    if output.windows(marker.len()).any(|window| window == marker) {
                        return;
                    }
                }
                Some(Message::CommandErr(error)) => panic!("server command failed: {error}"),
                Some(Message::CommandOk(_) | Message::Clipboard(_)) => {}
                Some(message) => panic!("unexpected output message: {message:?}"),
                None => panic!("server closed before expected pane output"),
            }
        }
    })
    .await;
    if outcome.is_err() {
        panic!(
            "expected pane output {:?} arrives; received {:?}",
            String::from_utf8_lossy(marker),
            String::from_utf8_lossy(&output),
        )
    }
}

async fn wait_for_shutdown(stream: &mut BoxedIpcStream) {
    tokio::time::timeout(Duration::from_secs(5), async {
        loop {
            match read_message(stream).await {
                Some(Message::Shutdown) | None => return,
                Some(Message::Output(_) | Message::Clipboard(_) | Message::CommandOk(_)) => {}
                Some(Message::CommandErr(error)) => panic!("shutdown command failed: {error}"),
                Some(message) => panic!("unexpected shutdown message: {message:?}"),
            }
        }
    })
    .await
    .expect("attached client observes session destruction");
}

async fn read_message(stream: &mut BoxedIpcStream) -> Option<Message> {
    let mut header = [0_u8; FRAME_HEADER_LEN];
    match stream.read_exact(&mut header).await {
        Ok(_) => {}
        Err(error) if error.kind() == io::ErrorKind::UnexpectedEof => return None,
        Err(error) => panic!("read protocol header: {error}"),
    }
    let (tag, payload_len) = decode_frame_header(&header).expect("protocol header is valid");
    let mut payload = vec![0_u8; payload_len];
    stream
        .read_exact(&mut payload)
        .await
        .expect("protocol payload is complete");
    Some(decode_frame_payload_owned(tag, payload).expect("protocol payload is valid"))
}

async fn write_message(stream: &mut BoxedIpcStream, message: Message) {
    let frame = EncodedFrame::from_message(message);
    stream
        .write_all(frame.header())
        .await
        .expect("protocol header writes");
    stream
        .write_all(frame.payload())
        .await
        .expect("protocol payload writes");
    stream.flush().await.expect("protocol frame flushes");
}
