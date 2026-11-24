#!/usr/bin/env bash
# Fails (exit 1) and lists file:line when a name from Kraken Mount that MaxPack replaced is still
# in src/, ps5/, .github/ or docs/ (binary files are skipped). Run by the CI `check` job.
set -euo pipefail
cd "$(dirname "$0")/.."

# What must be gone: the old name and paths, the old port, the old ampr-emu-ng update source.
PATTERN='Kraken Mount|krakenmount|kraken-mount|KRKN00001|(^|[^0-9])8097([^0-9]|$)|quer3q/ampr-emu-ng/releases|kraken\.log|kraken\.appcache|KRAKEN_SELF_ELF|/data/krakenmount'

# Allowed leftovers: each entry is an extended regex tried against the whole "file:line:text",
# so it must match the whole line exactly (anchored), never just a fragment of it.
EXCEPTIONS=(
  # The "about" credit link in ui.html names the project MaxPack is based on: exactly this line.
  '^src/ui\.html:[0-9]+: *<a class="gh-link button" data-credit="kraken-mount" href="https://github\.com/quer3q/kraken-mount" aria-label="Based on Kraken Mount by quer3q \(GPL-3\.0-or-later\)" title="Based on Kraken Mount by quer3q \(GPL-3\.0-or-later\)">$'
  # The credit line of Settings > About, in its markup and its English and Spanish texts: exactly these.
  '^src/ui\.html:[0-9]+: *<p data-i18n="aboutBased">Based on Kraken Mount by quer3q\.</p>$'
  "^src/ui\.html:[0-9]+: *aboutBased: 'Based on Kraken Mount by quer3q\.',$"
  "^src/ui\.html:[0-9]+: *aboutBased: 'Basado en Kraken Mount, de quer3q\.',$"
  # The same credit line in the other nine languages (double-quoted, the only entry on its line).
  '^src/ui\.html:[0-9]+: *aboutBased: "[^"]*Kraken Mount[^"]*",$'
  # The bug report form asks which other mounters were running: the original is one of the options.
  '^\.github/ISSUE_TEMPLATE/bug_report\.yml:[0-9]+: *- Kraken Mount$'
)
# The console checklist tests the original mounter itself (before anything, coming from it and
# going back to it), so there, and only there, it may name it, its data folder and its process:
# those three tokens are taken out of each of its lines, which is then checked again. Anything
# else of the old identity (its port, its log...) still fails there too.
CHECKLIST='docs/console-tests.md'
CHECKLIST_ALLOWED='Kraken Mount|/data/krakenmount|kraken-mount\.elf'
# The ampr-emu-ng contract (kraken.image, /app0/kraken/, kraken.pkg, kraken_zero.dat, "ampr-emu-ng ")
# needs no entry: none of those names match PATTERN. The internal crate kraken-mount-pkg lives in
# Cargo.toml and crates/, outside the scanned paths (src/ uses kraken_mount_pkg).

hits=$(grep -rnIE "$PATTERN" src ps5 .github docs --include="*" --exclude="*.png" || true)
for ex in "${EXCEPTIONS[@]}"; do
  hits=$(printf '%s\n' "$hits" | grep -vE "$ex" || true)
done
kept=""
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  if [[ "$line" == "$CHECKLIST":* ]]; then
    text=${line#"$CHECKLIST":*:}
    stripped=$(printf '%s\n' "$text" | sed -E "s#${CHECKLIST_ALLOWED}##g")
    [[ "$stripped" =~ $PATTERN ]] || continue
  fi
  kept+="$line"$'\n'
done <<< "$hits"
hits=$(printf '%s' "$kept" | sed '/^$/d')

if [[ -n "$hits" ]]; then
  echo "check_identity: names from the old identity are still present:" >&2
  printf '%s\n' "$hits" >&2
  echo "check_identity: $(printf '%s\n' "$hits" | wc -l) occurrence(s)" >&2
  exit 1
fi
# The bundled ampr-emu-ng is our own build (DeLFuS77/ampr-emu-ng): its VERSION ends in -maxpack.N.
if ! grep -qE -- '-maxpack\.[0-9]+$' vendor/ampr-emu-ng/VERSION; then
  echo "check_identity: vendor/ampr-emu-ng/VERSION ($(tr -d '\r\n' < vendor/ampr-emu-ng/VERSION)) must end in -maxpack.N (our own build)" >&2
  exit 1
fi
# MaxPack never downloads quer3q's build: no URL of its releases in code (the PATTERN above covers
# src/, ps5/, .github/ and docs/; Cargo.toml, crates/ and tools/ other than this script are checked here).
if grep -rnIE 'quer3q/ampr-emu-ng/releases' Cargo.toml crates tools --exclude=check_identity.sh 2>/dev/null | grep -q .; then
  echo "check_identity: a quer3q/ampr-emu-ng/releases URL is in code" >&2
  exit 1
fi
echo "check_identity: OK"
