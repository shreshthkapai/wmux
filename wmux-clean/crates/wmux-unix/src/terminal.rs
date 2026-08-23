use crossterm::event::{
    Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseButton as CrosstermMouseButton,
    MouseEvent as CrosstermMouseEvent, MouseEventKind as CrosstermMouseEventKind,
};
use nix::{
    sys::termios::{self, SetArg, Termios},
    unistd::dup,
};
use std::{
    io::{self, Write},
    os::fd::{AsRawFd, BorrowedFd, OwnedFd},
};
use wmux_platform::{
    MouseButton, MouseEvent, MouseEventKind, MouseModifiers, PlatformError, PlatformResult,
    TerminalBackend, TerminalInput, TerminalKeyCode, TerminalKeyEvent, TerminalKeyModifiers,
    TerminalModeGuard, TerminalSize,
};

#[derive(Default)]
pub struct UnixTerminalBackend;

impl TerminalBackend for UnixTerminalBackend {
    fn enter(&self) -> PlatformResult<Box<dyn TerminalModeGuard>> {
        let input = unsafe { BorrowedFd::borrow_raw(libc::STDIN_FILENO) };
        enter_raw_mode(input)
            .map(|guard| Box::new(guard) as Box<dyn TerminalModeGuard>)
            .map_err(|error| PlatformError::from_io("enter Unix terminal mode", error))
    }

    fn read_input(&self) -> PlatformResult<Option<TerminalInput>> {
        loop {
            let event = crossterm::event::read()
                .map_err(|error| PlatformError::from_io("read Unix terminal input", error))?;
            if let Some(input) = normalize_event(event) {
                return Ok(Some(input));
            }
        }
    }

    fn write_output(&self, bytes: &[u8]) -> PlatformResult<()> {
        let mut output = io::stdout().lock();
        output
            .write_all(bytes)
            .and_then(|()| output.flush())
            .map_err(|error| PlatformError::from_io("write Unix terminal output", error))
    }

    fn write_render_transaction(
        &self,
        bytes: &[u8],
        synchronized_output: bool,
    ) -> PlatformResult<()> {
        let mut output = io::stdout().lock();
        write_render_transaction_to(&mut output, bytes, synchronized_output)
            .and_then(|()| output.flush())
            .map_err(|error| PlatformError::from_io("write Unix render transaction", error))
    }

    fn write_clipboard_text(&self, text: &str) -> PlatformResult<()> {
        let mut output = io::stdout().lock();
        write_osc52_to(&mut output, text)
            .and_then(|()| output.flush())
            .map_err(|error| PlatformError::from_io("write Unix clipboard", error))
    }

    fn size(&self) -> PlatformResult<TerminalSize> {
        let output = unsafe { BorrowedFd::borrow_raw(libc::STDOUT_FILENO) };
        terminal_size(output)
            .map_err(|error| PlatformError::from_io("query Unix terminal size", error))
    }
}

struct UnixTerminalGuard {
    input: OwnedFd,
    saved: Termios,
}

impl Drop for UnixTerminalGuard {
    fn drop(&mut self) {
        let _ = termios::tcsetattr(&self.input, SetArg::TCSANOW, &self.saved);
    }
}

fn enter_raw_mode(input: BorrowedFd<'_>) -> io::Result<UnixTerminalGuard> {
    let input = dup(input).map_err(nix_error)?;
    let saved = termios::tcgetattr(&input).map_err(nix_error)?;
    let mut raw = saved.clone();
    termios::cfmakeraw(&mut raw);
    termios::tcsetattr(&input, SetArg::TCSANOW, &raw).map_err(nix_error)?;
    Ok(UnixTerminalGuard { input, saved })
}

fn nix_error(error: nix::errno::Errno) -> io::Error {
    io::Error::from_raw_os_error(error as i32)
}

fn normalize_event(event: Event) -> Option<TerminalInput> {
    match event {
        Event::Key(key) if key.kind != KeyEventKind::Release => {
            normalize_key(key).map(TerminalInput::Key)
        }
        Event::Paste(text) => Some(TerminalInput::Paste(text)),
        Event::Mouse(event) => Some(TerminalInput::Mouse(normalize_mouse(event))),
        Event::Resize(cols, rows) => Some(TerminalInput::Resize(TerminalSize::new(
            cols.max(1),
            rows.max(1),
        ))),
        _ => None,
    }
}

fn normalize_mouse(event: CrosstermMouseEvent) -> MouseEvent {
    let (kind, button) = match event.kind {
        CrosstermMouseEventKind::Down(button) => (MouseEventKind::Down, mouse_button(button)),
        CrosstermMouseEventKind::Up(button) => (MouseEventKind::Up, mouse_button(button)),
        CrosstermMouseEventKind::Drag(button) => (MouseEventKind::Drag, mouse_button(button)),
        CrosstermMouseEventKind::Moved => (MouseEventKind::Move, MouseButton::None),
        CrosstermMouseEventKind::ScrollUp => (MouseEventKind::ScrollUp, MouseButton::None),
        CrosstermMouseEventKind::ScrollDown => (MouseEventKind::ScrollDown, MouseButton::None),
        CrosstermMouseEventKind::ScrollLeft => (MouseEventKind::ScrollLeft, MouseButton::None),
        CrosstermMouseEventKind::ScrollRight => (MouseEventKind::ScrollRight, MouseButton::None),
    };
    let mut modifiers = 0;
    if event.modifiers.contains(KeyModifiers::SHIFT) {
        modifiers |= MouseModifiers::SHIFT;
    }
    if event.modifiers.contains(KeyModifiers::ALT) {
        modifiers |= MouseModifiers::ALT;
    }
    if event.modifiers.contains(KeyModifiers::CONTROL) {
        modifiers |= MouseModifiers::CONTROL;
    }
    MouseEvent {
        kind,
        button,
        modifiers: MouseModifiers::new(modifiers),
        column: event.column,
        row: event.row,
    }
}

fn mouse_button(button: CrosstermMouseButton) -> MouseButton {
    match button {
        CrosstermMouseButton::Left => MouseButton::Left,
        CrosstermMouseButton::Middle => MouseButton::Middle,
        CrosstermMouseButton::Right => MouseButton::Right,
    }
}

fn normalize_key(key: KeyEvent) -> Option<TerminalKeyEvent> {
    let raw = encode_key_bytes(key)?;
    let mut modifiers = terminal_key_modifiers(key.modifiers);
    let code = match key.code {
        KeyCode::Char(ch) if ch.is_ascii_uppercase() => {
            if !key.modifiers.contains(KeyModifiers::CONTROL) {
                modifiers =
                    TerminalKeyModifiers::new(modifiers.bits() | TerminalKeyModifiers::SHIFT);
            }
            TerminalKeyCode::Char(ch.to_ascii_lowercase())
        }
        KeyCode::Char(ch) => TerminalKeyCode::Char(ch),
        KeyCode::Left => TerminalKeyCode::Left,
        KeyCode::Right => TerminalKeyCode::Right,
        KeyCode::Up => TerminalKeyCode::Up,
        KeyCode::Down => TerminalKeyCode::Down,
        KeyCode::Home => TerminalKeyCode::Home,
        KeyCode::End => TerminalKeyCode::End,
        KeyCode::PageUp => TerminalKeyCode::PageUp,
        KeyCode::PageDown => TerminalKeyCode::PageDown,
        KeyCode::Backspace => TerminalKeyCode::Backspace,
        KeyCode::Delete => TerminalKeyCode::Delete,
        KeyCode::Insert => TerminalKeyCode::Insert,
        KeyCode::Enter => TerminalKeyCode::Enter,
        KeyCode::Tab => TerminalKeyCode::Tab,
        KeyCode::BackTab => TerminalKeyCode::BackTab,
        KeyCode::Esc => TerminalKeyCode::Escape,
        KeyCode::F(number) if (1..=24).contains(&number) => TerminalKeyCode::Function(number),
        _ => return None,
    };
    Some(TerminalKeyEvent {
        code,
        modifiers,
        raw,
    })
}

fn terminal_key_modifiers(modifiers: KeyModifiers) -> TerminalKeyModifiers {
    let mut bits = 0;
    if modifiers.contains(KeyModifiers::SHIFT) {
        bits |= TerminalKeyModifiers::SHIFT;
    }
    if modifiers.contains(KeyModifiers::ALT) {
        bits |= TerminalKeyModifiers::ALT;
    }
    if modifiers.contains(KeyModifiers::CONTROL) {
        bits |= TerminalKeyModifiers::CONTROL;
    }
    if modifiers.contains(KeyModifiers::SUPER) {
        bits |= TerminalKeyModifiers::SUPER;
    }
    TerminalKeyModifiers::new(bits)
}

fn encode_key_bytes(key: KeyEvent) -> Option<Vec<u8>> {
    let control = key.modifiers.contains(KeyModifiers::CONTROL);
    let mut bytes = match key.code {
        KeyCode::Backspace => vec![if control { 0x08 } else { 0x7f }],
        KeyCode::Enter => vec![b'\r'],
        KeyCode::Tab => vec![b'\t'],
        KeyCode::BackTab => b"\x1b[Z".to_vec(),
        KeyCode::Esc => vec![0x1b],
        KeyCode::Left => b"\x1b[D".to_vec(),
        KeyCode::Right => b"\x1b[C".to_vec(),
        KeyCode::Up => b"\x1b[A".to_vec(),
        KeyCode::Down => b"\x1b[B".to_vec(),
        KeyCode::Delete => b"\x1b[3~".to_vec(),
        KeyCode::Home => b"\x1b[H".to_vec(),
        KeyCode::End => b"\x1b[F".to_vec(),
        KeyCode::PageUp => b"\x1b[5~".to_vec(),
        KeyCode::PageDown => b"\x1b[6~".to_vec(),
        KeyCode::Insert => b"\x1b[2~".to_vec(),
        KeyCode::F(number) => function_key(number)?.to_vec(),
        KeyCode::Char(ch) if control => encode_control_character(ch)?,
        KeyCode::Char(ch) => {
            let mut buffer = [0; 4];
            ch.encode_utf8(&mut buffer).as_bytes().to_vec()
        }
        KeyCode::Null if control => vec![0],
        _ => return None,
    };
    if key.modifiers.contains(KeyModifiers::ALT) && !bytes.starts_with(b"\x1b") {
        bytes.insert(0, 0x1b);
    }
    Some(bytes)
}

fn encode_control_character(ch: char) -> Option<Vec<u8>> {
    if ch.is_ascii_alphabetic() {
        return Some(vec![(ch.to_ascii_lowercase() as u8) & 0x1f]);
    }
    match ch {
        ' ' | '@' => Some(vec![0x00]),
        '[' => Some(vec![0x1b]),
        '\\' => Some(vec![0x1c]),
        ']' => Some(vec![0x1d]),
        '^' => Some(vec![0x1e]),
        '_' => Some(vec![0x1f]),
        '?' => Some(vec![0x7f]),
        _ => None,
    }
}

fn function_key(number: u8) -> Option<&'static [u8]> {
    match number {
        1 => Some(b"\x1bOP"),
        2 => Some(b"\x1bOQ"),
        3 => Some(b"\x1bOR"),
        4 => Some(b"\x1bOS"),
        5 => Some(b"\x1b[15~"),
        6 => Some(b"\x1b[17~"),
        7 => Some(b"\x1b[18~"),
        8 => Some(b"\x1b[19~"),
        9 => Some(b"\x1b[20~"),
        10 => Some(b"\x1b[21~"),
        11 => Some(b"\x1b[23~"),
        12 => Some(b"\x1b[24~"),
        _ => None,
    }
}

fn write_render_transaction_to(
    writer: &mut impl Write,
    bytes: &[u8],
    synchronized_output: bool,
) -> io::Result<()> {
    if bytes.is_empty() {
        return Ok(());
    }
    if synchronized_output {
        writer.write_all(b"\x1b[?2026h")?;
    }
    writer.write_all(bytes)?;
    if synchronized_output {
        writer.write_all(b"\x1b[?2026l")?;
    }
    Ok(())
}

fn write_osc52_to(writer: &mut impl Write, text: &str) -> io::Result<()> {
    const MAX_CLIPBOARD_TEXT_BYTES: usize = 1024 * 1024;
    if text.len() > MAX_CLIPBOARD_TEXT_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "clipboard text exceeds one MiB",
        ));
    }
    writer.write_all(b"\x1b]52;c;")?;
    writer.write_all(encode_base64(text.as_bytes()).as_bytes())?;
    writer.write_all(b"\x07")
}

fn terminal_size(input: BorrowedFd<'_>) -> io::Result<TerminalSize> {
    let mut size = std::mem::MaybeUninit::<libc::winsize>::zeroed();
    let result = unsafe { libc::ioctl(input.as_raw_fd(), libc::TIOCGWINSZ, size.as_mut_ptr()) };
    if result != 0 {
        return Err(io::Error::last_os_error());
    }
    let size = unsafe { size.assume_init() };
    Ok(TerminalSize::new(size.ws_col.max(1), size.ws_row.max(1)))
}

fn encode_base64(bytes: &[u8]) -> String {
    const TABLE: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut result = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let first = chunk[0];
        let second = *chunk.get(1).unwrap_or(&0);
        let third = *chunk.get(2).unwrap_or(&0);
        result.push(char::from(TABLE[(first >> 2) as usize]));
        result.push(char::from(
            TABLE[(((first & 0b11) << 4) | (second >> 4)) as usize],
        ));
        result.push(if chunk.len() > 1 {
            char::from(TABLE[(((second & 0b1111) << 2) | (third >> 6)) as usize])
        } else {
            '='
        });
        result.push(if chunk.len() > 2 {
            char::from(TABLE[(third & 0b11_1111) as usize])
        } else {
            '='
        });
    }
    result
}

#[cfg(test)]
mod tests {
    use super::{
        enter_raw_mode, normalize_event, terminal_size, write_osc52_to, write_render_transaction_to,
    };
    use crossterm::event::{
        Event, KeyCode, KeyEvent, KeyModifiers, MouseButton as CrosstermMouseButton,
        MouseEvent as CrosstermMouseEvent, MouseEventKind as CrosstermMouseEventKind,
    };
    use nix::{
        pty::{openpty, Winsize},
        sys::termios::{self, LocalFlags},
    };
    use std::os::fd::AsFd;
    use wmux_platform::{
        MouseButton, MouseEvent, MouseEventKind, MouseModifiers, TerminalInput, TerminalKeyCode,
        TerminalKeyEvent, TerminalKeyModifiers, TerminalSize,
    };

    fn assert_send_sync<T: Send + Sync>() {}

    #[test]
    fn unix_terminal_backend_satisfies_the_frozen_threading_contract() {
        assert_send_sync::<super::UnixTerminalBackend>();
    }

    #[test]
    fn raw_mode_guard_restores_exact_termios_on_drop() {
        let pty = openpty(None, None).expect("test PTY opens");
        let saved = termios::tcgetattr(&pty.slave).expect("slave termios is readable");

        let guard = enter_raw_mode(pty.slave.as_fd()).expect("raw mode enters");
        let raw = termios::tcgetattr(&pty.slave).expect("raw termios is readable");
        assert!(!raw.local_flags.contains(LocalFlags::ICANON));
        assert!(!raw.local_flags.contains(LocalFlags::ECHO));
        drop(guard);

        let restored = termios::tcgetattr(&pty.slave).expect("restored termios is readable");
        assert_eq!(restored.input_flags, saved.input_flags);
        assert_eq!(restored.output_flags, saved.output_flags);
        assert_eq!(restored.control_flags, saved.control_flags);
        assert_eq!(restored.local_flags, saved.local_flags);
        assert_eq!(restored.control_chars, saved.control_chars);
        #[cfg(target_os = "linux")]
        assert_eq!(restored.line_discipline, saved.line_discipline);
        assert_eq!(
            termios::cfgetispeed(&restored),
            termios::cfgetispeed(&saved)
        );
        assert_eq!(
            termios::cfgetospeed(&restored),
            termios::cfgetospeed(&saved)
        );
    }

    #[test]
    fn semantic_keys_keep_identity_modifiers_and_raw_bytes() {
        let input = normalize_event(Event::Key(KeyEvent::new(
            KeyCode::Char('B'),
            KeyModifiers::CONTROL,
        )));

        assert_eq!(
            input,
            Some(TerminalInput::Key(TerminalKeyEvent {
                code: TerminalKeyCode::Char('b'),
                modifiers: TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                raw: vec![0x02],
            }))
        );
    }

    #[test]
    fn bracketed_paste_remains_a_distinct_semantic_event() {
        assert_eq!(
            normalize_event(Event::Paste("line one\nline two".to_string())),
            Some(TerminalInput::Paste("line one\nline two".to_string()))
        );
    }

    #[test]
    fn mouse_events_keep_cell_coordinates_and_modifiers() {
        let input = normalize_event(Event::Mouse(CrosstermMouseEvent {
            kind: CrosstermMouseEventKind::Down(CrosstermMouseButton::Left),
            column: 7,
            row: 3,
            modifiers: KeyModifiers::ALT,
        }));

        assert_eq!(
            input,
            Some(TerminalInput::Mouse(MouseEvent {
                kind: MouseEventKind::Down,
                button: MouseButton::Left,
                modifiers: MouseModifiers::new(MouseModifiers::ALT),
                column: 7,
                row: 3,
            }))
        );
    }

    #[test]
    fn resize_events_clamp_zero_dimensions_to_one_cell() {
        assert_eq!(
            normalize_event(Event::Resize(0, 0)),
            Some(TerminalInput::Resize(TerminalSize::new(1, 1)))
        );
    }

    #[test]
    fn fixed_and_modified_keys_encode_expected_application_bytes() {
        let cases = [
            (
                KeyEvent::new(KeyCode::Up, KeyModifiers::NONE),
                TerminalKeyCode::Up,
                TerminalKeyModifiers::default(),
                b"\x1b[A".to_vec(),
            ),
            (
                KeyEvent::new(KeyCode::Backspace, KeyModifiers::CONTROL),
                TerminalKeyCode::Backspace,
                TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                vec![0x08],
            ),
            (
                KeyEvent::new(KeyCode::F(12), KeyModifiers::ALT),
                TerminalKeyCode::Function(12),
                TerminalKeyModifiers::new(TerminalKeyModifiers::ALT),
                b"\x1b[24~".to_vec(),
            ),
            (
                KeyEvent::new(KeyCode::Char('['), KeyModifiers::CONTROL),
                TerminalKeyCode::Char('['),
                TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                vec![0x1b],
            ),
            (
                KeyEvent::new(KeyCode::Char('\u{03bb}'), KeyModifiers::ALT),
                TerminalKeyCode::Char('\u{03bb}'),
                TerminalKeyModifiers::new(TerminalKeyModifiers::ALT),
                vec![0x1b, 0xce, 0xbb],
            ),
        ];

        for (key, code, modifiers, raw) in cases {
            assert_eq!(
                normalize_event(Event::Key(key)),
                Some(TerminalInput::Key(TerminalKeyEvent {
                    code,
                    modifiers,
                    raw,
                }))
            );
        }
    }

    #[test]
    fn render_transaction_is_one_synchronized_write() {
        let mut output = Vec::new();

        write_render_transaction_to(&mut output, b"frame", true)
            .expect("synchronized frame writes");

        assert_eq!(output, b"\x1b[?2026hframe\x1b[?2026l");
    }

    #[test]
    fn osc52_clipboard_is_encoded_as_one_terminal_transaction() {
        let mut output = Vec::new();

        write_osc52_to(&mut output, "hi").expect("clipboard transaction writes");

        assert_eq!(output, b"\x1b]52;c;aGk=\x07");
    }

    #[test]
    fn osc52_clipboard_rejects_more_than_one_mibibyte_before_writing() {
        let mut output = Vec::new();
        let oversized = "x".repeat(1_048_577);

        let error = write_osc52_to(&mut output, &oversized)
            .expect_err("oversized clipboard text is rejected");

        assert_eq!(error.kind(), std::io::ErrorKind::InvalidInput);
        assert!(output.is_empty());
    }

    #[test]
    fn terminal_size_is_clamped_to_one_cell() {
        let pty = openpty(
            Some(&Winsize {
                ws_row: 0,
                ws_col: 0,
                ws_xpixel: 0,
                ws_ypixel: 0,
            }),
            None,
        )
        .expect("test PTY opens");

        assert_eq!(
            terminal_size(pty.slave.as_fd()).expect("terminal size is readable"),
            TerminalSize::new(1, 1)
        );
    }
}
