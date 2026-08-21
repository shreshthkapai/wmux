#![no_main]

use libfuzzer_sys::fuzz_target;
use std::hint::black_box;
use wmux_core::parse_command_text;

const MAX_INPUT_BYTES: usize = 1024 * 1024;

fuzz_target!(|data: &[u8]| {
    if data.len() > MAX_INPUT_BYTES {
        return;
    }
    let text = String::from_utf8_lossy(data);
    if let Ok(commands) = parse_command_text(&text) {
        for command in commands.iter() {
            black_box(command);
        }
    }
});
