# wmux v1.0.0 Release and Distribution Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Rust implementation of wmux ready for a normal open-source
`v1.0.0` GitHub release with atomic client/server archives, one-command
installation, seamless updater-based upgrades, and verifiable provenance.

**Architecture:** Keep `wmux-clean/` as the canonical Cargo workspace and make
its `wmux` package own both distributable binaries while continuing to call the
separate `wmux-server` runtime library. A repository-root dist workspace drives
five native release targets, installers, checksums, the updater, and GitHub
attestations. Human-facing OSS policy lives at the repository root, while
automated dependency and published-archive checks protect the release path.

**Tech Stack:** Rust 1.96.0, Cargo, dist 0.32.0, GitHub Actions, cargo-deny
0.20.2, PowerShell 7, SHA-256, GitHub artifact attestations.

**Spec:** `docs/superpowers/specs/2026-08-23-v1-release-distribution-design.md`

## Global Constraints

- Work directly on `main` and commit each completed task; do not create a
  worktree or feature branch.
- Do not use subagents. Execute this plan inline with
  `superpowers:executing-plans`.
- Preserve the user's untracked `.agents/` directory and exclude it from every
  commit.
- Treat `wmux-clean/` as the only product workspace. Do not rename, delete, or
  flatten either workspace in this work.
- Set the public version to `1.0.0` and release tag to `v1.0.0`; publish a
  normal GitHub release, not a prerelease.
- Distribute exactly `wmux` and `wmux-server` as one application package.
- Build Windows x64, Linux x64 musl, Linux ARM64 musl, macOS Intel, and macOS
  Apple Silicon artifacts.
- Pin Rust to `1.96.0`, dist to `0.32.0`, and cargo-deny to the 0.20.2 version
  supplied by `EmbarkStudios/cargo-deny-action@v2.1.1`.
- Keep all crates non-publishable to crates.io; v1 is distributed through
  GitHub Releases.
- Ship unsigned executables with SHA-256 checksums and GitHub attestations.
- Do not configure a remote, push, tag, publish, rewrite GitHub history, or
  alter repository settings. Those are explicit maintainer cutover actions.
- Research changes against local tmux/Zellij and official platform/tooling
  documentation as required by `AGENTS.md`.

---

## File Map

- `wmux-clean/Cargo.toml`: shared v1 metadata, non-publishable policy, and the
  dist build profile.
- `wmux-clean/rust-toolchain.toml`: local and CI Rust 1.96.0 pin.
- `wmux-clean/crates/*/Cargo.toml`: inherited metadata and explicit
  non-publishable package declarations.
- `wmux-clean/crates/wmux-client/src/server_main.rs`: the packaged
  `wmux-server` launcher, still delegating semantics to `wmux-server`.
- `wmux-clean/crates/wmux-client/tests/distribution_contract.rs`: executable
  pairing and version-output regression contract.
- `README.md`, `LICENSE`, `CHANGELOG.md`, `CONTRIBUTING.md`,
  `CODE_OF_CONDUCT.md`, `SECURITY.md`: public project entry points and policy.
- `docs/RELEASING.md`: maintainer preflight, cutover, validation, and recovery
  runbook.
- `.github/ISSUE_TEMPLATE/*`, `.github/pull_request_template.md`, and
  `.github/dependabot.yml`: contributor intake and dependency maintenance.
- `deny.toml` and `.github/workflows/supply-chain.yml`: advisory, license,
  duplicate-dependency, source, and release-plan gates.
- `dist-workspace.toml` and `.github/workflows/release.yml`: dist source config
  and generated release orchestration.
- `scripts/verify-release-archives.ps1` and
  `.github/workflows/release-assets.yml`: archive-content/checksum validation
  for all five published targets plus native Linux version smoke tests.

---

### Task 1: Make the `wmux` Cargo package an atomic v1 application

**Files:**

- Modify: `wmux-clean/Cargo.toml`
- Modify: every `wmux-clean/crates/*/Cargo.toml`
- Modify: `.github/workflows/beta-core.yml`
- Create: `wmux-clean/rust-toolchain.toml`
- Create: `wmux-clean/crates/wmux-client/src/server_main.rs`
- Create: `wmux-clean/crates/wmux-client/tests/distribution_contract.rs`
- Delete: `wmux-clean/crates/wmux-server/src/main.rs`

**Interfaces:**

- Consumes: `wmux_server::run_with_platform(Box<dyn ServerPlatform>)` and the
  existing OS-specific server-platform constructors.
- Produces: Cargo package `wmux` version `1.0.0` with binary targets `wmux` and
  `wmux-server`; both accept `--version` and print their exact package version.

- [ ] **Step 1: Write the failing distribution contract**

Create `wmux-clean/crates/wmux-client/tests/distribution_contract.rs`:

```rust
use std::process::Command;

#[test]
fn package_builds_versioned_client_and_server_binaries() {
    for (binary, name) in [
        (env!("CARGO_BIN_EXE_wmux"), "wmux"),
        (env!("CARGO_BIN_EXE_wmux-server"), "wmux-server"),
    ] {
        let output = Command::new(binary)
            .arg("--version")
            .output()
            .expect("packaged binary starts");
        assert!(output.status.success(), "{name} --version failed");
        assert_eq!(
            String::from_utf8(output.stdout).unwrap(),
            format!("{name} {}\n", env!("CARGO_PKG_VERSION"))
        );
    }
}
```

- [ ] **Step 2: Run the contract and verify the package boundary fails**

Run:

```powershell
cargo test -p wmux --test distribution_contract --no-run
```

Expected: compilation fails because package `wmux` does not yet define
`CARGO_BIN_EXE_wmux-server`.

- [ ] **Step 3: Set shared v1 metadata and the pinned toolchain**

Change `[workspace.package]` in `wmux-clean/Cargo.toml` to include:

```toml
version = "1.0.0"
edition = "2021"
rust-version = "1.96"
license = "MIT"
authors = ["Shreshth Kapai"]
description = "A persistent cross-platform terminal multiplexer"
repository = "https://github.com/shreshthkapai/wmux"
homepage = "https://github.com/shreshthkapai/wmux"
publish = false
```

Add:

```toml
[profile.dist]
inherits = "release"
lto = "thin"
```

Create `wmux-clean/rust-toolchain.toml`:

```toml
[toolchain]
channel = "1.96.0"
components = ["clippy", "rustfmt"]
profile = "minimal"
```

For every workspace package, add the applicable inherited declarations:

```toml
rust-version.workspace = true
authors.workspace = true
description.workspace = true
repository.workspace = true
homepage.workspace = true
publish.workspace = true
```

Keep the fuzz workspace at `0.0.0` and `publish = false`.

- [ ] **Step 4: Move binary ownership without moving server semantics**

In `wmux-clean/crates/wmux-client/Cargo.toml`, add:

```toml
[[bin]]
name = "wmux-server"
path = "src/server_main.rs"

[dependencies]
wmux-server = { path = "../wmux-server" }
```

Create `server_main.rs` from the existing server launcher, adding this local
version path before platform startup:

```rust
fn main() -> std::io::Result<()> {
    if std::env::args_os().nth(1).as_deref() == Some(std::ffi::OsStr::new("--version")) {
        println!("wmux-server {}", env!("CARGO_PKG_VERSION"));
        return Ok(());
    }

    run()
}
```

Keep the existing `#[cfg(windows)]`, `#[cfg(unix)]`, and unsupported-platform
branches in `run()`. Remove the `[[bin]]` declaration and `src/main.rs` from
the `wmux-server` package; retain its library and all runtime dependencies.

- [ ] **Step 5: Update CI to check the atomic package**

In `.github/workflows/beta-core.yml`, replace:

```yaml
run: cargo check --locked -p wmux -p wmux-server --bins
```

with:

```yaml
run: cargo check --locked -p wmux --bins
```

- [ ] **Step 6: Refresh locks and make the contract green**

Run:

```powershell
cargo check --workspace --all-targets
cargo test -p wmux --test distribution_contract
cargo test -p wmux-server --lib
cargo metadata --locked --format-version 1 --no-deps
```

Expected: both version assertions pass; metadata lists `wmux` and
`wmux-server` as binary targets of package `wmux`, and the server package has
only its library target.

- [ ] **Step 7: Verify and commit the atomic package**

Run:

```powershell
cargo fmt --all -- --check
cargo clippy -p wmux -p wmux-server --all-targets --locked -- -D warnings
cargo test -p wmux -p wmux-server --all-targets --locked
git -c safe.directory=C:/Users/shres/mux/wmux diff --check
```

Commit:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux add wmux-clean/Cargo.toml wmux-clean/Cargo.lock wmux-clean/rust-toolchain.toml wmux-clean/crates .github/workflows/beta-core.yml
git -c safe.directory=C:/Users/shres/mux/wmux commit -m "build: package wmux client and server atomically"
```

---

### Task 2: Add the public open-source project surface

**Files:**

- Create: `README.md`
- Create: `LICENSE`
- Create: `CHANGELOG.md`
- Create: `CONTRIBUTING.md`
- Create: `CODE_OF_CONDUCT.md`
- Create: `SECURITY.md`
- Create: `docs/RELEASING.md`

**Interfaces:**

- Consumes: the `v1.0.0` version contract, confirmed repository URL, five
  supported targets, existing short CLI commands, and existing key model.
- Produces: the canonical installation/update instructions and maintainer
  release procedure referenced by GitHub Releases and packaged archives.

- [ ] **Step 1: Create the MIT license and v1 changelog**

Use the standard MIT license text with:

```text
Copyright (c) 2026 Shreshth Kapai
```

Create `CHANGELOG.md` with `Unreleased` and `1.0.0 - 2026-08-23` sections. The
v1 entry must name persistent sessions, Windows ConPTY, Unix PTYs, versioned
IPC, sessions/windows/panes, splits, key tables, commands, copy/paste,
configuration/hooks/jobs/control mode, theme-native rendering, and the five
release targets. Record the unsigned-binary limitation and lack of live server
migration under `Known limitations`.

- [ ] **Step 2: Write the README around installation and first success**

Use these exact installer commands:

```powershell
powershell -ExecutionPolicy Bypass -c "irm https://github.com/shreshthkapai/wmux/releases/latest/download/wmux-installer.ps1 | iex"
```

```sh
curl --proto '=https' --tlsv1.2 -LsSf https://github.com/shreshthkapai/wmux/releases/latest/download/wmux-installer.sh | sh
```

Show this quick start:

```text
wmux new -s demo
wmux attach -t demo
wmux ls
```

Include: product promise; supported platform table; both installers; manual
archive/checksum/attestation verification; quick start; default `C-b`
keybindings including `C-b X`; update instructions using `wmux-update` or the
installer again; the requirement to end active sessions before updating;
unsigned Windows/macOS trust-prompt disclosure; links to compatibility,
known-difference, architecture, contribution, security, and conduct docs; and
a prominent note that `wmux-clean/` is canonical until the separately planned
repository move.

- [ ] **Step 3: Add contributor, conduct, and security policies**

`CONTRIBUTING.md` must require work from `wmux-clean/`, the boundaries in
`AGENTS.md`, research against local tmux/Zellij, formatting, Clippy with
warnings denied, workspace tests, platform-impact notes, and focused commits.

Use Contributor Covenant 2.1 in `CODE_OF_CONDUCT.md`, with enforcement contact
directed to GitHub private reporting rather than inventing a public email.

`SECURITY.md` must list `1.x` as supported, older versions as unsupported,
direct reporters to GitHub private vulnerability reporting, forbid public
security issues, request reproduction/impact/platform details, and state that
receipt and remediation timing depends on severity.

- [ ] **Step 4: Write the maintainer release runbook**

`docs/RELEASING.md` must define:

1. clean-tree and version/changelog preflight;
2. the complete local verification command set from Task 6;
3. `dist plan --tag=v1.0.0` and local Windows artifact inspection;
4. checking green hosted Windows/Linux/macOS CI;
5. the separate GitHub history cutover decision and recovery reference;
6. creating an annotated `v1.0.0` tag only after cutover approval;
7. waiting for release and release-assets workflows;
8. clean-host PowerShell and shell installer smoke tests;
9. enabling immutable releases only after all assets are present;
10. never moving a published tag or replacing immutable assets; use `1.0.1`
    for fixes.

- [ ] **Step 5: Verify links and commit the OSS surface**

Run:

```powershell
rg -n "wmux-clean|wmux new -s demo|wmux-update|v1.0.0|1.0.0" README.md CHANGELOG.md CONTRIBUTING.md SECURITY.md docs/RELEASING.md
rg -n "0.1.0-beta|v0.1.0|GitHub prerelease" README.md CHANGELOG.md CONTRIBUTING.md SECURITY.md docs/RELEASING.md
git -c safe.directory=C:/Users/shres/mux/wmux diff --check
```

Expected: the first search covers every release contract; the second search
has no matches.

Commit:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux add README.md LICENSE CHANGELOG.md CONTRIBUTING.md CODE_OF_CONDUCT.md SECURITY.md docs/RELEASING.md
git -c safe.directory=C:/Users/shres/mux/wmux commit -m "docs: add the wmux open source release surface"
```

---

### Task 3: Add contributor automation and dependency policy

**Files:**

- Create: `.github/ISSUE_TEMPLATE/bug.yml`
- Create: `.github/ISSUE_TEMPLATE/feature.yml`
- Create: `.github/ISSUE_TEMPLATE/config.yml`
- Create: `.github/pull_request_template.md`
- Create: `.github/dependabot.yml`
- Create: `deny.toml`
- Create: `.github/workflows/supply-chain.yml`

**Interfaces:**

- Consumes: canonical workspace path `wmux-clean/Cargo.toml`, Rust 1.96.0,
  and the five supported targets.
- Produces: structured reports, weekly dependency PRs, and a blocking policy
  check for licenses, registries, Git sources, and yanked dependencies.

- [ ] **Step 1: Add structured issue and pull-request templates**

The bug form must require wmux version, OS/architecture, terminal host, shell,
reproduction commands, expected/actual behavior, and logs with a secret-data
warning. The feature form must require user problem, proposed tmux/Zellij
precedent, cross-OS semantics, and alternatives. Disable blank issues and link
security reports to the repository security-advisory page.

The PR template must check architecture boundaries, reference research,
tests, docs, Windows/Linux/macOS impact, performance impact, and absence of
generated artifacts or secrets.

- [ ] **Step 2: Configure weekly dependency updates**

Create `.github/dependabot.yml` with version `2`, weekly Cargo updates rooted
at `/wmux-clean`, weekly GitHub Actions updates rooted at `/`, a limit of five
open PRs for each ecosystem, and labels `dependencies` and `rust` or `ci`.

- [ ] **Step 3: Add the exact dependency policy**

Create `deny.toml`:

```toml
[graph]
all-features = true
targets = [
    "x86_64-pc-windows-msvc",
    "x86_64-unknown-linux-musl",
    "aarch64-unknown-linux-musl",
    "x86_64-apple-darwin",
    "aarch64-apple-darwin",
]

[advisories]
version = 2
yanked = "deny"

[licenses]
version = 2
confidence-threshold = 0.93
allow = ["Apache-2.0", "MIT", "Unicode-3.0", "Unlicense"]

[bans]
multiple-versions = "warn"
wildcards = "deny"
highlight = "all"

[sources]
unknown-registry = "deny"
unknown-git = "deny"
allow-registry = ["https://github.com/rust-lang/crates.io-index"]
```

Do not add advisory, crate, or license exceptions merely to make the command
green. Investigate each diagnostic and document any genuinely required
exception in the same commit.

- [ ] **Step 4: Add the supply-chain workflow**

Create `.github/workflows/supply-chain.yml` with read-only contents
permissions and one dependency-policy job using:

```yaml
- uses: actions/checkout@v6
- uses: EmbarkStudios/cargo-deny-action@v2.1.1
  with:
    rust-version: "1.96.0"
    manifest-path: ./wmux-clean/Cargo.toml
    command: check
    arguments: --all-features
```

Add manifests, lockfiles, `deny.toml`, and this workflow to its path filters
while retaining `workflow_dispatch`. The dist-plan job is added in Task 4 only
after its configuration exists.

- [ ] **Step 5: Run the policy locally and commit**

Install the pinned checker if absent, then run:

```powershell
cargo install cargo-deny --version 0.20.2 --locked
cargo deny --manifest-path wmux-clean/Cargo.toml check
git -c safe.directory=C:/Users/shres/mux/wmux diff --check
```

Expected: advisories, bans, licenses, and sources all complete without denied
diagnostics.

Commit:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux add .github deny.toml
git -c safe.directory=C:/Users/shres/mux/wmux commit -m "ci: add open source dependency policy"
```

---

### Task 4: Configure dist installers, updates, and release CI

**Files:**

- Create: `dist-workspace.toml`
- Generate: `.github/workflows/release.yml`
- Modify: `.github/workflows/supply-chain.yml`
- Modify: `README.md`
- Modify: `docs/RELEASING.md`

**Interfaces:**

- Consumes: package `wmux` with binaries `wmux` and `wmux-server`, repository
  URL, root OSS documents, and the normal `v1.0.0` release contract.
- Produces: five archives, SHA-256 sidecars, shell and PowerShell installers,
  `wmux-update`, a dist manifest, and GitHub attestations.

- [ ] **Step 1: Create the root dist workspace**

Create `dist-workspace.toml`:

```toml
[workspace]
members = ["cargo:wmux-clean/"]

[dist]
cargo-dist-version = "0.32.0"
ci = "github"
pr-run-mode = "plan"
hosting = ["github"]
packages = ["wmux"]
targets = [
    "x86_64-pc-windows-msvc",
    "x86_64-unknown-linux-musl",
    "aarch64-unknown-linux-musl",
    "x86_64-apple-darwin",
    "aarch64-apple-darwin",
]
installers = ["shell", "powershell"]
install-path = "CARGO_HOME"
install-updater = true
checksum = "sha256"
unix-archive = ".tar.gz"
windows-archive = ".zip"
include = ["README.md", "LICENSE", "CHANGELOG.md"]
github-attestations = true
```

Do not set `force-latest`; `1.0.0` is already a normal SemVer release.

- [ ] **Step 2: Install the pinned generator and inspect the plan**

Run the official dist installer, then:

```powershell
dist --version
dist plan --tag=v1.0.0
```

Expected: dist reports `0.32.0`, exactly one application named `wmux`, two
binaries in every target archive, five targets, both installers, updater,
checksums, and attestations.

- [ ] **Step 3: Generate release CI and verify reproducibility**

Run:

```powershell
dist generate
dist generate --check
dist plan --tag=v1.0.0
```

Inspect `.github/workflows/release.yml`; it must trigger on `v*` tags, retain
dist-generated warnings, request only required release/attestation
permissions, build every configured target, and publish only after all builds
succeed. Do not hand-edit generated behavior; change `dist-workspace.toml` and
regenerate instead.

Add a second job to `.github/workflows/supply-chain.yml`. It checks out the
repository, installs dist 0.32.0 with the official installer, runs
`dist plan --tag=v1.0.0`, and runs `dist generate --check`. Add
`dist-workspace.toml` and `.github/workflows/release.yml` to that workflow's
path filters.

- [ ] **Step 4: Reconcile generated asset names with public docs**

Read the plan output and generated manifest names. Confirm the README installer
URLs end in `wmux-installer.ps1` and `wmux-installer.sh`, and the updater is
`wmux-update`. If dist emits different deterministic names, update README and
`docs/RELEASING.md` to the emitted names before committing.

- [ ] **Step 5: Commit the reproducible release definition**

Run:

```powershell
dist generate --check
dist plan --tag=v1.0.0
git -c safe.directory=C:/Users/shres/mux/wmux diff --check
```

Commit:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux add dist-workspace.toml .github/workflows/release.yml .github/workflows/supply-chain.yml README.md docs/RELEASING.md
git -c safe.directory=C:/Users/shres/mux/wmux commit -m "ci: add reproducible v1 release distribution"
```

---

### Task 5: Verify every published archive and checksum

**Files:**

- Create: `scripts/verify-release-archives.ps1`
- Create: `.github/workflows/release-assets.yml`
- Modify: `docs/RELEASING.md`

**Interfaces:**

- Consumes: a directory containing the GitHub release assets generated by
  dist for tag `v1.0.0`.
- Produces: a nonzero exit for missing targets, duplicate target archives,
  absent client/server binaries, absent README/LICENSE/changelog, or invalid
  SHA-256 sidecars.

- [ ] **Step 1: Write failing fixture checks for the archive verifier**

Before creating the verifier, make temporary good and bad fixture archives in
a directory returned by `New-Item -ItemType Directory` under `$env:TEMP`.
Each good target archive contains the correct platform binary names plus
`README.md`, `LICENSE`, and `CHANGELOG.md`; the bad Windows archive omits
`wmux-server.exe`.

Run the not-yet-created script against the fixture directory:

```powershell
pwsh -NoProfile -File scripts/verify-release-archives.ps1 -ArtifactsDirectory $fixtureRoot
```

Expected: failure because the verifier does not exist. After the script is
created, retain the fixture commands in a comment-based example in the script
and verify the bad fixture fails specifically with `missing wmux-server.exe`.

- [ ] **Step 2: Implement the portable PowerShell verifier**

The script must use `[System.IO.Compression.ZipFile]::OpenRead()` for ZIP files
and `tar -tf` for `.tar.gz` files. Discover exactly one archive containing each
target triple. Normalize entries to leaf filenames and require:

```powershell
$targets = @{
    'x86_64-pc-windows-msvc' = @('wmux.exe', 'wmux-server.exe')
    'x86_64-unknown-linux-musl' = @('wmux', 'wmux-server')
    'aarch64-unknown-linux-musl' = @('wmux', 'wmux-server')
    'x86_64-apple-darwin' = @('wmux', 'wmux-server')
    'aarch64-apple-darwin' = @('wmux', 'wmux-server')
}
$documents = @('README.md', 'LICENSE', 'CHANGELOG.md')
```

For each archive, require a `.sha256` sidecar and compare its first hash token
to `(Get-FileHash -Algorithm SHA256).Hash.ToLowerInvariant()`. Print one
success line per target and exit zero only after all five pass.

- [ ] **Step 3: Make the fixture cycle green**

Run the verifier against the good fixture directory and then the bad fixture
directory. Expected: good exits zero with five target lines; bad exits nonzero
and names the missing server executable. Remove the temporary fixtures after
the assertions.

- [ ] **Step 4: Add the published-release smoke workflow**

Create `.github/workflows/release-assets.yml` triggered by
`release: { types: [published] }` and `workflow_dispatch` with a required tag
input. Grant read-only contents permission. On `ubuntu-latest`:

1. check out the exact release tag;
2. download all assets for that tag with `gh release download` into
   `artifacts/` using `GH_TOKEN: ${{ github.token }}`;
3. run the PowerShell verifier;
4. derive the expected version by removing the leading `v` from the release
   tag;
5. extract the x86_64 Linux musl archive;
6. assert `wmux --version` equals `wmux <derived-version>`;
7. assert `wmux-server --version` equals
   `wmux-server <derived-version>`.

The release runbook must require this workflow to pass before enabling GitHub
immutable releases.

- [ ] **Step 5: Verify and commit the release-asset gate**

Run:

```powershell
pwsh -NoProfile -File scripts/verify-release-archives.ps1 -ArtifactsDirectory $goodFixtureRoot
git -c safe.directory=C:/Users/shres/mux/wmux diff --check
```

Commit:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux add scripts/verify-release-archives.ps1 .github/workflows/release-assets.yml docs/RELEASING.md
git -c safe.directory=C:/Users/shres/mux/wmux commit -m "ci: verify published release archives"
```

---

### Task 6: Run the v1 release-candidate verification gate

**Files:**

- Modify only files required to correct failures found by the commands below.
- Record evidence in `docs/RELEASING.md` only if the runbook needs a corrected
  command; do not paste transient timestamps or local paths into the repo.

**Interfaces:**

- Consumes: all preceding tasks.
- Produces: a locally verified release-candidate commit on `main`; it does not
  publish or mutate GitHub.

- [ ] **Step 1: Verify formatting, lint, tests, and fuzz compilation**

Run from `wmux-clean/`:

```powershell
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked -- -D warnings
cargo test --workspace --all-targets --locked
cargo check --locked --manifest-path fuzz/Cargo.toml --bins
cargo clippy --locked --manifest-path fuzz/Cargo.toml --bins -- -D warnings
```

Expected: every command exits zero with no warnings promoted to errors.

- [ ] **Step 2: Verify semantics, stress, and performance**

Run from `wmux-clean/`:

```powershell
cargo run --locked -p wmux-conformance --release
cargo run --locked -p wmux-stress --release -- --profile full
cargo run --locked -p wmux-bench --release -- --suite full --gate
```

Expected: conformance fingerprint matches the accepted repository value,
stress completes without invariant failures, and all release performance
thresholds pass.

- [ ] **Step 3: Verify cross-platform compilation and policy**

Run:

```powershell
rustup target add x86_64-apple-darwin aarch64-apple-darwin
cargo check --locked --manifest-path wmux-clean/Cargo.toml -p wmux --bins --target x86_64-apple-darwin
cargo check --locked --manifest-path wmux-clean/Cargo.toml -p wmux --bins --target aarch64-apple-darwin
cargo deny --manifest-path wmux-clean/Cargo.toml check
dist generate --check
dist plan --tag=v1.0.0
```

Expected: both macOS targets compile, dependency policy is green, generated CI
is current, and the release plan contains all five two-binary archives.

- [ ] **Step 4: Build and inspect the native Windows release archive**

Run:

```powershell
dist build --tag=v1.0.0 --target=x86_64-pc-windows-msvc
```

Run the archive verifier against dist's artifact output after copying or
downloading fixture archives for the other four planned targets. Extract the
Windows archive and assert:

```powershell
& .\wmux.exe --version
& .\wmux-server.exe --version
```

Expected output is exactly `wmux 1.0.0` and `wmux-server 1.0.0`.

- [ ] **Step 5: Audit the repository boundary**

Run from the repository root:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux diff --check
git -c safe.directory=C:/Users/shres/mux/wmux status --short --branch
git -c safe.directory=C:/Users/shres/mux/wmux ls-files | rg '(^|/)(target|dist|artifacts|packages)/|\.(pfx|p12|key|pem)$'
rg -n -i 'password\s*=|token\s*=|secret\s*=|private key' --glob '!Cargo.lock' --glob '!.agents/**'
```

Expected: branch is `main`; no product changes remain uncommitted; `.agents/`
is the only unrelated untracked path; no build output, credential, key, or
runtime-state file is tracked; and secret-pattern matches are only intentional
documentation examples, if any.

- [ ] **Step 6: Commit any verification-only corrections**

If verification required repository changes, re-run the affected complete
gate and commit only those changes. Use explicit verified paths instead of a
broad add:

```powershell
git -c safe.directory=C:/Users/shres/mux/wmux add README.md docs/RELEASING.md
git -c safe.directory=C:/Users/shres/mux/wmux commit -m "chore: finish v1 release preparation"
```

Do not create an empty commit when no corrections were required.

- [ ] **Step 7: Report local completion and external cutover work**

Report exact command evidence, commit IDs, and the remaining external actions:
GitHub history cutover approval, remote configuration/push, hosted CI on all
three OS families, `v1.0.0` tag creation, published installer smoke tests, and
immutable-release enablement. Do not describe macOS runtime as verified from a
Windows cross-check.
