#![no_main]

use libfuzzer_sys::fuzz_target;
use wmux_protocol::{decode_frame_header, decode_frame_payload, FRAME_HEADER_LEN};

fuzz_target!(|data: &[u8]| {
    let decoded_hex = decode_hex_seed(data);
    let data = decoded_hex.as_deref().unwrap_or(data);

    if let Some((&selector, payload)) = data.split_first() {
        let tag = selector % 14 + 1;
        let payload = &payload[..payload.len().min(64 * 1024)];
        let _ = decode_frame_payload(tag, payload);
    }

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

fn decode_hex_seed(data: &[u8]) -> Option<Vec<u8>> {
    let encoded = data.strip_prefix(b"hex:")?;
    let mut decoded = Vec::with_capacity(encoded.len() / 2);
    let mut high = None;
    for byte in encoded.iter().copied() {
        if byte.is_ascii_whitespace() {
            continue;
        }
        let nibble = match byte {
            b'0'..=b'9' => byte - b'0',
            b'a'..=b'f' => byte - b'a' + 10,
            b'A'..=b'F' => byte - b'A' + 10,
            _ => return None,
        };
        if let Some(high) = high.take() {
            decoded.push((high << 4) | nibble);
        } else {
            high = Some(nibble);
        }
    }
    high.is_none().then_some(decoded)
}
