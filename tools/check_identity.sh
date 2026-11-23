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
)
# The ampr-emu-ng contract (kraken.image, /app0/kraken/, kraken.pkg, kraken_zero.dat, "ampr-emu-ng ")
# needs no entry: none of those names match PATTERN. The internal crate kraken-mount-pkg lives in
# Cargo.toml and crates/, outside the scanned paths (src/ uses kraken_mount_pkg).

hits=$(grep -rnIE "$PATTERN" src ps5 .github docs --include="*" --exclude="*.png" || true)
for ex in "${EXCEPTIONS[@]}"; do
  hits=$(printf '%s\n' "$hits" | grep -vE "$ex" || true)
done
hits=$(printf '%s\n' "$hits" | sed '/^$/d')

if [[ -n "$hits" ]]; then
  echo "check_identity: names from the old identity are still present:" >&2
  printf '%s\n' "$hits" >&2
  echo "check_identity: $(printf '%s\n' "$hits" | wc -l) occurrence(s)" >&2
  exit 1
fi
echo "check_identity: OK"
