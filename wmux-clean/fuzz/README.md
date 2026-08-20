# wmux parser fuzzing

This independent Cargo package keeps libFuzzer tooling out of the production
workspace graph. The targets exercise only OS-neutral code:

- `protocol_frame` validates a fixed header and decodes a payload only when its
  declared length exactly matches the remaining input.
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
