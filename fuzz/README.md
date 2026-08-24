# wmux parser fuzzing

This independent Cargo package keeps libFuzzer tooling out of the production
workspace graph. The targets exercise only OS-neutral code:

- `protocol_frame` validates complete fixed headers and matching payloads. A
  secondary bounded path normalizes every non-empty input to a valid tag so
  payload decoding is exercised even before the fuzzer discovers a matching
  frame length.
- `terminal_bytes` bounds input to 64 KiB, dimensions to 120x50, history to 128
  rows, and feed chunks to 64 bytes before resize, copy, and full-render paths.
- `command_text` bounds input to 1 MiB, decodes arbitrary bytes lossily, and
  parses and walks every command in successful command lists without executing
  server mutations.

Install `cargo-fuzz` and run sanitizer-backed fuzzing on a supported nightly
Linux or macOS x86-64/aarch64 host. Run these commands from the repository
root so artifacts stay in the ignored per-target directories:

```bash
rustup toolchain install nightly --profile minimal
cargo +nightly install cargo-fuzz --locked
mkdir -p fuzz/artifacts/command_text fuzz/artifacts/protocol_frame fuzz/artifacts/terminal_bytes
cargo +nightly fuzz run command_text -- -max_total_time=30 -artifact_prefix=fuzz/artifacts/command_text/
cargo +nightly fuzz run protocol_frame -- -max_total_time=30 -artifact_prefix=fuzz/artifacts/protocol_frame/
cargo +nightly fuzz run terminal_bytes -- -max_total_time=30 -artifact_prefix=fuzz/artifacts/terminal_bytes/
```

Those three 30-second runs are the CI smoke gate. The release-candidate gate
runs every target for 15 minutes:

```bash
cargo +nightly fuzz run command_text -- -max_total_time=900 -artifact_prefix=fuzz/artifacts/command_text/
cargo +nightly fuzz run protocol_frame -- -max_total_time=900 -artifact_prefix=fuzz/artifacts/protocol_frame/
cargo +nightly fuzz run terminal_bytes -- -max_total_time=900 -artifact_prefix=fuzz/artifacts/terminal_bytes/
```

Minimize and replay a sanitizer artifact before converting it into an owning
unit or integration regression:

```bash
cargo +nightly fuzz tmin terminal_bytes fuzz/artifacts/terminal_bytes/crash-HASH
cargo +nightly fuzz run terminal_bytes fuzz/artifacts/terminal_bytes/crash-HASH
```

From Windows, validate the manifest and compile the harnesses without claiming
a sanitizer-backed fuzz run:

```powershell
cargo metadata --manifest-path fuzz/Cargo.toml --no-deps
cargo check --manifest-path fuzz/Cargo.toml --bins
cargo clippy --manifest-path fuzz/Cargo.toml --bins -- -D warnings
```

Keep checked-in seeds small and semantic. Do not replace the malformed inputs
with generated coverage files or raw crash artifacts. A minimized failure must
become an ordinary test in the crate that owns the parser as well as a named
corpus seed. Upload `fuzz/artifacts` on CI failure; an empty directory on success
is expected.
