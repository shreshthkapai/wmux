# Cross-Platform Beta Baseline

Baseline date: 2026-08-20

## Canonical workspace declaration

The authoritative wmux product workspace is `wmux-clean/`.

- Run all Cargo build, format, lint, test, conformance, and benchmark commands from `wmux-clean/`.
- Put all future product implementation under `wmux-clean/crates/` until a separately approved physical consolidation is complete.
- Treat the root `Cargo.toml` and `crates/` as a preserved legacy reference. Do not add product behavior there.
- Use the outer `wmux/` directory as the Git repository and CI-discovery boundary.

This declaration changes ownership and build routing only. It does not move or delete either source tree.

## Inventory method

The crate inventory comes from `cargo metadata --no-deps --format-version 1`. Rust source files and lines were counted recursively per crate. `Test attrs` counts literal `#[test]` and `#[tokio::test]` attributes for discovery comparison; the executed-test total is recorded separately from `cargo test --workspace`.

### Legacy root workspace

| Crate | Rust files | Rust lines | Test attrs | Targets |
|---|---:|---:|---:|---|
| `mux-cli` | 1 | 97 | 2 | lib |
| `mux-client` | 1 | 479 | 0 | bin |
| `mux-core` | 9 | 1,995 | 28 | lib |
| `mux-platform` | 5 | 168 | 0 | lib |
| `mux-platform-unix` | 6 | 33 | 0 | lib |
| `mux-platform-windows` | 8 | 1,423 | 4 | lib |
| `mux-protocol` | 4 | 582 | 2 | lib |
| `mux-server` | 1 | 1,041 | 0 | bin |
| **Total** | **35** | **5,818** | **36** | **8 crates** |

Root project documents:

- `AGENTS.md`
- `docs/tmux-windows-rust-architecture-audit.md`
- `docs/windows-first-cross-os-execution-plan.md`
- `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md`
- `docs/superpowers/plans/2026-08-20-canonical-workspace-and-ci.md`

Generated output discovered at the root: `target/`, 6,183 files, 980.2 MiB.

### Canonical `wmux-clean/` workspace

| Crate | Rust files | Rust lines | Test attrs | Targets |
|---|---:|---:|---:|---|
| `wmux` (`crates/wmux-client`) | 1 | 853 | 8 | bin |
| `wmux-bench` | 3 | 1,590 | 4 | bin |
| `wmux-config` | 1 | 318 | 5 | lib |
| `wmux-conformance` | 2 | 383 | 2 | lib, bin |
| `wmux-core` | 11 | 8,284 | 83 | lib |
| `wmux-platform` | 1 | 212 | 1 | lib |
| `wmux-protocol` | 1 | 612 | 5 | lib |
| `wmux-server` | 2 | 2,622 | 20 | lib, bin |
| `wmux-windows` | 4 | 1,729 | 12 | lib |
| **Total** | **26** | **16,603** | **140** | **9 crates** |

Canonical design documents:

- `docs/client-backpressure-and-ipc.md`
- `docs/compact-grid.md`
- `docs/copy-mode.md`
- `docs/cross-os-conformance.md`
- `docs/event-contract.md`
- `docs/hybrid-rendering.md`
- `docs/lazy-scrollback-reflow.md`
- `docs/native-async-io.md`
- `docs/performance.md`
- `docs/performance-gates.md`
- `docs/resize-transactions.md`
- `docs/scrollback-and-mouse.md`
- `docs/terminal-batching-and-damage.md`
- `docs/windows-input.md`

Generated output discovered under `wmux-clean/`:

| Directory | Size (MiB) |
|---|---:|
| `target/` | 4,114.8 |
| `target-copy-verify/` | 480.5 |
| `target-ipc/` | 132.5 |
| `target-next/` | 646.7 |
| `target-perf/` | 138.3 |
| `target-render/` | 132.3 |
| `target-stable/` | 132.3 |
| **Total** | **5,777.4** |

## Legacy-only migration matrix

Items that contain behavior and must be reconsidered explicitly before the legacy tree can be retired:

| Legacy item | Evidence | Destination phase | Required treatment |
|---|---|---|---|
| Shared `mux-cli::CliCommand`/`parse_args` boundary | `crates/mux-cli/src/lib.rs` separates invocation classification from the client | Task 3, `windows-ipc-lifecycle-cli` | Recreate as the planned canonical `wmux-cli` crate under TDD; do not copy its silent fallback behavior. |
| `server start`, `server status`, `server stop`, `reset-terminal`/`reset-tty` client flows | Present in legacy CLI/client and absent from canonical CLI classification | Task 3 | Specify human-readable lifecycle/recovery behavior, test it, then implement it in the canonical CLI/client boundary. |
| `display-message`/`display` and `show-messages` server message history | Present in legacy core/server and absent from the canonical command enum | Tasks 4 and 8 | Carry the command names into the shared parser plan and add the server-owned message semantics with the nonvisual automation/configuration backbone. |

Root-only material that is preserved but does not itself require a code port:

| Legacy item | Classification |
|---|---|
| `mux-platform-unix` modules | Six marker structs totaling 33 lines; no PTY, socket, process, signal, or termios behavior. Use only as naming input for Task 6. |
| `JobObjectBackend`, `OverlappedIo`, `WindowsProcessBackend`, and `WindowsTerminalFeatures` | Empty marker structs. The canonical ConPTY implementation already owns real Job Object and overlapped I/O mechanics. |
| Modular legacy protocol file layout | Superseded by the larger canonical versioned protocol and its tests; file shape is not behavior. |
| Root architecture/audit documents | Retain as project history and compare against current plans; no product-code port. |

The canonical implementation is selected because it has the authoritative pane-owned screen/terminal engine, stable state stores, serialized owner loop, client-scoped rendering baselines, layouts/splits/copy mode, active Windows ConPTY and Job Object mechanics, versioned IPC, conformance tooling, and a release performance gate. The legacy workspace has no exclusive implementation that outweighs these capabilities.

## Reference model used

- Local Zellij keeps one declared Cargo workspace root and separates build, test, integration, and format checks in `.github/workflows/rust.yml`.
- Local tmux keeps generated configure/build/regression artifacts out of version control and executes regression gates from repository source in `.github/workflows/regress.yml`.
- wmux follows the same repository discipline while retaining its Windows-first, server-authoritative architecture from `AGENTS.md`.

## Execution environment

| Item | Value |
|---|---|
| OS | Microsoft Windows 11 Home Single Language, version 10.0.26200, build 26200 |
| Architecture | x64-based PC / 64-bit OS |
| Rust host | `x86_64-pc-windows-msvc` |
| `rustc` | 1.96.0 (`ac68faa20`, 2026-05-25), LLVM 22.1.2 |
| `cargo` | 1.96.0 (`30a34c682`, 2026-05-25) |
| Shell | Windows PowerShell 5.1.26100.9168, Desktop edition |
| Terminal evidence | `WT_SESSION` was present; no `TERM_PROGRAM` or ConEmu marker was present |

## Canonical Windows baseline commands

Run from `wmux-clean/`:

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo test --workspace
cargo run -p wmux-conformance --release
cargo run -p wmux-bench --release -- --suite full --gate
```

## In-place baseline results

All commands below ran from `wmux-clean/` on 2026-08-20 before the foundation commit.

| Gate | Result | Command wall time |
|---|---|---:|
| Formatting | Exit 0; no diff emitted | 0.414 s |
| Clippy | Exit 0 with `-D warnings`; dev profile finished | 0.639 s |
| Workspace tests | 140 passed, 0 failed, 0 ignored | 3.891 s |
| Release conformance | Exit 0; all fingerprints emitted | 2.352 s |
| Full release performance gate | Exit 0; `performance gate passed` | 0.571 s |

Executed test distribution:

| Target | Passed |
|---|---:|
| `wmux` client | 8 |
| `wmux-bench` | 4 |
| `wmux-config` | 5 |
| `wmux-conformance` | 2 |
| `wmux-core` | 83 |
| `wmux-platform` | 1 |
| `wmux-protocol` | 5 |
| `wmux-server` | 20 |
| `wmux-windows` | 12 |
| **Total** | **140** |

Conformance fingerprints:

```text
platform=windows
vt-replay-grid=9014d34392cc9931
detach-reattach-persistence=c66ce8e71d0d30db
multiple-client-consistency=05e713cca6822bb7
resize-reflow=cf663b0fc6092524
malformed-input-resilience=96f1c08eb989bee3
key-paste-behavior=63a3aa224a4d7943
mouse-mode-routing=cfb320142e88231b
suite=feb48e6303354e80
```

Release performance results:

| Scenario | Total ms | p50 us | p95 us | MiB/s | Alloc MiB | Peak MiB | Queue |
|---|---:|---:|---:|---:|---:|---:|---:|
| `parser-codex` | 12.393 | 0.000 | 0.000 | 161.01 | 2.63 | 1.35 | 0 |
| `parser-claude` | 12.094 | 0.000 | 0.000 | 166.29 | 2.64 | 1.35 | 0 |
| `frame-codex` | 19.543 | 44.100 | 68.800 | 102.10 | 44.15 | 1.42 | 0 |
| `frame-claude` | 20.403 | 44.200 | 73.500 | 98.57 | 44.16 | 1.42 | 0 |
| `hybrid-frame-codex` | 20.285 | 43.800 | 72.200 | 98.37 | 43.73 | 1.40 | 0 |
| `hybrid-frame-claude` | 21.913 | 45.300 | 75.600 | 91.77 | 43.74 | 1.40 | 0 |
| `scene-frame-codex` | 51.270 | 118.100 | 177.800 | 38.92 | 170.49 | 1.75 | 0 |
| `idle-input-render` | 0.792 | 1.400 | 3.900 | 0.48 | 1.38 | 0.04 | 0 |
| `damage-proportional` | 0.011 | 1.300 | 1.300 | 0.09 | 0.01 | 0.00 | 0 |
| `large-paste` | 0.331 | 0.000 | 0.900 | 193,470.37 | 0.00 | 0.00 | 0 |
| `history-resize-100k` | 17.014 | 18.400 | 29.400 | 100.89 | 8.92 | 0.02 | 0 |
| `split-storm` | 11.100 | 24.400 | 46.800 | 0.00 | 25.06 | 0.12 | 0 |
| `detach-backlog` | 51.424 | 0.000 | 0.000 | 155.57 | 101.46 | 1.47 | 8,192 |
| `multiple-clients` | 58.116 | 132.200 | 194.600 | 34.60 | 53.93 | 1.43 | 8 |

These performance values are a comparison baseline for the same machine and release command, not portable absolute guarantees.

## Clean-checkout reproduction

A fresh temporary checkout was created from the repository foundation source snapshot with `git clone --no-local`. Before any build, it contained both workspace manifests, both `Cargo.lock` files, and root CI; it contained zero `target*` directories and had a clean Git status.

All five gates then ran from the clone's `wmux-clean/` directory:

| Gate | Clean-checkout result | Command wall time |
|---|---|---:|
| Formatting | Exit 0 | 1.076 s |
| Clippy | Exit 0 with `-D warnings` | 11.028 s |
| Workspace tests | 140 passed, 0 failed, 0 ignored | 22.139 s |
| Release conformance | Exit 0; suite `feb48e6303354e80` | 4.643 s |
| Full release performance gate | Exit 0; `performance gate passed` | 3.322 s |

The clean clone emitted the same eight individual conformance fingerprints recorded above. Its benchmark timings varied normally from the warm in-place run while allocation/queue counters remained within the existing gate. No source, manifest, CI, or product file changed between the clean-clone proof and recording this result.

## Terminal and shell evidence boundary

Phase 1 automates the Windows build/test/conformance/performance baseline from Windows PowerShell. A present `WT_SESSION` indicates a Windows Terminal session for this run, but Phase 1 does not certify interactive attach behavior from the non-interactive gates alone.

No formatting, lint, unit-test, conformance, or performance-gate failure was observed in the in-place baseline. The limitations below are validation gaps, not failures observed by these commands.

The following interactive matrix remains unverified by Phase 1 and is intentionally carried to later lifecycle and beta-core gates:

- Windows Terminal, conhost, and VS Code integrated terminal.
- PowerShell 7, Windows PowerShell, and cmd.exe.
- Terminal mode restoration after every detach/crash path.
- Interactive vim/neovim, less, Git prompts, and high-volume application output.
