# v1.0.0 Release and Distribution Design

**Date:** 2026-08-23

**Status:** Approved in conversation; pending implementation plan

## Purpose

Prepare wmux for an open-source `1.0.0` release with reproducible
artifacts, straightforward installation on Windows, Linux, and macOS, and a
low-maintenance update path for later releases.

The release work must preserve `wmux-clean/` as the authoritative product
workspace. The repository-root Rust workspace remains a historical reference
until a separately approved move or removal. No release input may come from
that legacy workspace.

## Release Contract

The initial public version is `1.0.0` and its Git tag is `v1.0.0`. A matching
version tag triggers the release workflow. The workflow publishes a normal
GitHub release only after its release build jobs succeed. It does not use
GitHub's prerelease flag, so the standard latest-release URLs and updater work
without a separate release channel.

The CLI, configuration, installer, and update contracts presented as stable in
the public documentation follow SemVer from v1 onward. Breaking those public
contracts requires a new major version. Internal implementation details and
explicitly documented platform differences are not compatibility promises.

The supported binary targets are:

| Platform | Rust target | Archive |
| --- | --- | --- |
| Windows x64 | `x86_64-pc-windows-msvc` | ZIP |
| Linux x64 | `x86_64-unknown-linux-musl` | tarball |
| Linux ARM64 | `aarch64-unknown-linux-musl` | tarball |
| macOS Intel | `x86_64-apple-darwin` | tarball |
| macOS Apple Silicon | `aarch64-apple-darwin` | tarball |

Windows ARM64 and package-manager repositories such as Winget and a Homebrew
tap are outside the initial release. They may be added after native validation
and after stable GitHub release URLs exist.

## Atomic Application Package

`wmux` discovers `wmux-server` beside the current client executable. A valid
installation therefore consists of both binaries at the same version in the
same directory. Publishing only the client is invalid.

The Cargo package named `wmux` will define both binary targets:

- `wmux`, the disposable CLI and attached terminal client;
- `wmux-server`, the persistent server process.

The `wmux-server` package remains the server runtime library but no longer owns
the distributable binary target. It is marked non-publishable and non-distable.
Development, tests, and architectural crate boundaries remain unchanged; this
is a packaging ownership change, not a transfer of server semantics into the
client.

This follows Cargo's multiple-binaries-per-package model and ensures source
installation and generated archives install both executables together. The
release plan must include an automated assertion that every platform archive
contains exactly the expected client and server binary names.

## Distribution System

Use `dist` (formerly `cargo-dist`) with a pinned version. A repository-root
`dist-workspace.toml` points to the canonical `wmux-clean/Cargo.toml`. Keeping
that path in one file makes the later move from `wmux-clean/` a localized
configuration update.

The generated release system provides:

- GitHub Actions release orchestration from SemVer tags;
- native or supported cross-platform release builds;
- ZIP/tar archives containing both executables and public documentation;
- SHA-256 checksum sidecars;
- PowerShell and POSIX shell installers;
- a standalone `wmux-update` updater installed with script-based installs;
- a machine-readable distribution manifest;
- GitHub artifact attestations backed by Sigstore.

The dist build profile inherits the release profile and enables thin LTO.
Workspace tools such as benchmarks, conformance runners, stress runners, and
fuzz targets are never included in public application archives.

The GitHub repository is `https://github.com/shreshthkapai/wmux`. Generated
installer and updater URLs use this repository as their immutable origin.

## Installation Experience

The README leads with prebuilt installation. Windows users receive a single
PowerShell installer command; Linux and macOS users receive a single TLS-only
shell installer command. Both installers select the correct target, install
the client and server together, record the installation, and add the install
directory to the user's `PATH` using dist's platform-native behavior.

Manual installation remains available through release archives and checksum
verification. Building from source is documented for contributors, not
presented as the default end-user path.

The first-run examples use the canonical short commands:

```text
wmux new -s demo
wmux attach -t demo
wmux ls
```

Installation documentation must state the release's supported OS/architecture
matrix and link to the known-differences and compatibility documents.

## Update Experience

Script-based installs include `wmux-update`. Re-running the original installer
and running `wmux-update` are both supported upgrade paths. The updater uses
the install receipt and GitHub Releases to select and install the matching
platform artifact.

An update must not silently destroy persistent sessions. Because Windows does
not permit replacing the running server executable and the current server does
not support live handoff, documentation requires users to finish or explicitly
terminate active sessions before updating. If update command integration is
implemented, it must refuse while sessions exist and must never imply that a
detached session is safe to discard.

The initial release does not promise live server migration across versions. A
session-preserving rolling update requires a separately designed state-transfer
or versioned-install mechanism.

## Open-Source Repository Surface

The repository root will contain:

- `README.md` with product scope, release status, installation, quick start,
  keybindings, configuration pointers, platform support, and links to project
  policies;
- `LICENSE` containing the MIT license and the project copyright notice;
- `CHANGELOG.md` with an Unreleased section and `1.0.0` release notes;
- `CONTRIBUTING.md` with canonical-workspace commands, architecture rules,
  testing expectations, and pull-request guidance;
- `CODE_OF_CONDUCT.md` using the Contributor Covenant;
- `SECURITY.md` defining supported versions and private vulnerability reporting;
- issue forms for actionable bug reports and feature proposals;
- a pull-request template with test, documentation, and platform-impact checks;
- dependency-update configuration for Cargo and GitHub Actions;
- maintainer release documentation with dry-run, tag, validation, rollback,
  and post-release steps.

All Cargo packages inherit accurate version, license, repository, authorship,
Rust-version, and homepage metadata where applicable. Internal libraries and
test/benchmark tools are explicitly non-publishable. The initial release is
hosted as GitHub release artifacts; publishing the workspace to crates.io is
outside scope.

The README must clearly identify `wmux-clean/` as the temporary canonical
workspace and the root `crates/` tree as legacy, preventing contributors from
editing the wrong implementation. No legacy source is removed in this work.

## Supply-Chain and Platform Trust

The release is unsigned. Open-source licensing does not require Windows or Apple
developer certificates. Native Windows signing and Apple Developer ID
notarization are deferred without changing artifact names or end-user install
commands.

Unsigned status and possible platform trust prompts are documented honestly.
Integrity and provenance are supplied through:

- SHA-256 checksums for every archive;
- GitHub artifact attestations for release outputs;
- locked Cargo dependencies;
- pinned release-tool and GitHub Action versions where generated tooling allows;
- dependency advisory, license, and source-policy checks;
- a documented recommendation to enable GitHub immutable releases.

The dependency policy accepts licenses compatible with an MIT-distributed
binary and rejects unknown registries or Git dependencies unless explicitly
reviewed. Security reports must use GitHub private vulnerability reporting or
the contact method in `SECURITY.md`, not public issues.

## CI and Release Flow

Normal pushes and pull requests retain the existing beta-core quality,
portable semantics, native lifecycle, stress, fuzz-smoke, and Windows
performance gates. Additional repository checks validate dependency policy and
the dist release plan.

The maintainer flow is:

1. Update the changelog and set the workspace version.
2. Run the documented local release preflight from `wmux-clean/`.
3. Confirm the current `main` commit has green Windows, Linux, and macOS CI.
4. Run the dist plan for the intended version tag and inspect artifact contents.
5. Create and push the signed or annotated version tag.
6. Let GitHub Actions build and publish the release and its attestations.
7. Install from the public PowerShell and shell installers on clean hosts.
8. Verify both binaries report the released version and complete a native
   create/detach/reattach/kill smoke test.
9. Enable immutable releases after all expected assets are present.

Release tags must point to `main` commits that passed required checks. A failed
release build must not be presented as supported. Tags and published
immutable release assets are never moved or replaced; fixes receive a new
SemVer patch version.

## Existing GitHub Repository Cutover

The public GitHub repository currently contains the older C++ implementation,
while this local Rust repository has independent history and no configured
remote. This implementation prepares the local tree and embeds the confirmed
public URL, but it does not force-push, delete remote history, or change GitHub
settings.

Publishing requires a separately authorized cutover. Before that cutover, the
maintainer must choose whether to preserve the old GitHub history through a
merge/archive branch or replace the remote default branch. The operation and a
recovery reference must be reviewed before any destructive remote update.

## Verification

Implementation acceptance requires:

- `cargo fmt --all -- --check`;
- workspace Clippy with warnings denied;
- all workspace tests and fuzz-harness compilation/lint;
- portable conformance and deterministic stress suites;
- the full Windows release performance gate;
- native Windows and Linux lifecycle coverage;
- macOS Intel and Apple Silicon compilation, followed by native GitHub runner
  evidence before publishing;
- dependency advisory/license/source policy checks;
- a successful dist plan for `v1.0.0`;
- a local Windows release archive containing matching `wmux.exe` and
  `wmux-server.exe` binaries plus README, LICENSE, and changelog;
- version checks from both extracted executables;
- no tracked build outputs, credentials, local agent files, or runtime state.

The repository is release-candidate ready when all local checks pass. It is
publicly released only after the GitHub cutover, green hosted CI, published
artifacts, and clean-host installer smoke tests.

## Out of Scope

- Deleting, flattening, or renaming the legacy and canonical workspaces.
- Force-pushing or otherwise rewriting the existing GitHub repository.
- Windows Store, Winget, Homebrew tap, MacPorts, distro repositories, or
  crates.io publication.
- Windows ARM64 release artifacts.
- Windows Authenticode signing, Apple Developer ID signing, and notarization.
- Session-preserving live upgrades between server versions.

## Reference Model

- Local Zellij release automation packages Linux musl x64/ARM64, macOS
  x64/ARM64, and Windows x64 artifacts with checksums.
- Local tmux keeps source releases, contributor guidance, licensing, and
  regression workflows explicit at the repository boundary.
- Dist's multiple-binaries-per-package model guarantees both client and server
  ship as one application: <https://axodotdev.github.io/cargo-dist/book/workspaces/workspace-guide.html>
- Dist installers and updater behavior:
  <https://axodotdev.github.io/cargo-dist/book/installers/index.html> and
  <https://axodotdev.github.io/cargo-dist/book/installers/updater.html>
- GitHub artifact attestations:
  <https://docs.github.com/en/actions/concepts/security/artifact-attestations>
- GitHub immutable releases:
  <https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases>
