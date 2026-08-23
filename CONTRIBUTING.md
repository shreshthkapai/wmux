# Contributing to wmux

Thanks for helping build wmux. The project aims to be a serious persistent
terminal multiplexer, with tmux semantics where they are OS-independent and
native platform behavior where they are not.

By participating, you agree to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Start in the canonical workspace

The product currently lives in `wmux-clean/`. The root `crates/` tree is a
historical implementation and must not receive product changes. This temporary
layout will be removed only through a separately reviewed repository move.

```powershell
Set-Location wmux-clean
cargo build --locked
```

Rust 1.96.0 is pinned in `wmux-clean/rust-toolchain.toml`.

## Architecture rules

Read [AGENTS.md](AGENTS.md) before designing or changing behavior. In
particular:

- core crates must not import Windows or Unix APIs;
- the server is the only authority for multiplexer state;
- clients are disposable views;
- panes own virtual terminal state in core, not platform handles;
- all mutations pass through the serialized server command queue;
- IPC remains versioned and bounded;
- platform backends expose OS-neutral semantic events;
- IO and rendering stay chunked, coalesced, and allocation-conscious.

Changes to these boundaries require matching architecture documentation and
tests.

## Research before changing behavior

tmux and Zellij are first-class reference implementations. Before a feature,
fix, or improvement involving input, rendering, PTYs, keybindings, paste,
resize, detach/attach, sessions/windows/panes, layouts, commands, targets, or
server lifecycle:

1. inspect the local `../tmux` implementation for semantics;
2. inspect the local `../zellij` implementation for Rust architecture and
   terminal handling;
3. consult official platform documentation for OS mechanics;
4. read the relevant wmux architecture and execution-plan documents.

Record the adopted model in the change when the choice is not obvious.

## Development workflow

Keep changes focused and use clear commit messages. Add tests in proportion to
risk, with test-first regression coverage for bugs. Preserve existing user
changes in a dirty worktree and never commit local build output, runtime state,
credentials, keys, or `.agents/` content.

Minimum local checks from `wmux-clean/`:

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
cargo check --locked --manifest-path fuzz/Cargo.toml --bins
cargo clippy --locked --manifest-path fuzz/Cargo.toml --bins -- -D warnings
```

Behavioral or performance-sensitive changes should also run:

```powershell
cargo run --locked -p wmux-conformance --release
cargo run --locked -p wmux-stress --release -- --profile full
cargo run --locked -p wmux-bench --release -- --suite full --gate
```

Platform work must include native tests where possible. Cross-compilation is
useful evidence but must not be described as native runtime verification.

## Pull requests

Explain:

- the user-visible problem and intended behavior;
- the tmux/Zellij or official-platform precedent;
- architecture and data-flow impact;
- tests and commands run;
- Windows, Linux, and macOS impact;
- performance impact for hot paths;
- documentation or compatibility changes.

Keep generated release files synchronized with their source configuration.
Never hand-edit `.github/workflows/release.yml`; change
`dist-workspace.toml` and run `dist generate`.

## Security

Do not disclose suspected vulnerabilities in issues, discussions, or pull
requests. Follow [SECURITY.md](SECURITY.md) for private reporting.

## License

Unless explicitly stated otherwise, contributions are licensed under the
project's [MIT License](LICENSE).
