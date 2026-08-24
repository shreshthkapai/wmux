# Contributing to wmux

Thanks for helping build wmux. The project aims to be a serious persistent
terminal multiplexer with consistent semantics and native platform integration
across Windows, Linux, and macOS.

By participating, you agree to follow the
[Code of Conduct](CODE_OF_CONDUCT.md).

## Build from the repository root

```powershell
cargo build --locked
```

Rust 1.96.0 is pinned in `rust-toolchain.toml`.

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

Start with the user problem and wmux's documented contracts. Before changing
input, rendering, PTYs, keybindings, paste, resize, detach/attach,
sessions/windows/panes, layouts, commands, targets, or server lifecycle:

1. inspect the relevant wmux implementation, tests, and architecture docs;
2. consult official terminal, protocol, language, and platform documentation;
3. identify the cross-OS behavior and failure cases explicitly;
4. study prior art when useful, without treating another product as wmux's
   compatibility contract.

Record non-obvious decisions in the change and keep wmux terminology in public
documentation, tests, comments, and APIs.

## Development workflow

Keep changes focused and use clear commit messages. Add tests in proportion to
risk, with test-first regression coverage for bugs. Preserve existing user
changes in a dirty worktree and never commit local build output, runtime state,
credentials, keys, or `.agents/` content.

Minimum local checks from the repository root:

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
- the relevant wmux contract, standard, or official platform documentation;
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
