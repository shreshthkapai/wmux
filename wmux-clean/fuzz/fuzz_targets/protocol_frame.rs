#![no_main]

use libfuzzer_sys::fuzz_target;
use wmux_protocol::{decode_frame_header, decode_frame_payload, FRAME_HEADER_LEN};

fuzz_target!(|data: &[u8]| {
    if data.len() < FRAME_HEADER_LEN {
        return;
    }

    let mut header = [0_u8; FRAME_HEADER_LEN];
    header.copy_from_slice(&data[..FRAME_HEADER_LEN]);
    let Ok((tag, declared_len)) = decode_frame_header(&header) else {
        return;
    };
    let payload = &data[FRAME_HEADER_LEN..];
    if declared_len == payload.len() {
        let _ = decode_frame_payload(tag, payload);
    }
});
