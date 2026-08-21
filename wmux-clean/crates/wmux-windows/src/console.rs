use crossterm::event::{
    self, Event, KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseButton as CrosstermButton,
    MouseEvent as CrosstermMouseEvent, MouseEventKind as CrosstermMouseEventKind,
};
use std::{
    ffi::c_void,
    io::{self, Write},
    os::windows::ffi::OsStringExt,
    ptr, thread,
    time::Duration,
};
use wmux_platform::{
    MouseButton, MouseEvent, MouseEventKind, MouseModifiers, TerminalInput, TerminalKeyCode,
    TerminalKeyEvent, TerminalKeyModifiers, TerminalSize,
};

pub type ConsoleInput = TerminalInput;

#[derive(Debug)]
pub struct ConsoleGuard {
    input: Handle,
    output: Handle,
    input_mode: Dword,
    output_mode: Dword,
}

// Console handles are process-wide kernel handles. The guard owns only saved
// mode values and may restore them from any thread; it never closes the handles.
unsafe impl Send for ConsoleGuard {}

impl ConsoleGuard {
    pub fn enter() -> io::Result<Self> {
        let input = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
        let output = unsafe { GetStdHandle(STD_OUTPUT_HANDLE) };
        if input.is_null() || input == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }
        if output.is_null() || output == INVALID_HANDLE_VALUE {
            return Err(io::Error::last_os_error());
        }

        let input_mode = get_mode(input)?;
        let output_mode = get_mode(output)?;

        // Match zellij's native Windows path: request window and mouse input
        // while explicitly clearing Quick Edit, line input, echo, and
        // processed input. Quick Edit otherwise intercepts mouse records.
        set_mode(
            input,
            ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_EXTENDED_FLAGS,
        )?;
        set_mode(
            output,
            output_mode
                | ENABLE_PROCESSED_OUTPUT
                | ENABLE_WRAP_AT_EOL_OUTPUT
                | ENABLE_VIRTUAL_TERMINAL_PROCESSING,
        )?;

        Ok(Self {
            input,
            output,
            input_mode,
            output_mode,
        })
    }
}

impl Drop for ConsoleGuard {
    fn drop(&mut self) {
        let _ = set_mode(self.input, self.input_mode);
        let _ = set_mode(self.output, self.output_mode);
    }
}

pub fn read_input() -> io::Result<Option<ConsoleInput>> {
    loop {
        match event::read()? {
            Event::Key(key) if key.kind != KeyEventKind::Release => {
                if is_clipboard_paste_key(key) {
                    match read_clipboard_text() {
                        Ok(Some(text)) => return Ok(Some(ConsoleInput::Paste(text))),
                        Ok(None) | Err(_) => continue,
                    }
                }
                if let Some(bytes) = encode_key_event(key) {
                    return Ok(Some(ConsoleInput::Key(bytes)));
                }
            }
            Event::Paste(text) => return Ok(Some(ConsoleInput::Paste(text))),
            Event::Mouse(event) => return Ok(Some(ConsoleInput::Mouse(mouse_event(event)))),
            Event::Resize(cols, rows) => {
                return Ok(Some(ConsoleInput::Resize(TerminalSize::new(
                    cols.max(1),
                    rows.max(1),
                ))))
            }
            _ => {}
        }
    }
}

fn mouse_event(event: CrosstermMouseEvent) -> MouseEvent {
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

fn mouse_button(button: CrosstermButton) -> MouseButton {
    match button {
        CrosstermButton::Left => MouseButton::Left,
        CrosstermButton::Middle => MouseButton::Middle,
        CrosstermButton::Right => MouseButton::Right,
    }
}

fn is_clipboard_paste_key(key: KeyEvent) -> bool {
    if !key.modifiers.contains(KeyModifiers::CONTROL) {
        return false;
    }

    matches!(key.code, KeyCode::Char(ch) if ch.eq_ignore_ascii_case(&'v'))
}

fn read_clipboard_text() -> io::Result<Option<String>> {
    let _clipboard = ClipboardGuard::open_with_retry()?;
    let handle = unsafe { GetClipboardData(CF_UNICODETEXT) };
    if handle.is_null() {
        return Ok(None);
    }

    let ptr = unsafe { GlobalLock(handle) as *const u16 };
    if ptr.is_null() {
        return Err(io::Error::last_os_error());
    }
    let _memory = GlobalMemoryGuard(handle);

    let mut len = 0;
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
        }
    }
    let wide = unsafe { std::slice::from_raw_parts(ptr, len) };
    Ok(Some(
        std::ffi::OsString::from_wide(wide)
            .to_string_lossy()
            .into_owned(),
    ))
}

pub fn write_clipboard_text(text: &str) -> io::Result<()> {
    let _clipboard = ClipboardGuard::open_with_retry()?;
    if unsafe { EmptyClipboard() } == 0 {
        return Err(io::Error::last_os_error());
    }
    let wide = text
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect::<Vec<_>>();
    let bytes = wide.len() * std::mem::size_of::<u16>();
    let handle = unsafe { GlobalAlloc(GMEM_MOVEABLE, bytes) };
    if handle.is_null() {
        return Err(io::Error::last_os_error());
    }
    let pointer = unsafe { GlobalLock(handle) as *mut u16 };
    if pointer.is_null() {
        unsafe { GlobalFree(handle) };
        return Err(io::Error::last_os_error());
    }
    unsafe {
        pointer.copy_from_nonoverlapping(wide.as_ptr(), wide.len());
        GlobalUnlock(handle);
    }
    if unsafe { SetClipboardData(CF_UNICODETEXT, handle) }.is_null() {
        unsafe { GlobalFree(handle) };
        return Err(io::Error::last_os_error());
    }
    Ok(())
}

struct ClipboardGuard;

impl ClipboardGuard {
    fn open_with_retry() -> io::Result<Self> {
        const ATTEMPTS: usize = 20;
        for attempt in 0..ATTEMPTS {
            if unsafe { OpenClipboard(ptr::null_mut()) } != 0 {
                return Ok(Self);
            }
            if attempt + 1 < ATTEMPTS {
                thread::sleep(Duration::from_millis(5));
            }
        }
        Err(io::Error::last_os_error())
    }
}

impl Drop for ClipboardGuard {
    fn drop(&mut self) {
        unsafe {
            CloseClipboard();
        }
    }
}

struct GlobalMemoryGuard(Handle);

impl Drop for GlobalMemoryGuard {
    fn drop(&mut self) {
        unsafe {
            GlobalUnlock(self.0);
        }
    }
}

fn encode_key_event(key: KeyEvent) -> Option<TerminalKeyEvent> {
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
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    let alt = key.modifiers.contains(KeyModifiers::ALT);

    let mut bytes = match key.code {
        KeyCode::Backspace => {
            if ctrl {
                vec![0x08]
            } else {
                vec![0x7f]
            }
        }
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
        KeyCode::F(number) => function_key(number)?,
        KeyCode::Char(ch) => encode_character_key(ch, ctrl)?,
        KeyCode::Null if ctrl => vec![0],
        _ => return None,
    };

    if alt && !bytes.starts_with(b"\x1b") {
        bytes.insert(0, 0x1b);
    }
    Some(bytes)
}

fn encode_character_key(ch: char, ctrl: bool) -> Option<Vec<u8>> {
    if ctrl {
        if ch.is_ascii_alphabetic() {
            return Some(vec![(ch.to_ascii_lowercase() as u8) & 0x1f]);
        }
        match ch {
            ' ' | '@' => return Some(vec![0x00]),
            '[' => return Some(vec![0x1b]),
            '\\' => return Some(vec![0x1c]),
            ']' => return Some(vec![0x1d]),
            '^' => return Some(vec![0x1e]),
            '_' => return Some(vec![0x1f]),
            '?' => return Some(vec![0x7f]),
            _ => {}
        }
    }

    let mut buffer = [0; 4];
    Some(ch.encode_utf8(&mut buffer).as_bytes().to_vec())
}

fn function_key(n: u8) -> Option<Vec<u8>> {
    match n {
        1 => Some(b"\x1bOP".to_vec()),
        2 => Some(b"\x1bOQ".to_vec()),
        3 => Some(b"\x1bOR".to_vec()),
        4 => Some(b"\x1bOS".to_vec()),
        5 => Some(b"\x1b[15~".to_vec()),
        6 => Some(b"\x1b[17~".to_vec()),
        7 => Some(b"\x1b[18~".to_vec()),
        8 => Some(b"\x1b[19~".to_vec()),
        9 => Some(b"\x1b[20~".to_vec()),
        10 => Some(b"\x1b[21~".to_vec()),
        11 => Some(b"\x1b[23~".to_vec()),
        12 => Some(b"\x1b[24~".to_vec()),
        _ => None,
    }
}

pub fn write_output(bytes: &[u8]) -> io::Result<()> {
    let mut stdout = io::stdout().lock();
    stdout.write_all(bytes)?;
    stdout.flush()
}

pub fn write_render_transaction(bytes: &[u8], synchronized_output: bool) -> io::Result<()> {
    if bytes.is_empty() {
        return Ok(());
    }
    let mut stdout = io::stdout().lock();
    write_render_transaction_to(&mut stdout, bytes, synchronized_output)?;
    stdout.flush()
}

fn write_render_transaction_to(
    writer: &mut impl Write,
    bytes: &[u8],
    synchronized_output: bool,
) -> io::Result<()> {
    if synchronized_output {
        writer.write_all(b"\x1b[?2026h")?;
    }
    writer.write_all(bytes)?;
    if synchronized_output {
        writer.write_all(b"\x1b[?2026l")?;
    }
    Ok(())
}

pub fn size() -> io::Result<TerminalSize> {
    let output = unsafe { GetStdHandle(STD_OUTPUT_HANDLE) };
    if output.is_null() || output == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    let mut info = ConsoleScreenBufferInfo::default();
    let ok = unsafe { GetConsoleScreenBufferInfo(output, &mut info) };
    if ok == 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(TerminalSize::new(
        (info.sr_window.right - info.sr_window.left + 1).max(1) as u16,
        (info.sr_window.bottom - info.sr_window.top + 1).max(1) as u16,
    ))
}

fn get_mode(handle: Handle) -> io::Result<Dword> {
    let mut mode = 0;
    let ok = unsafe { GetConsoleMode(handle, &mut mode) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(mode)
    }
}

fn set_mode(handle: Handle, mode: Dword) -> io::Result<()> {
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

const ENABLE_WINDOW_INPUT: Dword = 0x0008;
const ENABLE_MOUSE_INPUT: Dword = 0x0010;
const ENABLE_EXTENDED_FLAGS: Dword = 0x0080;
const ENABLE_PROCESSED_OUTPUT: Dword = 0x0001;
const ENABLE_WRAP_AT_EOL_OUTPUT: Dword = 0x0002;
const ENABLE_VIRTUAL_TERMINAL_PROCESSING: Dword = 0x0004;
const CF_UNICODETEXT: u32 = 13;
const GMEM_MOVEABLE: u32 = 0x0002;

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
struct ConsoleScreenBufferInfo {
    size: Coord,
    cursor_position: Coord,
    attributes: u16,
    sr_window: SmallRect,
    maximum_window_size: Coord,
}

#[link(name = "kernel32")]
extern "system" {
    fn GetStdHandle(nStdHandle: Dword) -> Handle;
    fn GetConsoleMode(hConsoleHandle: Handle, lpMode: *mut Dword) -> Bool;
    fn SetConsoleMode(hConsoleHandle: Handle, dwMode: Dword) -> Bool;
    fn GetConsoleScreenBufferInfo(
        hConsoleOutput: Handle,
        lpConsoleScreenBufferInfo: *mut ConsoleScreenBufferInfo,
    ) -> Bool;
}

#[link(name = "user32")]
extern "system" {
    fn OpenClipboard(hwnd: Handle) -> Bool;
    fn CloseClipboard() -> Bool;
    fn GetClipboardData(format: u32) -> Handle;
    fn EmptyClipboard() -> Bool;
    fn SetClipboardData(format: u32, memory: Handle) -> Handle;
}

#[link(name = "kernel32")]
extern "system" {
    fn GlobalLock(hmem: Handle) -> *mut c_void;
    fn GlobalUnlock(hmem: Handle) -> Bool;
    fn GlobalAlloc(flags: u32, bytes: usize) -> Handle;
    fn GlobalFree(hmem: Handle) -> Handle;
}

#[cfg(test)]
mod tests {
    use super::*;
    use wmux_platform::{TerminalKeyCode, TerminalKeyEvent, TerminalKeyModifiers};

    fn key(code: KeyCode, modifiers: KeyModifiers) -> KeyEvent {
        KeyEvent::new(code, modifiers)
    }

    #[test]
    fn normalized_key_keeps_semantic_identity_and_application_bytes() {
        assert_eq!(
            encode_key_event(key(KeyCode::Char('B'), KeyModifiers::CONTROL)),
            Some(TerminalKeyEvent {
                code: TerminalKeyCode::Char('b'),
                modifiers: TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                raw: vec![0x02],
            })
        );
    }

    #[test]
    fn multibyte_and_navigation_keys_are_one_semantic_event() {
        let unicode = encode_key_event(key(KeyCode::Char('λ'), KeyModifiers::ALT)).unwrap();
        assert_eq!(unicode.code, TerminalKeyCode::Char('λ'));
        assert_eq!(
            unicode.modifiers,
            TerminalKeyModifiers::new(TerminalKeyModifiers::ALT)
        );
        assert_eq!(unicode.raw, [0x1b, 0xce, 0xbb]);

        let up = encode_key_event(key(KeyCode::Up, KeyModifiers::NONE)).unwrap();
        assert_eq!(up.code, TerminalKeyCode::Up);
        assert_eq!(up.modifiers, TerminalKeyModifiers::default());
        assert_eq!(up.raw, b"\x1b[A");
    }

    #[test]
    fn normalizes_supported_key_codes_and_modifiers() {
        let shifted = encode_key_event(key(KeyCode::Char('A'), KeyModifiers::SHIFT)).unwrap();
        assert_eq!(shifted.code, TerminalKeyCode::Char('a'));
        assert_eq!(
            shifted.modifiers,
            TerminalKeyModifiers::new(TerminalKeyModifiers::SHIFT)
        );
        assert_eq!(shifted.raw, b"A");

        let super_key = encode_key_event(key(KeyCode::Char('k'), KeyModifiers::SUPER)).unwrap();
        assert_eq!(super_key.code, TerminalKeyCode::Char('k'));
        assert_eq!(
            super_key.modifiers,
            TerminalKeyModifiers::new(TerminalKeyModifiers::SUPER)
        );
        assert_eq!(super_key.raw, b"k");

        let back_tab = encode_key_event(key(KeyCode::BackTab, KeyModifiers::SHIFT)).unwrap();
        assert_eq!(back_tab.code, TerminalKeyCode::BackTab);
        assert_eq!(
            back_tab.modifiers,
            TerminalKeyModifiers::new(TerminalKeyModifiers::SHIFT)
        );
        assert_eq!(back_tab.raw, b"\x1b[Z");
    }

    #[test]
    fn normalizes_control_and_fixed_keys() {
        let cases = [
            (
                key(KeyCode::Char(' '), KeyModifiers::CONTROL),
                TerminalKeyCode::Char(' '),
                TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                vec![0x00],
            ),
            (
                key(KeyCode::Backspace, KeyModifiers::CONTROL),
                TerminalKeyCode::Backspace,
                TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                vec![0x08],
            ),
            (
                key(KeyCode::Enter, KeyModifiers::NONE),
                TerminalKeyCode::Enter,
                TerminalKeyModifiers::default(),
                vec![b'\r'],
            ),
            (
                key(KeyCode::Esc, KeyModifiers::NONE),
                TerminalKeyCode::Escape,
                TerminalKeyModifiers::default(),
                vec![0x1b],
            ),
        ];

        for (input, code, modifiers, raw) in cases {
            assert_eq!(
                encode_key_event(input),
                Some(TerminalKeyEvent {
                    code,
                    modifiers,
                    raw,
                })
            );
        }
    }

    #[test]
    fn normalizes_supported_function_keys() {
        let f1 = encode_key_event(key(KeyCode::F(1), KeyModifiers::NONE)).unwrap();
        assert_eq!(f1.code, TerminalKeyCode::Function(1));
        assert_eq!(f1.raw, b"\x1bOP");

        let f12 = encode_key_event(key(KeyCode::F(12), KeyModifiers::ALT)).unwrap();
        assert_eq!(f12.code, TerminalKeyCode::Function(12));
        assert_eq!(
            f12.modifiers,
            TerminalKeyModifiers::new(TerminalKeyModifiers::ALT)
        );
        assert_eq!(f12.raw, b"\x1b[24~");
    }

    #[test]
    fn encodes_printable_character() {
        assert_eq!(
            encode_key_bytes(key(KeyCode::Char('a'), KeyModifiers::NONE)),
            Some(b"a".to_vec())
        );
    }

    #[test]
    fn encodes_ctrl_b_as_prefix_byte() {
        assert_eq!(
            encode_key_bytes(key(KeyCode::Char('b'), KeyModifiers::CONTROL)),
            Some(vec![0x02])
        );
    }

    #[test]
    fn encodes_ctrl_c_from_virtual_key_without_unicode_char() {
        assert_eq!(
            encode_key_bytes(key(KeyCode::Char('c'), KeyModifiers::CONTROL)),
            Some(vec![0x03])
        );
    }

    #[test]
    fn encodes_backspace() {
        assert_eq!(
            encode_key_bytes(key(KeyCode::Backspace, KeyModifiers::NONE)),
            Some(vec![0x7f])
        );
    }

    #[test]
    fn encodes_ctrl_backspace_as_legacy_ctrl_h() {
        assert_eq!(
            encode_key_bytes(key(KeyCode::Backspace, KeyModifiers::CONTROL)),
            Some(vec![0x08])
        );
    }

    #[test]
    fn render_transaction_is_one_synchronized_host_write() {
        let mut output = Vec::new();
        write_render_transaction_to(&mut output, b"frame", true).unwrap();
        assert_eq!(output, b"\x1b[?2026hframe\x1b[?2026l");

        output.clear();
        write_render_transaction_to(&mut output, b"frame", false).unwrap();
        assert_eq!(output, b"frame");
    }

    #[test]
    fn recognizes_native_windows_paste_shortcuts() {
        assert!(is_clipboard_paste_key(key(
            KeyCode::Char('v'),
            KeyModifiers::CONTROL
        )));
        assert!(is_clipboard_paste_key(key(
            KeyCode::Char('V'),
            KeyModifiers::CONTROL | KeyModifiers::SHIFT
        )));
        assert!(!is_clipboard_paste_key(key(
            KeyCode::Char('p'),
            KeyModifiers::CONTROL | KeyModifiers::SHIFT
        )));
    }
}
