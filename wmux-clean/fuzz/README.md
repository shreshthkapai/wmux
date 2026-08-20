# wmux parser fuzzing

This independent Cargo package keeps libFuzzer tooling out of the production
workspace graph. The targets exercise only OS-neutral code:

- `protocol_frame` validates complete fixed headers and matching payloads. A
  secondary bounded path normalizes every non-empty input to a valid tag so
  payload decoding is exercised even before the fuzzer discovers a matching
  frame length.
- `terminal_bytes` bounds input to 64 KiB, dimensions to 120x50, history to 128
  rows, and feed chunks to 64 bytes before resize, copy, and full-render paths.

Run sanitizer-backed fuzzing on a supported nightly Unix-like x86-64 or aarch64
host:

```bash
cargo +nightly fuzz run terminal_bytes -- -max_total_time=60
cargo +nightly fuzz run protocol_frame -- -max_total_time=60
```

From Windows, validate the manifest and compile the harnesses without claiming
a sanitizer-backed fuzz run:

```powershell
cargo metadata --manifest-path fuzz/Cargo.toml --no-deps
cargo check --manifest-path fuzz/Cargo.toml --bins
```

Keep checked-in seeds small and semantic. Do not replace the malformed inputs
with generated crash artifacts; add minimized regressions as ordinary tests as
well as corpus entries.
