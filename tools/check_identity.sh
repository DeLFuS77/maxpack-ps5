#!/usr/bin/env bash
# Fails (exit 1) and lists file:line when a name from Kraken Mount that MaxPack replaced is still
# in src/, ps5/ or .github/ (binary files are skipped). Run by the CI `check` job.
set -euo pipefail
cd "$(dirname "$0")/.."

# What must be gone: the old name and paths, the old port, the old ampr-emu-ng update source.
PATTERN='Kraken Mount|krakenmount|kraken-mount|KRKN00001|(^|[^0-9])8097([^0-9]|$)|quer3q/ampr-emu-ng/releases'

# Allowed leftovers: each entry is an extended regex tried against the whole "file:line:text".
EXCEPTIONS=(
  # The internal crate keeps its upstream name (crates/kraken-mount-pkg) so merges from upstream stay easy.
  'kraken-mount-pkg'
  # The "about" credit link in ui.html (marked data-credit) names the project MaxPack is based on.
  '^src/ui\.html:[0-9]+:.*data-credit='
)
# The ampr-emu-ng contract (kraken.image, /app0/kraken/, kraken.pkg, kraken_zero.dat, "ampr-emu-ng ")
# does not need an entry: none of those names match PATTERN.

hits=$(grep -rnIE "$PATTERN" src ps5 .github || true)
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
