# Safe Workspace Consolidation Plan

Date: 2026-08-20

## Decision

The outer `wmux/` directory is the repository and CI-discovery boundary. `wmux-clean/` is the only authoritative product workspace. The root `Cargo.toml` and `crates/` are a preserved legacy implementation and receive no new product work.

Phase 1 does not flatten, rename, archive, or delete either tree. This keeps every source file recoverable while later phases port the small set of legacy-only behavior deliberately.

## Stage 1: Preserve and route

1. Initialize Git at the outer `wmux/` boundary because the supplied `.git/` directory contains no usable metadata or recoverable history.
2. Track both source trees, both `Cargo.lock` files, architecture documents, baseline documents, and root CI.
3. Ignore all Cargo `target*` output and transient/runtime/package artifacts.
4. Route local baseline commands and GitHub Actions to `wmux-clean/`.
5. Mark the root workspace as legacy in the baseline and do not mutate its product code.

Rollback: before any later removal, the foundation commit contains both workspaces. Resetting or checking out that commit restores the Phase 1 arrangement without reconstructing files.

The managed workspace is owned by the sandbox SID while Git runs under the user's SID. Phase 1 uses `git -c safe.directory=C:/Users/shres/mux/wmux ...` for source-repository commands, avoiding a persistent change to the user's global Git configuration.

## Stage 2: Resolve the legacy-only behavior ledger

Each item is handled only in its assigned roadmap phase, after tmux/Zellij research and with a failing canonical test first:

| Item | Roadmap owner | Completion evidence |
|---|---|---|
| Shared CLI invocation classification and lifecycle/recovery commands | Task 3 | Canonical `wmux-cli` tests cover help/version, server lifecycle, reset/recovery, implicit startup policy, and diagnostics. |
| `display-message`/`display` and `show-messages` | Tasks 4 and 8 | Shared parser tests and server-owned message-state tests define accepted semantics. |
| Unix crate/module names | Task 6 | Real Unix PTY/process/IPC/terminal/signal backend passes native Linux tests; no placeholder is copied as implementation. |
| Older Windows helper/module shapes | Tasks 3 and 5 | Windows lifecycle/security tests and the frozen platform contract show whether any behavior gap exists. Empty marker types are not ported. |
| Root architecture documents | Every architecture phase | Documents remain available as historical constraints; current docs/tests change with architecture. |

No ledger item is considered resolved merely because similarly named canonical code exists. The relevant behavioral test must pass.

## Stage 3: Prove the canonical tree independently

Before proposing any physical consolidation:

1. Root CI discovers and runs the `wmux-clean/` jobs.
2. A clean local clone at the foundation commit passes formatting, clippy, all workspace tests, portable conformance, and the release performance gate.
3. Every legacy-only behavior-ledger row is either implemented with tests or rejected in a written product decision.
4. All subsequent roadmap work and documentation links target the canonical tree.
5. No release or packaging script consumes the legacy workspace.

## Stage 4: Choose a physical end state separately

After Stage 3, prepare a distinct reviewable proposal for one of these outcomes:

- Keep `wmux-clean/` as a permanent nested canonical workspace and archive the legacy tree outside the active source layout.
- Move the canonical manifest/crates/docs to the repository root in a Git-tracked rename, update all paths, and prove the full baseline before removing duplicates.

For either outcome, preserve the legacy tree until the moved/remaining canonical tree passes the clean-checkout baseline. Removing or archiving the legacy tree requires explicit user approval because it is a destructive scope change not authorized by Phase 1.

## Removal gate

The root legacy workspace may be removed or archived only when all conditions are true:

- The legacy-only behavior ledger has no unresolved row.
- Canonical CI and clean-checkout gates pass at the same commit.
- No canonical manifest, script, document, or test references root `crates/` as active code.
- A Git tag or retained commit contains both trees.
- The exact removal path and recovery command are documented.
- The user explicitly approves the destructive change.
