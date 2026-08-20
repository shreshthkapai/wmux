//! Windows console mode helpers for attached clients.

use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers};
use mux_platform::terminal::TerminalSize;
use std::{
    collections::VecDeque,
    ffi::c_void,
    io::{self, Write},
    sync::{Mutex, OnceLock},
    time::Duration,
};

#[derive(Debug, Default)]
pub struct WindowsConsoleBackend;

#[derive(Debug)]
pub struct ConsoleModeGuard {
    input: Handle,
    output: Handle,
    original_input_mode: Dword,
    original_output_mode: Dword,
}

impl ConsoleModeGuard {
    pub fn enter() -> io::Result<Self> {
        let input = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
        let output = unsafe { GetStdHandle(STD_OUTPUT_HANDLE) };
        if input.is_null() || input == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        if output.is_null() || output == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }

        let original_input_mode = get_console_mode(input)?;
        let original_output_mode = get_console_mode(output)?;

        crossterm::terminal::enable_raw_mode().map_err(crossterm_error)?;

        let input_mode = ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS;
        let output_mode = original_output_mode
            | ENABLE_PROCESSED_OUTPUT
            | ENABLE_WRAP_AT_EOL_OUTPUT
            | ENABLE_VIRTUAL_TERMINAL_PROCESSING;

        set_console_mode(input, input_mode)?;
        set_console_mode(output, output_mode)?;

        Ok(Self {
            input,
            output,
            original_input_mode,
            original_output_mode,
        })
    }
}

impl Drop for ConsoleModeGuard {
    fn drop(&mut self) {
        let _ = crossterm::terminal::disable_raw_mode();
        let _ = set_console_mode(self.input, self.original_input_mode);
        let _ = set_console_mode(self.output, self.original_output_mode);
    }
}

pub fn query_terminal_size() -> io::Result<TerminalSize> {
    let output = unsafe { GetStdHandle(STD_OUTPUT_HANDLE) };
    if output.is_null() || output == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }

    let mut info = ConsoleScreenBufferInfo::default();
    let ok = unsafe { GetConsoleScreenBufferInfo(output, &mut info) };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }

    let columns = (info.srWindow.right - info.srWindow.left + 1).max(1) as u16;
    let rows = (info.srWindow.bottom - info.srWindow.top + 1).max(1) as u16;
    Ok(TerminalSize::cells(columns, rows))
}

pub fn read_console_input(buffer: &mut [u8]) -> io::Result<usize> {
    if buffer.is_empty() {
        return Ok(0);
    }

    loop {
        let drained = drain_pending_input(buffer);
        if drained > 0 {
            return Ok(drained);
        }

        match event::read().map_err(crossterm_error)? {
            Event::Key(key_event) if key_event.kind == KeyEventKind::Press => {
                if let Some(bytes) = encode_crossterm_key(key_event) {
                    queue_pending_input(bytes);
                }
            }
            Event::Paste(text) => {
                let mut bytes = Vec::with_capacity(text.len() + 12);
                bytes.extend_from_slice(b"\x1b[200~");
                bytes.extend_from_slice(text.as_bytes());
                bytes.extend_from_slice(b"\x1b[201~");
                queue_pending_input(bytes);
            }
            Event::Resize(..) | Event::Mouse(_) | Event::FocusGained | Event::FocusLost => {
                return Ok(0);
            }
            _ => {}
        }
    }
}

pub fn has_console_input() -> io::Result<bool> {
    if !pending_input()
        .lock()
        .expect("pending input lock")
        .is_empty()
    {
        return Ok(true);
    }
    event::poll(Duration::from_millis(0)).map_err(crossterm_error)
}

fn pending_input() -> &'static Mutex<VecDeque<u8>> {
    static PENDING_INPUT: OnceLock<Mutex<VecDeque<u8>>> = OnceLock::new();
    PENDING_INPUT.get_or_init(|| Mutex::new(VecDeque::new()))
}

fn queue_pending_input(bytes: Vec<u8>) {
    pending_input()
        .lock()
        .expect("pending input lock")
        .extend(bytes);
}

fn drain_pending_input(buffer: &mut [u8]) -> usize {
    let mut pending = pending_input().lock().expect("pending input lock");
    let count = buffer.len().min(pending.len());
    for slot in buffer.iter_mut().take(count) {
        *slot = pending.pop_front().expect("pending input byte");
    }
    count
}

fn crossterm_error(error: std::io::Error) -> io::Error {
    io::Error::new(error.kind(), error)
}

fn encode_crossterm_key(event: KeyEvent) -> Option<Vec<u8>> {
    let mut modifiers = event.modifiers;
    let has_ctrl = modifiers.contains(KeyModifiers::CONTROL);
    let has_alt = modifiers.contains(KeyModifiers::ALT);

    if event.code == KeyCode::Backspace {
        if has_ctrl {
            return Some(vec![0x17]);
        }
        if has_alt {
            return Some(vec![0x1b, 0x08]);
        }
        return Some(vec![0x08]);
    }

    let (raw_bytes, is_char_key) = match event.code {
        KeyCode::Char(ch) => {
            modifiers.remove(KeyModifiers::SHIFT);
            let is_altgr = has_ctrl && has_alt && !ch.is_ascii_control();
            if is_altgr {
                modifiers.remove(KeyModifiers::CONTROL);
                modifiers.remove(KeyModifiers::ALT);
            }

            let bytes = if is_altgr {
                utf8_bytes(ch)
            } else if has_ctrl && has_alt && ch.is_ascii_alphabetic() {
                vec![0x1b, (ch.to_ascii_lowercase() as u8) & 0x1f]
            } else if has_ctrl && ch.is_ascii_alphabetic() {
                vec![(ch.to_ascii_lowercase() as u8) & 0x1f]
            } else if has_alt {
                let mut bytes = vec![0x1b];
                bytes.extend_from_slice(&utf8_bytes(ch));
                bytes
            } else {
                utf8_bytes(ch)
            };
            (bytes, true)
        }
        KeyCode::Enter => (vec![0x0d], false),
        KeyCode::Tab => (vec![0x09], false),
        KeyCode::BackTab => {
            modifiers.insert(KeyModifiers::SHIFT);
            (b"\x1b[Z".to_vec(), false)
        }
        KeyCode::Backspace => unreachable!("backspace is handled before modifier encoding"),
        KeyCode::Esc => (vec![0x1b], false),
        KeyCode::Left => (b"\x1b[D".to_vec(), false),
        KeyCode::Right => (b"\x1b[C".to_vec(), false),
        KeyCode::Up => (b"\x1b[A".to_vec(), false),
        KeyCode::Down => (b"\x1b[B".to_vec(), false),
        KeyCode::Home => (b"\x1b[H".to_vec(), false),
        KeyCode::End => (b"\x1b[F".to_vec(), false),
        KeyCode::PageUp => (b"\x1b[5~".to_vec(), false),
        KeyCode::PageDown => (b"\x1b[6~".to_vec(), false),
        KeyCode::Delete => (b"\x1b[3~".to_vec(), false),
        KeyCode::Insert => (b"\x1b[2~".to_vec(), false),
        KeyCode::F(number) => (function_key_bytes(number), false),
        KeyCode::Null => return Some(vec![0x00]),
        _ => return None,
    };

    Some(encode_modified_non_char_key(
        raw_bytes,
        modifiers,
        has_ctrl,
        has_alt,
        is_char_key,
    ))
}

fn utf8_bytes(ch: char) -> Vec<u8> {
    let mut buffer = [0; 4];
    ch.encode_utf8(&mut buffer).as_bytes().to_vec()
}

fn function_key_bytes(number: u8) -> Vec<u8> {
    match number {
        1 => b"\x1bOP".to_vec(),
        2 => b"\x1bOQ".to_vec(),
        3 => b"\x1bOR".to_vec(),
        4 => b"\x1bOS".to_vec(),
        5 => b"\x1b[15~".to_vec(),
        6 => b"\x1b[17~".to_vec(),
        7 => b"\x1b[18~".to_vec(),
        8 => b"\x1b[19~".to_vec(),
        9 => b"\x1b[20~".to_vec(),
        10 => b"\x1b[21~".to_vec(),
        11 => b"\x1b[23~".to_vec(),
        12 => b"\x1b[24~".to_vec(),
        _ => Vec::new(),
    }
}

fn encode_modified_non_char_key(
    raw_bytes: Vec<u8>,
    modifiers: KeyModifiers,
    has_ctrl: bool,
    has_alt: bool,
    is_char_key: bool,
) -> Vec<u8> {
    let has_shift = modifiers.contains(KeyModifiers::SHIFT);
    let has_any_modifier = has_alt || has_ctrl || has_shift;
    if !has_any_modifier || raw_bytes.is_empty() || is_char_key {
        return raw_bytes;
    }

    let modifier_code = 1
        + if has_shift { 1 } else { 0 }
        + if has_alt { 2 } else { 0 }
        + if has_ctrl { 4 } else { 0 };
    let first = raw_bytes.first().copied();
    let second = raw_bytes.get(1).copied();
    let third = raw_bytes.get(2).copied();
    let last = raw_bytes.last().copied();

    match (first, second, third, last) {
        (Some(byte), _, _, _) if byte != 0x1b => {
            if modifier_code == 3 {
                let mut bytes = vec![0x1b];
                bytes.extend_from_slice(&raw_bytes);
                bytes
            } else {
                format!("\x1b[{};{}u", byte, modifier_code).into_bytes()
            }
        }
        (Some(0x1b), Some(b'['), Some(final_byte), _) if raw_bytes.len() == 3 => {
            format!("\x1b[1;{}{}", modifier_code, final_byte as char).into_bytes()
        }
        (Some(0x1b), Some(b'['), _, Some(b'~')) if raw_bytes.len() >= 4 => {
            let num_part = &raw_bytes[2..raw_bytes.len() - 1];
            let num_str = std::str::from_utf8(num_part).unwrap_or("1");
            format!("\x1b[{};{}~", num_str, modifier_code).into_bytes()
        }
        (Some(0x1b), Some(b'O'), Some(final_byte), _) if raw_bytes.len() == 3 => {
            format!("\x1b[1;{}{}", modifier_code, final_byte as char).into_bytes()
        }
        _ => raw_bytes,
    }
}

pub fn flush_console_input() -> io::Result<()> {
    let input = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
    if input.is_null() || input == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }

    let ok = unsafe { FlushConsoleInputBuffer(input) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

pub fn write_console_output(bytes: &[u8]) -> io::Result<()> {
    let mut stdout = io::stdout().lock();
    stdout.write_all(bytes)?;
    stdout.flush()
}

pub fn reset_console_modes() -> io::Result<()> {
    let input = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
    let output = unsafe { GetStdHandle(STD_OUTPUT_HANDLE) };
    if input.is_null() || input == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    if output.is_null() || output == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }

    let input_mode = ENABLE_PROCESSED_INPUT
        | ENABLE_LINE_INPUT
        | ENABLE_ECHO_INPUT
        | ENABLE_INSERT_MODE
        | ENABLE_EXTENDED_FLAGS
        | ENABLE_QUICK_EDIT_MODE;
    let output_mode =
        ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    set_console_mode(input, input_mode)?;
    set_console_mode(output, output_mode)?;
    Ok(())
}

fn get_console_mode(handle: Handle) -> io::Result<Dword> {
    let mut mode = 0;
    let ok = unsafe { GetConsoleMode(handle, &mut mode) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(mode)
    }
}

fn set_console_mode(handle: Handle, mode: Dword) -> io::Result<()> {
    let ok = unsafe { SetConsoleMode(handle, mode) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

type Handle = *mut c_void;
type Bool = i32;
type Dword = u32;

const INVALID_HANDLE_VALUE: Handle = !0_usize as Handle;
const STD_INPUT_HANDLE: Dword = -10_i32 as Dword;
const STD_OUTPUT_HANDLE: Dword = -11_i32 as Dword;
const ENABLE_PROCESSED_INPUT: Dword = 0x0001;
const ENABLE_LINE_INPUT: Dword = 0x0002;
const ENABLE_ECHO_INPUT: Dword = 0x0004;
const ENABLE_WINDOW_INPUT: Dword = 0x0008;
const ENABLE_MOUSE_INPUT: Dword = 0x0010;
const ENABLE_INSERT_MODE: Dword = 0x0020;
const ENABLE_QUICK_EDIT_MODE: Dword = 0x0040;
const ENABLE_EXTENDED_FLAGS: Dword = 0x0080;
const ENABLE_PROCESSED_OUTPUT: Dword = 0x0001;
const ENABLE_WRAP_AT_EOL_OUTPUT: Dword = 0x0002;
const ENABLE_VIRTUAL_TERMINAL_PROCESSING: Dword = 0x0004;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Coord {
    x: i16,
    y: i16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct SmallRect {
    left: i16,
    top: i16,
    right: i16,
    bottom: i16,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
#[allow(non_snake_case)]
struct ConsoleScreenBufferInfo {
    dwSize: Coord,
    dwCursorPosition: Coord,
    wAttributes: u16,
    srWindow: SmallRect,
    dwMaximumWindowSize: Coord,
}

#[link(name = "kernel32")]
extern "system" {
    fn GetStdHandle(nStdHandle: Dword) -> Handle;
    fn GetConsoleMode(hConsoleHandle: Handle, lpMode: *mut Dword) -> Bool;
    fn SetConsoleMode(hConsoleHandle: Handle, dwMode: Dword) -> Bool;
    fn FlushConsoleInputBuffer(hConsoleInput: Handle) -> Bool;
    fn GetConsoleScreenBufferInfo(
        hConsoleOutput: Handle,
        lpConsoleScreenBufferInfo: *mut ConsoleScreenBufferInfo,
    ) -> Bool;
}

#[cfg(test)]
mod tests {
    use super::encode_crossterm_key;
    use crossterm::event::{KeyCode, KeyEvent, KeyEventKind, KeyEventState, KeyModifiers};

    fn key(code: KeyCode, modifiers: KeyModifiers) -> KeyEvent {
        KeyEvent {
            code,
            modifiers,
            kind: KeyEventKind::Press,
            state: KeyEventState::empty(),
        }
    }

    #[test]
    fn encodes_backspace_for_windows_conpty_cmd_editing() {
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Backspace, KeyModifiers::empty())),
            Some(vec![0x08])
        );
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Backspace, KeyModifiers::CONTROL)),
            Some(vec![0x17])
        );
    }

    #[test]
    fn encodes_characters_and_control_characters() {
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Char('a'), KeyModifiers::empty())),
            Some(b"a".to_vec())
        );
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Char('c'), KeyModifiers::CONTROL)),
            Some(vec![0x03])
        );
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Char('w'), KeyModifiers::CONTROL)),
            Some(vec![0x17])
        );
    }

    #[test]
    fn encodes_modified_special_keys_like_zellij() {
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Left, KeyModifiers::empty())),
            Some(b"\x1b[D".to_vec())
        );
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Left, KeyModifiers::CONTROL)),
            Some(b"\x1b[1;5D".to_vec())
        );
        assert_eq!(
            encode_crossterm_key(key(KeyCode::Delete, KeyModifiers::CONTROL)),
            Some(b"\x1b[3;5~".to_vec())
        );
    }
}
