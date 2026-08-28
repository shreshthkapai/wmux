use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use std::{
    cell::RefCell,
    ffi::c_void,
    io::{self, Write},
    os::windows::ffi::OsStringExt,
    ptr, thread,
    time::Duration,
};
use windows_sys::Win32::System::Console::{
    ReadConsoleInputW, DOUBLE_CLICK, FROM_LEFT_1ST_BUTTON_PRESSED, FROM_LEFT_2ND_BUTTON_PRESSED,
    INPUT_RECORD, KEY_EVENT, KEY_EVENT_RECORD, LEFT_ALT_PRESSED, LEFT_CTRL_PRESSED, MOUSE_EVENT,
    MOUSE_EVENT_RECORD, MOUSE_HWHEELED, MOUSE_MOVED, MOUSE_WHEELED, RIGHTMOST_BUTTON_PRESSED,
    RIGHT_ALT_PRESSED, RIGHT_CTRL_PRESSED, SHIFT_PRESSED, WINDOW_BUFFER_SIZE_EVENT,
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

        // Request window and mouse input through the native Windows console
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
        let record = read_console_input_record()?;
        let input = NATIVE_INPUT_DECODER
            .with(|decoder| decode_native_input_record(&mut decoder.borrow_mut(), record))?;
        match input {
            Some(NativeInput::Key(key)) => {
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
            Some(NativeInput::Mouse(event)) => return Ok(Some(ConsoleInput::Mouse(event))),
            Some(NativeInput::Resize(size)) => return Ok(Some(ConsoleInput::Resize(size))),
            None => {}
        }
    }
}

thread_local! {
    static NATIVE_INPUT_DECODER: RefCell<NativeInputDecoder> = RefCell::new(NativeInputDecoder::default());
}

#[derive(Default)]
struct NativeInputDecoder {
    key: NativeKeyDecoder,
    mouse_buttons: u32,
}

#[derive(Default)]
struct NativeKeyDecoder {
    high_surrogate: Option<u16>,
}

enum NativeInput {
    Key(KeyEvent),
    Mouse(MouseEvent),
    Resize(TerminalSize),
}

fn read_console_input_record() -> io::Result<INPUT_RECORD> {
    let input = unsafe { GetStdHandle(STD_INPUT_HANDLE) };
    if input.is_null() || input == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }

    loop {
        let mut record = INPUT_RECORD::default();
        let mut read = 0;
        let ok = unsafe { ReadConsoleInputW(input, &mut record, 1, &mut read) };
        if ok == 0 {
            return Err(io::Error::last_os_error());
        }
        if read == 1 {
            return Ok(record);
        }
    }
}

fn decode_native_input_record(
    decoder: &mut NativeInputDecoder,
    record: INPUT_RECORD,
) -> io::Result<Option<NativeInput>> {
    match u32::from(record.EventType) {
        KEY_EVENT => {
            let key = unsafe { record.Event.KeyEvent };
            Ok(decode_native_key_record(&mut decoder.key, key).map(NativeInput::Key))
        }
        MOUSE_EVENT => {
            let mouse = unsafe { record.Event.MouseEvent };
            let window_top = console_window_top()?;
            Ok(
                decode_native_mouse_record(&mut decoder.mouse_buttons, mouse, window_top)
                    .map(NativeInput::Mouse),
            )
        }
        WINDOW_BUFFER_SIZE_EVENT => {
            let resize = unsafe { record.Event.WindowBufferSizeEvent };
            Ok(Some(NativeInput::Resize(TerminalSize::new(
                (resize.dwSize.X as u16).saturating_add(1).max(1),
                (resize.dwSize.Y as u16).saturating_add(1).max(1),
            ))))
        }
        _ => Ok(None),
    }
}

fn decode_native_key_record(
    decoder: &mut NativeKeyDecoder,
    record: KEY_EVENT_RECORD,
) -> Option<KeyEvent> {
    let modifiers = native_key_modifiers(record.dwControlKeyState);
    let unicode = unsafe { record.uChar.UnicodeChar };
    let is_alt_code = record.wVirtualKeyCode == VK_MENU && record.bKeyDown == 0 && unicode != 0;
    if record.bKeyDown == 0 && !is_alt_code {
        return None;
    }

    let only_alt = modifiers.contains(KeyModifiers::ALT)
        && !modifiers.intersects(KeyModifiers::SHIFT | KeyModifiers::CONTROL);
    if only_alt && (VK_NUMPAD0..=VK_NUMPAD9).contains(&record.wVirtualKeyCode) {
        return None;
    }

    let code = match record.wVirtualKeyCode {
        VK_SHIFT | VK_CONTROL | VK_MENU if unicode == 0 => None,
        VK_BACK => Some(KeyCode::Backspace),
        VK_TAB if modifiers.contains(KeyModifiers::SHIFT) => Some(KeyCode::BackTab),
        VK_TAB => Some(KeyCode::Tab),
        VK_RETURN => Some(KeyCode::Enter),
        VK_ESCAPE => Some(KeyCode::Esc),
        VK_PAGE_UP => Some(KeyCode::PageUp),
        VK_PAGE_DOWN => Some(KeyCode::PageDown),
        VK_END => Some(KeyCode::End),
        VK_HOME => Some(KeyCode::Home),
        VK_LEFT => Some(KeyCode::Left),
        VK_UP => Some(KeyCode::Up),
        VK_RIGHT => Some(KeyCode::Right),
        VK_DOWN => Some(KeyCode::Down),
        VK_INSERT => Some(KeyCode::Insert),
        VK_DELETE => Some(KeyCode::Delete),
        VK_F1..=VK_F24 => Some(KeyCode::F((record.wVirtualKeyCode - VK_F1 + 1) as u8)),
        _ => decode_native_character(decoder, record.wVirtualKeyCode, unicode).map(KeyCode::Char),
    }?;

    Some(KeyEvent::new(code, modifiers))
}

fn decode_native_character(
    decoder: &mut NativeKeyDecoder,
    virtual_key: u16,
    unicode: u16,
) -> Option<char> {
    match unicode {
        high @ 0xd800..=0xdbff => {
            decoder.high_surrogate = Some(high);
            None
        }
        low @ 0xdc00..=0xdfff => {
            let high = decoder.high_surrogate.take()?;
            char::decode_utf16([high, low]).next()?.ok()
        }
        0x20..=0xd7ff | 0xe000..=u16::MAX => {
            decoder.high_surrogate = None;
            char::from_u32(u32::from(unicode))
        }
        _ => {
            decoder.high_surrogate = None;
            match virtual_key {
                VK_A..=VK_Z => {
                    char::from_u32(u32::from(virtual_key)).map(|ch| ch.to_ascii_lowercase())
                }
                VK_0..=VK_9 => char::from_u32(u32::from(virtual_key)),
                _ => None,
            }
        }
    }
}

fn native_key_modifiers(state: u32) -> KeyModifiers {
    let mut modifiers = KeyModifiers::NONE;
    if state & SHIFT_PRESSED != 0 {
        modifiers |= KeyModifiers::SHIFT;
    }
    if state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED) != 0 {
        modifiers |= KeyModifiers::ALT;
    }
    if state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED) != 0 {
        modifiers |= KeyModifiers::CONTROL;
    }
    modifiers
}

fn decode_native_mouse_record(
    previous_buttons: &mut u32,
    record: MOUSE_EVENT_RECORD,
    window_top: i16,
) -> Option<MouseEvent> {
    let current_buttons = record.dwButtonState & MOUSE_BUTTON_MASK;
    let previous = *previous_buttons;
    *previous_buttons = current_buttons;

    let (kind, button) = match record.dwEventFlags {
        0 | DOUBLE_CLICK => button_transition(previous, current_buttons)?,
        MOUSE_MOVED if current_buttons == 0 => (MouseEventKind::Move, MouseButton::None),
        MOUSE_MOVED => (MouseEventKind::Drag, pressed_mouse_button(current_buttons)),
        MOUSE_WHEELED => (
            if wheel_delta(record.dwButtonState) > 0 {
                MouseEventKind::ScrollUp
            } else {
                MouseEventKind::ScrollDown
            },
            MouseButton::None,
        ),
        MOUSE_HWHEELED => (
            if wheel_delta(record.dwButtonState) > 0 {
                MouseEventKind::ScrollRight
            } else {
                MouseEventKind::ScrollLeft
            },
            MouseButton::None,
        ),
        _ => return None,
    };

    Some(MouseEvent {
        kind,
        button,
        modifiers: native_mouse_modifiers(record.dwControlKeyState),
        column: record.dwMousePosition.X.max(0) as u16,
        row: record.dwMousePosition.Y.saturating_sub(window_top).max(0) as u16,
    })
}

fn button_transition(previous: u32, current: u32) -> Option<(MouseEventKind, MouseButton)> {
    for (mask, button) in [
        (FROM_LEFT_1ST_BUTTON_PRESSED, MouseButton::Left),
        (RIGHTMOST_BUTTON_PRESSED, MouseButton::Right),
        (FROM_LEFT_2ND_BUTTON_PRESSED, MouseButton::Middle),
    ] {
        match (previous & mask != 0, current & mask != 0) {
            (false, true) => return Some((MouseEventKind::Down, button)),
            (true, false) => return Some((MouseEventKind::Up, button)),
            _ => {}
        }
    }
    None
}

fn pressed_mouse_button(buttons: u32) -> MouseButton {
    if buttons & RIGHTMOST_BUTTON_PRESSED != 0 {
        MouseButton::Right
    } else if buttons & FROM_LEFT_2ND_BUTTON_PRESSED != 0 {
        MouseButton::Middle
    } else {
        MouseButton::Left
    }
}

fn wheel_delta(button_state: u32) -> i16 {
    (button_state >> 16) as u16 as i16
}

fn native_mouse_modifiers(state: u32) -> MouseModifiers {
    let modifiers = native_key_modifiers(state);
    let mut bits = 0;
    if modifiers.contains(KeyModifiers::SHIFT) {
        bits |= MouseModifiers::SHIFT;
    }
    if modifiers.contains(KeyModifiers::ALT) {
        bits |= MouseModifiers::ALT;
    }
    if modifiers.contains(KeyModifiers::CONTROL) {
        bits |= MouseModifiers::CONTROL;
    }
    MouseModifiers::new(bits)
}

fn console_window_top() -> io::Result<i16> {
    let output = unsafe { GetStdHandle(STD_OUTPUT_HANDLE) };
    if output.is_null() || output == INVALID_HANDLE_VALUE {
        return Err(io::Error::last_os_error());
    }
    let mut info = ConsoleScreenBufferInfo::default();
    let ok = unsafe { GetConsoleScreenBufferInfo(output, &mut info) };
    if ok == 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(info.sr_window.top)
    }
}

const VK_BACK: u16 = 0x08;
const VK_TAB: u16 = 0x09;
const VK_RETURN: u16 = 0x0d;
const VK_SHIFT: u16 = 0x10;
const VK_CONTROL: u16 = 0x11;
const VK_MENU: u16 = 0x12;
const VK_ESCAPE: u16 = 0x1b;
const VK_PAGE_UP: u16 = 0x21;
const VK_PAGE_DOWN: u16 = 0x22;
const VK_END: u16 = 0x23;
const VK_HOME: u16 = 0x24;
const VK_LEFT: u16 = 0x25;
const VK_UP: u16 = 0x26;
const VK_RIGHT: u16 = 0x27;
const VK_DOWN: u16 = 0x28;
const VK_0: u16 = 0x30;
const VK_9: u16 = 0x39;
const VK_A: u16 = 0x41;
const VK_Z: u16 = 0x5a;
const VK_NUMPAD0: u16 = 0x60;
const VK_NUMPAD9: u16 = 0x69;
const VK_F1: u16 = 0x70;
const VK_F24: u16 = 0x87;
const VK_INSERT: u16 = 0x2d;
const VK_DELETE: u16 = 0x2e;
const MOUSE_BUTTON_MASK: u32 =
    FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED | FROM_LEFT_2ND_BUTTON_PRESSED;

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
        KeyCode::Enter if ctrl && !alt && !key.modifiers.contains(KeyModifiers::SUPER) => {
            vec![b'\n']
        }
        KeyCode::Enter
            if key
                .modifiers
                .intersects(KeyModifiers::SHIFT | KeyModifiers::CONTROL | KeyModifiers::SUPER) =>
        {
            modified_enter_bytes(key.modifiers)
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

fn modified_enter_bytes(modifiers: KeyModifiers) -> Vec<u8> {
    let parameter = 1
        + u8::from(modifiers.contains(KeyModifiers::SHIFT))
        + 2 * u8::from(modifiers.contains(KeyModifiers::ALT))
        + 4 * u8::from(modifiers.contains(KeyModifiers::CONTROL))
        + 8 * u8::from(modifiers.contains(KeyModifiers::SUPER));
    format!("\x1b[13;{parameter}u").into_bytes()
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
    use windows_sys::Win32::System::Console::{
        COORD, KEY_EVENT_RECORD_0, LEFT_CTRL_PRESSED, MOUSE_EVENT_RECORD, SHIFT_PRESSED,
    };
    use wmux_platform::{TerminalKeyCode, TerminalKeyEvent, TerminalKeyModifiers};

    fn key(code: KeyCode, modifiers: KeyModifiers) -> KeyEvent {
        KeyEvent::new(code, modifiers)
    }

    fn native_key(virtual_key: u16, scan_code: u16, unicode: char, state: u32) -> KEY_EVENT_RECORD {
        native_key_u16(virtual_key, scan_code, unicode as u16, state, true)
    }

    fn native_key_u16(
        virtual_key: u16,
        scan_code: u16,
        unicode: u16,
        state: u32,
        key_down: bool,
    ) -> KEY_EVENT_RECORD {
        KEY_EVENT_RECORD {
            bKeyDown: i32::from(key_down),
            wRepeatCount: 1,
            wVirtualKeyCode: virtual_key,
            wVirtualScanCode: scan_code,
            uChar: KEY_EVENT_RECORD_0 {
                UnicodeChar: unicode,
            },
            dwControlKeyState: state,
        }
    }

    #[test]
    fn native_windows_shifted_digit_row_reaches_client_encoding() {
        let mut decoder = NativeKeyDecoder::default();
        let cases = [
            (b'1', 0x02, '!'),
            (b'2', 0x03, '@'),
            (b'3', 0x04, '#'),
            (b'4', 0x05, '$'),
            (b'5', 0x06, '%'),
            (b'6', 0x07, '^'),
            (b'7', 0x08, '&'),
            (b'8', 0x09, '*'),
            (b'9', 0x0a, '('),
            (b'0', 0x0b, ')'),
        ];

        for (virtual_key, scan_code, character) in cases {
            let record = native_key(u16::from(virtual_key), scan_code, character, SHIFT_PRESSED);
            let event = decode_native_key_record(&mut decoder, record)
                .and_then(encode_key_event)
                .unwrap_or_else(|| panic!("native {character:?} record was dropped"));
            assert_eq!(event.code, TerminalKeyCode::Char(character));
            assert_eq!(
                event.modifiers,
                TerminalKeyModifiers::new(TerminalKeyModifiers::SHIFT)
            );
            assert_eq!(event.raw, character.to_string().into_bytes());
        }
    }

    #[test]
    fn native_windows_parser_keeps_known_good_printable_records() {
        let mut decoder = NativeKeyDecoder::default();
        for (virtual_key, scan_code, character, state) in [
            (u16::from(b'D'), 0x20, 'D', SHIFT_PRESSED),
            (0xbd, 0x0c, '-', 0),
            (u16::from(b'7'), 0x08, '7', 0),
            (0xbd, 0x0c, '_', SHIFT_PRESSED),
            (0xdb, 0x1a, '{', SHIFT_PRESSED),
            (0xdc, 0x2b, '|', SHIFT_PRESSED),
        ] {
            let event = decode_native_key_record(
                &mut decoder,
                native_key(virtual_key, scan_code, character, state),
            )
            .and_then(encode_key_event)
            .unwrap_or_else(|| panic!("native {character:?} record was dropped"));
            assert_eq!(event.raw, character.to_string().into_bytes());
        }
    }

    #[test]
    fn native_windows_parser_preserves_control_shift_ime_and_surrogates() {
        let mut decoder = NativeKeyDecoder::default();
        let control_shift = decode_native_key_record(
            &mut decoder,
            native_key_u16(
                u16::from(b'D'),
                0x20,
                0x04,
                LEFT_CTRL_PRESSED | SHIFT_PRESSED,
                true,
            ),
        )
        .and_then(encode_key_event)
        .unwrap();
        assert_eq!(control_shift.code, TerminalKeyCode::Char('d'));
        assert_eq!(
            control_shift.modifiers,
            TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL | TerminalKeyModifiers::SHIFT)
        );
        assert_eq!(control_shift.raw, [0x04]);

        let ime = decode_native_key_record(&mut decoder, native_key_u16(0, 0, 0x03bb, 0, true))
            .and_then(encode_key_event)
            .unwrap();
        assert_eq!(ime.code, TerminalKeyCode::Char('λ'));
        assert_eq!(ime.raw, "λ".as_bytes());

        assert!(
            decode_native_key_record(&mut decoder, native_key_u16(0, 0, 0xd83d, 0, true),)
                .is_none()
        );
        let non_bmp = decode_native_key_record(&mut decoder, native_key_u16(0, 0, 0xde80, 0, true))
            .and_then(encode_key_event)
            .unwrap();
        assert_eq!(non_bmp.code, TerminalKeyCode::Char('🚀'));
        assert_eq!(non_bmp.raw, "🚀".as_bytes());
    }

    #[test]
    fn native_windows_parser_ignores_releases_and_bare_modifiers() {
        let mut decoder = NativeKeyDecoder::default();
        assert!(decode_native_key_record(
            &mut decoder,
            native_key_u16(u16::from(b'A'), 0x1e, u16::from(b'a'), 0, false),
        )
        .is_none());
        assert!(decode_native_key_record(
            &mut decoder,
            native_key_u16(VK_SHIFT, 0x2a, 0, SHIFT_PRESSED, true),
        )
        .is_none());
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
                key(KeyCode::Char('j'), KeyModifiers::CONTROL),
                TerminalKeyCode::Char('j'),
                TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                vec![b'\n'],
            ),
            (
                key(KeyCode::Enter, KeyModifiers::ALT),
                TerminalKeyCode::Enter,
                TerminalKeyModifiers::new(TerminalKeyModifiers::ALT),
                b"\x1b\r".to_vec(),
            ),
            (
                key(KeyCode::Enter, KeyModifiers::SHIFT),
                TerminalKeyCode::Enter,
                TerminalKeyModifiers::new(TerminalKeyModifiers::SHIFT),
                b"\x1b[13;2u".to_vec(),
            ),
            (
                key(KeyCode::Enter, KeyModifiers::CONTROL),
                TerminalKeyCode::Enter,
                TerminalKeyModifiers::new(TerminalKeyModifiers::CONTROL),
                vec![b'\n'],
            ),
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
    fn leading_ampersand_keeps_its_character_and_raw_byte() {
        assert_eq!(
            encode_key_event(key(KeyCode::Char('&'), KeyModifiers::SHIFT)),
            Some(TerminalKeyEvent {
                code: TerminalKeyCode::Char('&'),
                modifiers: TerminalKeyModifiers::new(TerminalKeyModifiers::SHIFT),
                raw: b"&".to_vec(),
            })
        );
    }

    #[test]
    fn mouse_drag_and_wheel_sequence_keeps_order_and_identity() {
        let record = |buttons, flags, state, column, row| MOUSE_EVENT_RECORD {
            dwMousePosition: COORD { X: column, Y: row },
            dwButtonState: buttons,
            dwControlKeyState: state,
            dwEventFlags: flags,
        };
        let mut buttons = 0;
        let actual = [
            decode_native_mouse_record(
                &mut buttons,
                record(FROM_LEFT_1ST_BUTTON_PRESSED, 0, 0, 2, 104),
                100,
            )
            .unwrap(),
            decode_native_mouse_record(
                &mut buttons,
                record(
                    FROM_LEFT_1ST_BUTTON_PRESSED,
                    MOUSE_MOVED,
                    SHIFT_PRESSED,
                    8,
                    104,
                ),
                100,
            )
            .unwrap(),
            decode_native_mouse_record(&mut buttons, record(0, 0, SHIFT_PRESSED, 8, 104), 100)
                .unwrap(),
            decode_native_mouse_record(
                &mut buttons,
                record(120_u32 << 16, MOUSE_WHEELED, LEFT_CTRL_PRESSED, 8, 104),
                100,
            )
            .unwrap(),
            decode_native_mouse_record(
                &mut buttons,
                record(
                    u32::from((-120_i16) as u16) << 16,
                    MOUSE_WHEELED,
                    LEFT_ALT_PRESSED,
                    8,
                    104,
                ),
                100,
            )
            .unwrap(),
        ];

        assert_eq!(
            actual,
            [
                MouseEvent {
                    kind: MouseEventKind::Down,
                    button: MouseButton::Left,
                    modifiers: MouseModifiers::default(),
                    column: 2,
                    row: 4,
                },
                MouseEvent {
                    kind: MouseEventKind::Drag,
                    button: MouseButton::Left,
                    modifiers: MouseModifiers::new(MouseModifiers::SHIFT),
                    column: 8,
                    row: 4,
                },
                MouseEvent {
                    kind: MouseEventKind::Up,
                    button: MouseButton::Left,
                    modifiers: MouseModifiers::new(MouseModifiers::SHIFT),
                    column: 8,
                    row: 4,
                },
                MouseEvent {
                    kind: MouseEventKind::ScrollUp,
                    button: MouseButton::None,
                    modifiers: MouseModifiers::new(MouseModifiers::CONTROL),
                    column: 8,
                    row: 4,
                },
                MouseEvent {
                    kind: MouseEventKind::ScrollDown,
                    button: MouseButton::None,
                    modifiers: MouseModifiers::new(MouseModifiers::ALT),
                    column: 8,
                    row: 4,
                },
            ]
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
