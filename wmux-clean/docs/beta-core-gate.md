# Phase 8 Beta-Core Gate

**Status on 2026-08-22:** Beta Core implementation complete; native macOS
runtime gate pending.

Phase 8 qualifies the persistent, nonvisual core built in Phases 1 through 7.
It does not include UI/UX polish, themes, status decoration, packaging, or
release assets. The repository-root workflow is configured to supply the
remaining macOS evidence, but configuration is not execution evidence.

## Gate results

| Required behavior | Executable evidence | Result |
| --- | --- | --- |
| Repeated attach/detach and abrupt disconnect | `wmux-stress` lifecycle plus both native lifecycle suites | passed on Windows and Linux |
| Authoritative reattach after background output | stress and real PTY lifecycle suites | passed on Windows and Linux |
| Clean shutdown, endpoint removal, and restart | isolated native server tests | passed on Windows and Linux |
| Resize storms converge | 2,000-resize event-pressure scenario | passed |
| Output, exit, kill, and close ordering | event-pressure scenario and focused server regressions | passed |
| Slow-client isolation and bounded queues | 32-client fan-out scenario and focused queue tests | passed |
| Many panes and clients | 32 panes and 32 clients | passed |
| Large history and paste | 100,000 lines and exact 16 MiB paste | passed |
| Synchronized/control output remains bounded | storage stress and server queue tests | passed |
| Recoverable terminal exits restore once | client fake-guard tests and native exact-mode tests | passed |
| Native process-tree cleanup | Job Object and process-group descendant tests | passed on Windows and Linux |
| Malformed input under sanitizers | three 30-second cargo-fuzz runs | passed on Linux |
| Existing performance thresholds | full release `wmux-bench --gate` | passed on Windows |
| Cross-OS portable semantics | 16-case conformance fingerprint | identical on Windows and Linux; macOS runtime pending |
| Difference classification | `known-differences.md` | no unexplained divergence |

## Deterministic fingerprints

The full stress profile was run twice on Windows and twice on Linux. Every run
produced the same values:

| Scenario | Operations | Fingerprint |
| --- | ---: | --- |
| lifecycle | 760 | `97b58005542b360c` |
| event-pressure | 2,005 | `18b6c1a82ebba442` |
| fan-out | 163 | `9b10258b994ae82a` |
| storage | 100,260 | `736e6230dad79c0c` |
| aggregate | 103,188 | `d537f5686435cc2e` |

The shorter CI profile produces aggregate `ddaebbc7a1286327`. The 16-case
portable suite produced aggregate `d5670ad858ef5735` twice on both Windows and
Linux. `EXPECTED_DIFFERENCES` remains empty.

## Verification commands

Run from `wmux-clean`:

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
cargo check --locked --manifest-path fuzz/Cargo.toml --bins
cargo clippy --locked --manifest-path fuzz/Cargo.toml --bins -- -D warnings
cargo run --locked -p wmux-conformance --release
cargo run --locked -p wmux-stress --release -- --profile full
cargo run --locked -p wmux-bench --release -- --suite full --gate
```

The Linux gate runs the same format, lint, workspace test, conformance, and
stress commands in `rust:1.96-bookworm`. Apple target evidence is produced by:

```powershell
cargo check --locked -p wmux-conformance -p wmux-unix -p wmux -p wmux-server --target x86_64-apple-darwin
cargo check --locked -p wmux-conformance -p wmux-unix -p wmux -p wmux-server --target aarch64-apple-darwin
```

These commands are compile evidence only. The `portable`, `native`, and
`stress` jobs in `.github/workflows/beta-core.yml` must succeed on
`macos-latest` before Phase 8 is fully complete.

## Fuzz evidence

On Linux, the nightly sanitizer smoke completed without crashes or artifacts:

| Target | Duration | Executions |
| --- | ---: | ---: |
| `command_text` | 30 seconds | 167,660 |
| `protocol_frame` | 30 seconds | 7,225,936 |
| `terminal_bytes` | 30 seconds | 25,935 |

CI retains crash artifacts on failure. Release-candidate practice extends each
target to 15 minutes and minimizes/replays any crash before accepting it.

## Local hosts and counts

- Windows: Microsoft Windows NT 10.0.26200.0, Rust/Cargo 1.96.0; 350 workspace
  tests, full conformance and stress twice, and the full performance gate.
- Linux: Debian Bookworm container on Linux
  6.6.114.1-microsoft-standard-WSL2 x86_64, Rust/Cargo 1.96.1; 354 workspace
  tests, real AF_UNIX/PTY lifecycle, and full conformance and stress twice.
- macOS: Intel and Apple Silicon target checks passed; no native runner result
  is available for this commit.

## Remaining acceptance work

1. Push or otherwise dispatch the repository-root workflow and require green
   macOS portable, native lifecycle, and stress jobs.
2. Record those job URLs and change the applicable macOS matrix cells from
   `compile-only`/`manual-pending` to `CI-verified`.
3. Perform the terminal-host/shell smoke matrix documented in
   [compatibility-matrix.md](compatibility-matrix.md).

The uncatchable-kill restoration limit and native mechanism differences are
recorded in [known-differences.md](known-differences.md). No UI work is part of
this gate.
