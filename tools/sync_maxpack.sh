#!/usr/bin/env bash
# Copies into vendor/maxpack what the payload builds from the MaxPack engine (the phase-2
# installer): the sources of one commit of the MaxPack repository, with the zstd and zlib sources
# its submodules pin at that commit and their licences. The commit goes into vendor/maxpack/COMMIT,
# and the tag that keeps it into vendor/maxpack/COMMIT.tag.
# A copy, not a submodule: this repository's CI cannot read the other private repository.
#
#   tools/sync_maxpack.sh <path-to-the-maxpack-repo> <commit>
#
# The MaxPack checkout needs its submodules (git submodule update --init). Nothing there is changed.
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "usage: $0 <path-to-the-maxpack-repo> <commit>" >&2
  exit 2
fi
# Absolute before the cd below, so a path relative to where it was run from still works.
repo=$(cd "$1" && pwd)
cd "$(dirname "$0")/.."
dest=vendor/maxpack
commit=$(git -C "$repo" rev-parse --verify "$2^{commit}")
# LF whatever the checkout's settings: the files go in as the repository stores them.
archive() { git -C "$1" -c core.autocrlf=false -c core.eol=lf archive --format=tar "$2" -- "${@:3}"; }

rm -rf "$dest"
mkdir -p "$dest"
# What CMakeLists.txt reads with -DMAXPACK_PS5=ON: cmake/, the library sources and src/ps5 (its
# maxpack_elf target must configure, though the payload does not build it). The engine's own
# installer page (web/index.html) is gone since its commit f8f69a7: the payload serves src/ui.html.
archive "$repo" "$commit" CMakeLists.txt LICENSE cmake src/core src/analyze src/pack src/net \
  src/install src/web src/ps5 | tar -x -C "$dest"

# The submodules at the commits the engine's commit records (cmake/ThirdParty.cmake compiles them).
sub() {
  local path=$1
  shift
  local sha
  sha=$(git -C "$repo" rev-parse "$commit:$path")
  mkdir -p "$dest/$path"
  archive "$repo/$path" "$sha" "$@" | tar -x -C "$dest/$path"
  echo "$path $sha" >> "$dest/COMMIT.submodules"
}
: > "$dest/COMMIT.submodules"
sub third_party/zstd LICENSE COPYING lib/zstd.h lib/zstd_errors.h lib/zdict.h \
  lib/common lib/compress lib/decompress lib/dictBuilder
sub third_party/zlib LICENSE README zlib.h zconf.h adler32.c compress.c crc32.c crc32.h deflate.c \
  deflate.h gzguts.h inffast.c inffast.h inffixed.h inflate.c inflate.h inftrees.c inftrees.h trees.c \
  trees.h uncompr.c zutil.c zutil.h

echo "$commit" > "$dest/COMMIT"
# The tag that keeps that commit in the MaxPack repository, so a rebase there cannot orphan it.
tags=$(git -C "$repo" tag --points-at "$commit")
if [[ -n "$tags" ]]; then
  printf '%s\n' "$tags" > "$dest/COMMIT.tag"
else
  echo "warning: no tag points at $commit in $repo; tag it (git tag -a motor-consola-N $commit) and run this again" >&2
fi
echo "vendor/maxpack: $commit ($(git -C "$repo" log -1 --format=%s "$commit"))"
cat "$dest/COMMIT.submodules"
