# Publishing

What to do in this repository on the day MaxPack's repositories become public, and what stays as it
is until then. The order across the three repositories: DeLFuS77/ampr-emu-ng first, then
DeLFuS77/maxpack (the PC tool and the engine), then this one, since each one's README and
Corresponding Source links point at the others.

## Before the first release

- **`vendor/ampr-emu-ng/SOURCE` names a private CI run.** Its `ci-run` line
  (`https://github.com/DeLFuS77/ampr-emu-ng/actions/runs/45365840836`) and the sentence "built by its
  CI run 45365840836" in `THIRD_PARTY_NOTICES` point at a run of a repository that is private today.
  Re-point both on publication day: to the public release of ampr-emu-ng that carries this exact
  `libSceAmpr.sprx` (`v0.5.1-maxpack.1`, same SHA-256 as `vendor/ampr-emu-ng/SHA256SUMS`), or to the
  run that built it if it is still visible then (run artifacts expire). The commit line
  (`3f91dbee4ab9eee80709ab878c6a7d77489bcc3d`) stays: it must remain reachable in that repository
  (merge its branch without squashing). Change the vendored binary only together with `SHA256SUMS`,
  `VERSION` and `SOURCE` (DESIGN.md, Vendored components).
- **`vendor/maxpack/COMMIT`** (`e9e0ec4`, tag `motor-consola-3`) must stay reachable in
  DeLFuS77/maxpack: merge there without squashing or rebasing.
- **Tags.** This repository keeps the upstream project's tags locally (`v0.0.1-beta` to `v0.3.2`).
  Never `git push --tags`: it would publish them, and every `v*` tag starts the release job. Push
  only the release tag, `v` plus `Cargo.toml`'s version (`v1.0.0`); the release job refuses any
  other.
- **ampr-emu-ng auto-update** (`ampr_auto_update`) is off by default "until MaxPack is published"
  (`src/config.rs` default and help text, the page's `amprAutoUpdateHint` texts, README). Turning it on
  by default is a decision of its own, and it needs a published (not draft, not pre-release)
  ampr-emu-ng release with a `libSceAmpr.sprx` asset, since it reads `releases/latest`.

## The release

1. Push the tag `v1.0.0`. CI builds the payload and the release job creates a **draft** release with
   `maxpack.elf`, `SHA256SUMS` and `payloads.json`.
2. Publish the draft by hand, as a normal release (not a pre-release): GitHub's `releases/latest`,
   which the self-update and ps5-payload-manager read, skips drafts and pre-releases.
3. Check `https://github.com/DeLFuS77/maxpack-ps5/releases/latest/download/payloads.json` downloads
   and names this release's `maxpack.elf` and its SHA-256.

## The self-update

Nothing to change. While the repository is private, or has no published release, the look at start
gets a 404: the log has one line (`update: no published release of DeLFuS77/maxpack-ps5 ...`) and the
page offers nothing. Once `v1.0.0` is published, a 1.0.0 console logs `update: the latest release is
v1.0.0, running 1.0.0: nothing to offer`; the next release is the first one the page offers.
