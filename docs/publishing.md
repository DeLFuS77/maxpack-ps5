# Publishing

What is left in this repository before and at the first release. The three repositories are
public: DeLFuS77/ampr-emu-ng, DeLFuS77/maxpack (the PC tool and the engine) and this one, and each
one's README and Corresponding Source links point at the others.

## Before the first release

- **ampr-emu-ng is settled.** `vendor/ampr-emu-ng/libSceAmpr.sprx` is byte-identical to the
  `libSceAmpr.sprx` of the public release `v0.5.1-maxpack.1` of DeLFuS77/ampr-emu-ng (CI run 45435074348,
  tag on `73b2973`): same SHA-256. `SOURCE`, `SHA256SUMS` and `THIRD_PARTY_NOTICES` point at that release.
  Publish that draft release before or with this repository's `v1.0.0`. Change the vendored binary only
  together with `SHA256SUMS`, `VERSION` and `SOURCE` (DESIGN.md, Vendored components).
- **The secure-connection check passes on a console before `v1.0.0` is tagged.** With a build of the
  commit to be tagged, Settings > "Check secure connections" (docs/console-tests.md, 8.1) must end with
  "Passed": the console refuses an untrusted root, a wrong host and an expired certificate, and reaches
  badssl.com and api.github.com, with the checks `src/net.rs` sets. If it says a forged certificate was
  accepted, fix `src/net.rs` before any release. If a refusal shows a code that `maxpack_logic::tls` does
  not take as a TLS refusal (its line says "failed before the certificate check"), find out what that code
  means and add it there only if it is a certificate refusal.
- **`vendor/maxpack/COMMIT`** (`e8de15a`, tag `motor-consola-3`) is reachable in DeLFuS77/maxpack
  through that tag: keep the tag, and merge there without squashing or rebasing.
- **Tags.** This repository keeps the upstream project's tags locally (`v0.0.1-beta` to `v0.3.2`).
  Never `git push --tags`: it would publish them, and every `v*` tag starts the release job. Push
  only the release tag, `v` plus `Cargo.toml`'s version (`v1.0.0`); the release job refuses any
  other.
- **ampr-emu-ng auto-update** (`ampr_auto_update`) is off by default "until MaxPack is published"
  (`src/config.rs` default and help text, the page's `amprAutoUpdateHint` texts, README). Turning it on
  by default is a decision of its own, and it needs a published (not draft, not pre-release)
  ampr-emu-ng release with a `libSceAmpr.sprx` asset, since it reads `releases/latest`.

## The release

0. Once, before the first release: turn on immutable releases in this repository (Settings >
   General, Releases: enable release immutability), and in DeLFuS77/ampr-emu-ng. A published release
   can then no longer have its assets replaced or its tag moved: the self-update trusts the
   `SHA256SUMS` next to `maxpack.elf`, and `payloads.json` points at those assets. Drafts stay
   editable until they are published.
1. Push the tag `v1.0.0`. CI builds the payload and the release job creates a **draft** release with
   `maxpack.elf`, `SHA256SUMS` and `payloads.json`.
2. Publish the draft by hand, as a normal release (not a pre-release): GitHub's `releases/latest`,
   which the self-update and ps5-payload-manager read, skips drafts and pre-releases.
3. Check `https://github.com/DeLFuS77/maxpack-ps5/releases/latest/download/payloads.json` downloads
   and names this release's `maxpack.elf` and its SHA-256.

## The self-update

Nothing to change. While the repository has no published release, the look at start
gets a 404: the log has one line (`update: no published release of DeLFuS77/maxpack-ps5 ...`) and the
page offers nothing. Once `v1.0.0` is published, a 1.0.0 console logs `update: the latest release is
v1.0.0, running 1.0.0: nothing to offer`; the next release is the first one the page offers. On each
console it is offered only after Settings > "Check secure connections" has passed there once; until then the
notice asks for that check instead of showing Update.
