#![cfg(windows)]

use std::{
    io,
    sync::mpsc,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use windows_sys::Win32::{
    Foundation::{CloseHandle, WAIT_OBJECT_0},
    System::Threading::{OpenProcess, WaitForSingleObject, PROCESS_SYNCHRONIZE},
};
use wmux_platform::{BoxedIpcStream, ClientTransport};
use wmux_protocol::{
    decode_frame_header, decode_frame_payload_owned, EncodedFrame, Message, TerminalCapabilities,
    FRAME_HEADER_LEN, VERSION,
};
use wmux_windows::platform::{WindowsClientTransport, WindowsServerPlatform};

#[test]
fn native_lifecycle_endpoints_are_explicitly_isolated() {
    let instance = format!("phase8-native-{}", std::process::id());
    let other_instance = format!("{instance}-other");

    let _server = WindowsServerPlatform::for_instance(&instance)
        .expect("isolated Windows server platform is constructed");
    let client = WindowsClientTransport::for_instance(&instance)
        .expect("matching Windows client transport is constructed");
    let matching = WindowsClientTransport::for_instance(&instance)
        .expect("second matching Windows client transport is constructed");
    let other = WindowsClientTransport::for_instance(&other_instance)
        .expect("different Windows client transport is constructed");

    assert_eq!(client.endpoint(), matching.endpoint());
    assert_ne!(client.endpoint(), other.endpoint());
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn real_windows_lifecycle_survives_client_loss_and_cleans_process_trees() {
    let nonce = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("test clock is after Unix epoch")
        .as_nanos();
    let instance = format!("phase8-native-{}-{nonce}", std::process::id());
    run_lifecycle(&instance).await;
    run_restart(&instance).await;
}

async fn run_lifecycle(instance: &str) {
    let platform = WindowsServerPlatform::for_instance(instance)
        .expect("isolated Windows server platform is constructed");
    let transport = WindowsClientTransport::for_instance(instance)
        .expect("isolated Windows client transport is constructed");
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
        Message::Input(b"echo WMUX_NATIVE_TYPED\r".to_vec()),
    )
    .await;
    wait_for_output(&mut attached, b"WMUX_NATIVE_TYPED").await;

    drop(attached);
    let mut verifier = connect_and_handshake(&transport).await;
    let sessions = command(&mut verifier, "list-sessions").await;
    assert!(sessions.contains("native"));
    command(&mut verifier, "attach-session -t native").await;

    write_message(
        &mut verifier,
        Message::Input(b"ping -n 2 127.0.0.1 >nul & echo WMUX_BACKGROUND\r".to_vec()),
    )
    .await;
    write_message(&mut verifier, Message::Detach).await;
    wait_for_command_ok(&mut verifier).await;
    drop(verifier);
    tokio::time::sleep(Duration::from_millis(1_250)).await;

    let mut reattached = connect_and_handshake(&transport).await;
    let early = command(&mut reattached, "attach-session -t native").await;
    if !early
        .as_bytes()
        .windows(b"WMUX_BACKGROUND".len())
        .any(|window| window == b"WMUX_BACKGROUND")
    {
        wait_for_output(&mut reattached, b"WMUX_BACKGROUND").await;
    }

    // Keep the complete marker out of echoed input: renderer diffs may omit
    // cells that are unchanged between the command and its output.
    let child_command = concat!(
        "powershell -NoLogo -NoProfile -Command \"",
        "$p=Start-Process powershell -ArgumentList ",
        "'-NoLogo','-NoProfile','-Command','Start-Sleep -Seconds 60' -PassThru; ",
        "Write-Output ('WMUX_' + 'CHILD_PID_' + $p.Id); Wait-Process -Id $p.Id\"\r"
    );
    write_message(
        &mut reattached,
        Message::Input(child_command.as_bytes().to_vec()),
    )
    .await;
    // Renderer output is a diff; the unchanged `WMUX_` prefix can be omitted.
    let child_pid = wait_for_pid_marker(&mut reattached, b"CHILD_PID_").await;
    assert!(process_is_running(child_pid));

    let mut controller = connect_and_handshake(&transport).await;
    command(&mut controller, "kill-session -t native").await;
    wait_for_shutdown(&mut reattached).await;
    wait_for_process_exit(child_pid).await;
    command(&mut controller, "kill-server").await;
    drop(controller);

    let server_result = server_result_rx
        .recv_timeout(Duration::from_secs(10))
        .expect("server exits after kill-server");
    server_result.expect("server exits cleanly");
    server.join().expect("server thread joins");
}

async fn run_restart(instance: &str) {
    let platform = WindowsServerPlatform::for_instance(instance)
        .expect("Windows endpoint is reusable after shutdown");
    let transport = WindowsClientTransport::for_instance(instance)
        .expect("matching restart transport is constructed");
    let (server_result_tx, server_result_rx) = mpsc::sync_channel(1);
    let server = thread::spawn(move || {
        let result = wmux_server::run_with_platform_and_config(
            Box::new(platform),
            wmux_config::WmuxConfig::default(),
        );
        let _ = server_result_tx.send(result);
    });

    let mut client = connect_and_handshake(&transport).await;
    command(&mut client, "new-session -d -s restarted").await;
    assert!(command(&mut client, "list-sessions")
        .await
        .contains("restarted"));
    command(&mut client, "kill-server").await;
    drop(client);

    server_result_rx
        .recv_timeout(Duration::from_secs(10))
        .expect("restarted server exits")
        .expect("restarted server exits cleanly");
    server.join().expect("restarted server thread joins");
}

async fn connect_and_handshake(transport: &WindowsClientTransport) -> BoxedIpcStream {
    let deadline = Instant::now() + Duration::from_secs(10);
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
            pid: std::process::id(),
            capabilities: TerminalCapabilities::default(),
            current_dir: std::env::current_dir()
                .unwrap()
                .to_string_lossy()
                .into_owned(),
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

async fn command(stream: &mut BoxedIpcStream, command: &str) -> String {
    write_message(stream, Message::Command(command.to_string())).await;
    wait_for_command_ok(stream).await
}

async fn wait_for_command_ok(stream: &mut BoxedIpcStream) -> String {
    tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match read_message(stream).await {
                Some(Message::CommandOk(message)) => return message,
                Some(Message::CommandErr(error)) => panic!("command failed: {error}"),
                Some(Message::Output { .. } | Message::Clipboard(_)) => {}
                Some(message) => panic!("unexpected command response: {message:?}"),
                None => panic!("server closed before command completion"),
            }
        }
    })
    .await
    .expect("command completes")
}

async fn wait_for_output(stream: &mut BoxedIpcStream, marker: &[u8]) {
    let mut output = Vec::new();
    let outcome = tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match read_message(stream).await {
                Some(Message::Output { bytes, .. }) => {
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
            "expected pane output {:?}; received {:?}",
            String::from_utf8_lossy(marker),
            String::from_utf8_lossy(&output),
        )
    }
}

async fn wait_for_pid_marker(stream: &mut BoxedIpcStream, marker: &[u8]) -> u32 {
    let mut output = Vec::new();
    tokio::time::timeout(Duration::from_secs(15), async {
        loop {
            match read_message(stream).await {
                Some(Message::Output { bytes, .. }) => {
                    output.extend_from_slice(&bytes);
                    if let Some(pid) = parse_pid_marker(&output, marker) {
                        return pid;
                    }
                }
                Some(Message::CommandOk(_) | Message::Clipboard(_)) => {}
                Some(Message::CommandErr(error)) => panic!("server command failed: {error}"),
                Some(message) => panic!("unexpected PID response: {message:?}"),
                None => panic!("server closed before child PID was reported"),
            }
        }
    })
    .await
    .unwrap_or_else(|_| {
        panic!(
            "child PID marker timed out; output was {:?}",
            String::from_utf8_lossy(&output)
        )
    })
}

fn parse_pid_marker(output: &[u8], marker: &[u8]) -> Option<u32> {
    output
        .windows(marker.len())
        .enumerate()
        .filter(|(_, window)| *window == marker)
        .find_map(|(offset, _)| {
            let start = offset + marker.len();
            let digits = output[start..]
                .iter()
                .copied()
                .take_while(u8::is_ascii_digit)
                .collect::<Vec<_>>();
            (!digits.is_empty())
                .then(|| std::str::from_utf8(&digits).ok()?.parse().ok())
                .flatten()
        })
}

async fn wait_for_shutdown(stream: &mut BoxedIpcStream) {
    tokio::time::timeout(Duration::from_secs(10), async {
        loop {
            match read_message(stream).await {
                Some(Message::Shutdown) | None => return,
                Some(Message::Output { .. } | Message::Clipboard(_) | Message::CommandOk(_)) => {}
                Some(Message::CommandErr(error)) => panic!("shutdown command failed: {error}"),
                Some(message) => panic!("unexpected shutdown message: {message:?}"),
            }
        }
    })
    .await
    .expect("attached client observes session destruction");
}

fn process_is_running(pid: u32) -> bool {
    let process = unsafe { OpenProcess(PROCESS_SYNCHRONIZE, 0, pid) };
    if process.is_null() {
        return false;
    }
    let status = unsafe { WaitForSingleObject(process, 0) };
    unsafe { CloseHandle(process) };
    status != WAIT_OBJECT_0
}

async fn wait_for_process_exit(pid: u32) {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        if !process_is_running(pid) {
            return;
        }
        if Instant::now() >= deadline {
            panic!("descendant process {pid} leaked");
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
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
