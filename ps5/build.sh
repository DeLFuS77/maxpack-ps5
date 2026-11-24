#!/usr/bin/env bash
# Builds target/ps5/maxpack.elf, the PS5 payload, in Docker (toolchain from ps5-dump-forge).
set -euo pipefail
cd "$(dirname "$0")/.."

if [[ "${1:-}" != --inside ]]; then
  docker build -t maxpack-ps5 ps5
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    -e CARGO_HOME=/work/target/ps5/cargo -v "$PWD:/work" maxpack-ps5 ps5/build.sh --inside
fi

# The bundled ampr-emu-ng must be the released file (SHA256SUMS copied from its GitHub release).
(cd vendor/ampr-emu-ng && sha256sum -c --ignore-missing --strict SHA256SUMS)
# The bundled dlc_emu and pgo_stub (src/cache.rs EMUS) too.
(cd vendor/dlc_emu && sha256sum -c --strict SHA256SUMS)
(cd vendor/pgo_stub && sha256sum -c --strict SHA256SUMS)
echo "MaxPack engine: vendor/maxpack $(cat vendor/maxpack/COMMIT)"

out=target/ps5
cc="$PS5_PAYLOAD_SDK/bin/prospero-clang"
mkdir -p "$out/shim"
for src in ps5/shim/freebsd11.c ps5/shim/syscalls.S ps5/entry.c ps5/compat.c ps5/usb.c; do
  name=$(basename "${src%.*}")
  "$cc" -O2 -Wall -Wextra -Werror -c "$src" -o "$out/shim/$name.o"
done
"$cc" -O2 -Wall -Wextra -Werror -I/opt/sqlite -c ps5/appdb.c -o "$out/shim/appdb.o"
# The SQLite amalgamation (Dockerfile) is third-party code: no -Werror.
"$cc" -O2 -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -c /opt/sqlite/sqlite3.c -o "$out/shim/sqlite3.o"

# The MaxPack engine (vendor/maxpack, the phase-2 installer: tools/sync_maxpack.sh copies it), built
# by its own CMake with the SDK as the engine's CI ps5 job does, into the static libraries that
# ps5/linker.sh links (in this order). Its maxpack_elf target (the phase-2 ELF) is not built, so
# -DMAXPACK_RUNTIME, which only feeds that target, is not passed: the runtime the installer carries
# is src/main.rs BUNDLED_AMPR, the same vendor/ampr-emu-ng/libSceAmpr.sprx, embedded once.
eng="$out/engine"
engine_libs=(maxpack_web maxpack_page maxpack_install maxpack_net maxpack_pack maxpack_analyze maxpack_core mp_zstd mp_zlib)
"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S vendor/maxpack -B "$eng" -DMAXPACK_PS5=ON -DCMAKE_BUILD_TYPE=Release
cmake --build "$eng" -j "$(nproc)" --target "${engine_libs[@]}"
for lib in "${engine_libs[@]}"; do
  [[ -f "$eng/lib$lib.a" ]] || { echo "build.sh: no $eng/lib$lib.a" >&2; ls "$eng" >&2; exit 1; }
done
printf '%s\n' "${engine_libs[@]}" > "$eng/libs.txt"
# src/installer.rs's side of it: the console's env and the ShadowMountPlus client (ps5/installer.c).
"$cc" -O2 -Wall -Wextra -Werror -Ivendor/maxpack/src/core -Ivendor/maxpack/src/net \
  -Ivendor/maxpack/src/install -Ivendor/maxpack/src/web -c ps5/installer.c -o "$out/shim/installer.o"
# The mtime the installer gives the runtime it writes (installer::runtime_mtime): the file's, as
# the engine's CMake takes it for -DMAXPACK_RUNTIME.
MAXPACK_RUNTIME_MTIME=$(stat -c %Y vendor/ampr-emu-ng/libSceAmpr.sprx)
export MAXPACK_RUNTIME_MTIME

export RUSTC_BOOTSTRAP=1
# The PS5 kernel fills FreeBSD 11 structs (stat 120 bytes, dirent 264, old statfs); libc and the
# std built here default to FreeBSD 12's. libc >= 0.2.187 reads this cfg; rustflags reach std
# too under -Z build-std.
export CARGO_TARGET_X86_64_PS5_FREEBSD_RUSTFLAGS='--cfg libc_unstable_freebsd_version="11"'
export CARGO_TARGET_DIR="$out"
export CARGO_TARGET_X86_64_PS5_FREEBSD_LINKER=/work/ps5/linker.sh
export CC_x86_64_ps5_freebsd="$cc" AR_x86_64_ps5_freebsd="$PS5_PAYLOAD_SDK/bin/prospero-ar"
# Cargo doesn't see the C objects or linker.sh: drop the linked output so a shim change relinks.
build() {
  rm -f "$out"/x86_64-ps5-freebsd/release/deps/maxpack_ps5-*
  cargo build --release --locked \
    --target ps5/x86_64-ps5-freebsd.json -Z json-target-spec \
    -Z build-std=std,panic_abort -Z build-std-features=panic-unwind "$@"
}
# Stage 1 is the copy the tile starts (/data/maxpack/maxpack.elf); stage 2, the ELF you
# push, embeds it and saves it there (as Dump Forge's self copy).
build
cp "$out/x86_64-ps5-freebsd/release/maxpack-ps5" "$out/maxpack-stage1.elf"
MAXPACK_SELF_ELF="/work/$out/maxpack-stage1.elf" build --features self-copy
cp "$out/x86_64-ps5-freebsd/release/maxpack-ps5" "$out/maxpack.elf"
llvm-readelf -h -d "$out/maxpack.elf" > "$out/maxpack.elf.txt"
# The engine is inside: maxpack.elf is stripped (Cargo profile strip), so llvm-readelf has no
# symbol table to show; lld's map of the same link (ps5/linker.sh -Map) lists every symbol placed.
{
  echo
  echo "MaxPack engine vendor/maxpack $(cat vendor/maxpack/COMMIT), symbols in maxpack.elf (lld map):"
  for sym in mp_api_call mp_inst_install mp_play_open; do
    line=$(grep -E "[[:space:]]$sym\$" "$out/maxpack.map" | head -n 1 || true)
    [[ -n "$line" ]] || { echo "build.sh: $sym is not in maxpack.elf ($out/maxpack.map)" >&2; exit 1; }
    echo "$line" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+/ /g'
  done
  # zstd's dictBuilder (ZDICT_trainFromBuffer, used by the engine's mp_v2exp.c) can bring
  # libdivsufsort-lite (MIT) along. Not linked today (the payload pulls no dictionary training);
  # if a later engine sync links it, the build stops until THIRD_PARTY_NOTICES carries its notice.
  if grep -qE '[[:space:]]divsufsort$|divsufsort\.c\.o' "$out/maxpack.map"; then
    echo "zstd dictBuilder divsufsort: linked"
    grep -q 'divsufsort' THIRD_PARTY_NOTICES \
      || { echo "build.sh: divsufsort (MIT) is linked: add its notice to THIRD_PARTY_NOTICES" >&2; exit 1; }
  else
    echo "zstd dictBuilder divsufsort: not linked"
  fi
} >> "$out/maxpack.elf.txt"
ls -l "$out/maxpack.elf"
