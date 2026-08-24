#![no_main]

use libfuzzer_sys::fuzz_target;
use std::hint::black_box;

const MAX_INPUT_BYTES: usize = 64 * 1024;

fuzz_target!(|data: &[u8]| {
    if data.len() <= MAX_INPUT_BYTES {
        let _ = black_box(wmux_config::parse_theme_document(data));
    }
});
