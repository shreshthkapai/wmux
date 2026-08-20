# Canonical Workspace and CI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish `wmux-clean/` as wmux's single authoritative workspace, preserve the legacy root workspace safely, and make the Windows baseline reproducible from a clean Git checkout.

**Architecture:** The outer `wmux/` directory becomes the repository boundary while `wmux-clean/` remains the canonical Cargo build root used by CI and all later roadmap phases. The legacy root `Cargo.toml` and `crates/` remain read-only migration input until their unique behavior is explicitly ported or rejected; no source tree is moved or deleted in this phase.

**Tech Stack:** Rust 2021, Cargo workspace tooling, PowerShell, Git, GitHub Actions on `windows-latest`, and the existing `wmux-conformance`/`wmux-bench` gates.

**Spec:** `docs/superpowers/plans/2026-08-20-cross-platform-beta-completion.md`

## Global Constraints

- Preserve both the legacy root workspace and `wmux-clean/` throughout Phase 1.
- Treat `wmux-clean/` as canonical for builds, tests, CI, and future implementation.
- Keep `Cargo.lock` files tracked; ignore Cargo output directories, transient logs, runtime lock/PID files, and packaged artifacts.
- Do not modify pane rendering, UI/UX, terminal colours, or terminal-theme inheritance.
- Do not claim a terminal/shell combination is supported unless this phase has evidence for it.
- Follow the local tmux and Zellij repository/CI model: one declared repository root, generated artifacts ignored, and explicit repeatable gates.

---

### Task 1: Record the Workspace Inventory and Canonical Decision

**Files:**
- Create: `docs/baseline/cross-platform-beta-baseline.md`
- Create: `docs/baseline/safe-workspace-consolidation.md`

**Interfaces:**
- Consumes: both Cargo manifests, crate/source inventories, test attributes, documentation, generated output directories, and the approved beta roadmap.
- Produces: a canonical-workspace declaration, a legacy-to-canonical migration matrix, and non-destructive consolidation gates used by every later phase.

- [x] **Step 1: Record the measured inventory**

  Add tables listing each crate, Rust source-file count, Rust line count, test-attribute count, documentation files, and generated Cargo directories for both workspaces. Record the discovery values: legacy root has 8 crates/36 test attributes; `wmux-clean/` has 9 crates/140 test attributes.

- [x] **Step 2: Classify root-only material**

  Record `mux-cli` as behavior to reconsider in Phase 3's shared `wmux-cli`, the Unix crate as placeholder-only input for Phase 6, the older Windows helper modules as superseded mechanics that must not be copied without a behavior gap, and the root architecture documents as retained project records.

- [x] **Step 3: Declare the canonical build root**

  State that all build commands execute from `wmux-clean/`, all new product source changes land under `wmux-clean/`, and root `crates/` receives no new product work.

- [x] **Step 4: Write the safe consolidation stages**

  Define: preserve both trees now; port only explicitly accepted root-only behavior under TDD in its assigned roadmap phase; verify canonical CI and a clean checkout; request explicit approval before removing or archiving the legacy tree. State that physical flattening is not part of Phase 1.

- [x] **Step 5: Review the documents against the master Task 1 checklist**

  Confirm the documents answer which workspace is authoritative, which root-only items remain, what may be removed later, and which evidence is still missing.

---

### Task 2: Establish the Git Boundary and Artifact Policy

**Files:**
- Modify: `.gitignore`
- Initialize: `.git/` at the outer `wmux/` directory

**Interfaces:**
- Consumes: the empty, unusable `.git/` directory and generated directories discovered in Task 1.
- Produces: a valid repository boundary whose tracked tree includes source and lockfiles but excludes reproducible/transient artifacts.

- [x] **Step 1: Initialize the repository boundary**

  Run `git init -b main .` from the outer `wmux/` directory. Do not add a remote and do not rewrite any existing history because no valid history exists in the supplied `.git/` directory.

- [x] **Step 2: Verify the current ignore policy is insufficient**

  Run `git check-ignore wmux-clean/target-perf/probe`; expect exit code 1 because the current `/target/` rule does not cover nested `target-*` directories.

- [x] **Step 3: Replace the ignore policy**

  Use these repository-root categories: `**/target/`, `**/target-*/`, `*.log`, `*.log.*`, `*.pid`, `*.tmp`, `wmux*.lock`, `/dist/`, `/artifacts/`, `/packages/`, and `/.worktrees/`. Do not add `*.lock`, because both Cargo lockfiles must remain trackable.

- [x] **Step 4: Verify ignored and tracked cases**

  Run `git check-ignore` for `target/probe`, `wmux-clean/target/probe`, `wmux-clean/target-perf/probe`, `server.log`, `wmux-server.lock`, and `dist/wmux.exe`; require exit code 0 for each. Run `git check-ignore Cargo.lock wmux-clean/Cargo.lock`; require exit code 1 for both.

- [x] **Step 5: Inspect the complete pre-commit file set**

  Run `git status --short --untracked-files=all` and confirm no file under any `target*` directory and no binary/log/runtime artifact is listed.

---

### Task 3: Install the Root CI Foundation

**Files:**
- Create: `.github/workflows/ci.yml`
- Retain: `wmux-clean/.github/workflows/conformance.yml` as historical input until a later explicit cleanup

**Interfaces:**
- Consumes: the canonical `wmux-clean/Cargo.toml` and the five baseline commands from the master plan.
- Produces: GitHub Actions jobs that execute from `wmux-clean/` and enforce portable semantics plus the complete Windows baseline.

- [x] **Step 1: Create a root-discoverable workflow**

  Define `push`, `pull_request`, and `workflow_dispatch` triggers; `permissions: contents: read`; `CARGO_TERM_COLOR: always`; and job-level `defaults.run.working-directory: wmux-clean`.

- [x] **Step 2: Preserve portable semantic coverage**

  Add a `portable-semantics` matrix for `windows-latest`, `ubuntu-latest`, and `macos-latest`. Check out with `actions/checkout@v4`, install the recorded Rust toolchain with rustfmt/clippy, run `cargo run -p wmux-conformance --release`, and run tests for `wmux-core`, `wmux-platform`, `wmux-protocol`, and `wmux-conformance`.

- [x] **Step 3: Add the Windows baseline job**

  On `windows-latest`, run in order: `cargo fmt --all -- --check`, `cargo clippy --workspace --all-targets -- -D warnings`, `cargo test --workspace`, `cargo run -p wmux-conformance --release`, and `cargo run -p wmux-bench --release -- --suite full --gate`.

- [x] **Step 4: Validate workflow structure locally**

  Parse `.github/workflows/ci.yml` as text and verify it has two jobs, both jobs set `working-directory: wmux-clean`, and the Windows job contains the five baseline command strings in the same order as the roadmap.

---

### Task 4: Capture the Windows Baseline

**Files:**
- Modify: `docs/baseline/cross-platform-beta-baseline.md`

**Interfaces:**
- Consumes: the unchanged canonical product code and the root CI command sequence.
- Produces: dated toolchain, test, conformance, performance, environment, and known-gap evidence against which later phases are compared.

- [x] **Step 1: Record the execution environment**

  Run `rustc -Vv`, `cargo -V`, and PowerShell environment probes. Record the OS build, architecture, PowerShell version, and any detected terminal host variables without inferring an unavailable terminal name.

- [x] **Step 2: Run formatting and lint gates**

  From `wmux-clean/`, run `cargo fmt --all -- --check` and `cargo clippy --workspace --all-targets -- -D warnings`; require exit code 0.

- [x] **Step 3: Run all workspace tests**

  Run `cargo test --workspace`; require exit code 0 and record the total number of passing tests from the emitted per-target summaries.

- [x] **Step 4: Run semantic conformance**

  Run `cargo run -p wmux-conformance --release`; require exit code 0 and record every printed suite fingerprint, including the aggregate fingerprint.

- [x] **Step 5: Run the release performance gate**

  Run `cargo run -p wmux-bench --release -- --suite full --gate`; require exit code 0 and record the reported throughput/latency/allocation metrics and thresholds.

- [x] **Step 6: Record supported evidence and known gaps**

  List only the current Windows/PowerShell execution environment as automated evidence. Mark Windows Terminal/conhost/VS Code and PowerShell 7/Windows PowerShell/cmd interactive combinations as unverified by Phase 1 unless explicitly exercised, and carry them into the later compatibility matrix.

---

### Task 5: Commit and Reproduce from a Clean Checkout

**Files:**
- Track: all non-generated source, documentation, manifests, and both `Cargo.lock` files under the outer repository boundary

**Interfaces:**
- Consumes: Tasks 1-4 and a passing canonical baseline.
- Produces: one initial foundation commit and clean-checkout evidence for the Phase 1 exit gate.

- [x] **Step 1: Stage the complete source snapshot**

  Run `git add .`, inspect `git status --short`, and verify both workspaces are preserved while all `target*` output is absent.

- [x] **Step 2: Commit the repository foundation**

  Run `git commit -m "chore: establish canonical wmux workspace"`. If Git lacks author identity, stop and report that exact blocker instead of inventing an identity.

- [x] **Step 3: Create a clean verification checkout**

  Clone the local repository at `HEAD` to a unique directory under the system temporary directory using `git clone --no-local`. Confirm the checkout has `wmux-clean/Cargo.toml`, both source trees, and no `target*` directories.

- [x] **Step 4: Re-run all five gates in the clean checkout**

  From the clone's `wmux-clean/`, run formatting, clippy, workspace tests, release conformance, and the full release performance gate exactly as Task 4. Require all exit codes to be 0.

- [x] **Step 5: Verify the exit gate**

  Run `git status --short` in the source repository and require no uncommitted Phase 1 files. Confirm the baseline document names `wmux-clean/` as authoritative and contains the clean-checkout result.
