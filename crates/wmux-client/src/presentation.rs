use std::{
    io,
    sync::{mpsc, Arc},
    thread,
};

use tokio::sync::mpsc as async_mpsc;
use wmux_platform::{PlatformResult, TerminalBackend};

pub(crate) struct PresentationRequest {
    pub(crate) sequence: u64,
    pub(crate) bytes: Vec<u8>,
    pub(crate) synchronized_output: bool,
}

pub(crate) struct PresentationCompletion {
    pub(crate) sequence: u64,
    pub(crate) result: PlatformResult<()>,
}

pub(crate) struct PresentationWorker {
    requests: Option<mpsc::SyncSender<PresentationRequest>>,
    thread: Option<thread::JoinHandle<()>>,
}

impl PresentationWorker {
    pub(crate) fn spawn(
        terminal: Arc<dyn TerminalBackend>,
        completions: async_mpsc::Sender<PresentationCompletion>,
    ) -> io::Result<Self> {
        let (requests, receiver) = mpsc::sync_channel::<PresentationRequest>(1);
        let thread = thread::Builder::new()
            .name("wmux-presentation".to_string())
            .spawn(move || {
                while let Ok(request) = receiver.recv() {
                    let result = terminal
                        .write_render_transaction(&request.bytes, request.synchronized_output);
                    if completions
                        .blocking_send(PresentationCompletion {
                            sequence: request.sequence,
                            result,
                        })
                        .is_err()
                    {
                        break;
                    }
                }
            })?;
        Ok(Self {
            requests: Some(requests),
            thread: Some(thread),
        })
    }

    pub(crate) fn present(&self, request: PresentationRequest) -> io::Result<()> {
        let Some(requests) = self.requests.as_ref() else {
            return Err(io::Error::new(
                io::ErrorKind::BrokenPipe,
                "terminal presentation worker stopped",
            ));
        };
        requests.try_send(request).map_err(|error| match error {
            mpsc::TrySendError::Full(_) => io::Error::new(
                io::ErrorKind::WouldBlock,
                "terminal presentation request already queued",
            ),
            mpsc::TrySendError::Disconnected(_) => io::Error::new(
                io::ErrorKind::BrokenPipe,
                "terminal presentation worker stopped",
            ),
        })
    }
}

impl Drop for PresentationWorker {
    fn drop(&mut self) {
        self.requests.take();
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{PresentationRequest, PresentationWorker};
    use std::sync::{Arc, Mutex};
    use std::time::Duration;
    use wmux_platform::{
        PlatformError, PlatformErrorKind, PlatformResult, TerminalBackend, TerminalInput,
        TerminalModeGuard, TerminalSize,
    };

    struct ScriptedTerminal {
        writes: Arc<Mutex<Vec<(Vec<u8>, bool)>>>,
        fail: bool,
    }

    impl TerminalBackend for ScriptedTerminal {
        fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
            Ok(Box::new(()))
        }

        fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
            Ok(None)
        }

        fn write_output(&self, _bytes: &[u8]) -> PlatformResult<()> {
            Ok(())
        }

        fn write_render_transaction(
            &self,
            bytes: &[u8],
            synchronized_output: bool,
        ) -> PlatformResult<()> {
            self.writes
                .lock()
                .unwrap()
                .push((bytes.to_vec(), synchronized_output));
            if self.fail {
                Err(PlatformError::new(
                    PlatformErrorKind::Disconnected,
                    "present test frame",
                    "scripted terminal failure",
                ))
            } else {
                Ok(())
            }
        }

        fn write_clipboard_text(&self, _text: &str) -> PlatformResult<()> {
            Ok(())
        }

        fn size(&self) -> PlatformResult<TerminalSize> {
            Ok(TerminalSize::new(80, 24))
        }
    }

    #[tokio::test]
    async fn reports_success_only_after_the_exact_frame_is_written() {
        let writes = Arc::new(Mutex::new(Vec::new()));
        let terminal = Arc::new(ScriptedTerminal {
            writes: Arc::clone(&writes),
            fail: false,
        });
        let (completion_tx, mut completion_rx) = tokio::sync::mpsc::channel(1);
        let worker = PresentationWorker::spawn(terminal, completion_tx).unwrap();

        worker
            .present(PresentationRequest {
                sequence: 42,
                bytes: b"frame".to_vec(),
                synchronized_output: true,
            })
            .unwrap();

        let completion = tokio::time::timeout(Duration::from_secs(1), completion_rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(completion.sequence, 42);
        assert!(completion.result.is_ok());
        assert_eq!(
            writes.lock().unwrap().as_slice(),
            [(b"frame".to_vec(), true)]
        );
    }

    #[tokio::test]
    async fn reports_failure_for_the_matching_frame_without_hiding_the_error() {
        let writes = Arc::new(Mutex::new(Vec::new()));
        let terminal = Arc::new(ScriptedTerminal {
            writes: Arc::clone(&writes),
            fail: true,
        });
        let (completion_tx, mut completion_rx) = tokio::sync::mpsc::channel(1);
        let worker = PresentationWorker::spawn(terminal, completion_tx).unwrap();

        worker
            .present(PresentationRequest {
                sequence: 7,
                bytes: b"broken".to_vec(),
                synchronized_output: false,
            })
            .unwrap();

        let completion = tokio::time::timeout(Duration::from_secs(1), completion_rx.recv())
            .await
            .unwrap()
            .unwrap();
        assert_eq!(completion.sequence, 7);
        assert_eq!(
            completion.result.unwrap_err().kind(),
            PlatformErrorKind::Disconnected
        );
        assert_eq!(
            writes.lock().unwrap().as_slice(),
            [(b"broken".to_vec(), false)]
        );
    }
}
