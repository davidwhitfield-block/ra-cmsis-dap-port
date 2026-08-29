#!/usr/bin/env bash
#
# Build the RA4M2 CMSIS-DAP probe firmware.
#
#   scripts/build.sh              incremental (a no-op if nothing changed)
#   scripts/build.sh --clean      from scratch
#   scripts/build.sh --quiet      only complain if something goes wrong
#
# Exit codes: 0 built (or already current), 1 bad usage/environment, 3 build failed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PROJ="$ROOT/ra4m2"
BUILD="$PROJ/Debug"
BIN="$BUILD/CMSIS_DAP_RA4M2.bin"

CLEAN=0
QUIET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --quiet|-q) QUIET=1; shift ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's|^# \{0,1\}||'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

say() { [[ $QUIET -eq 1 ]] || printf '%s\n' "$*"; }
hdr() { [[ $QUIET -eq 1 ]] || printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31mFAIL: %s\033[0m\n' "$1" >&2; exit "${2:-3}"; }

command -v arm-none-eabi-gcc >/dev/null \
    || die "arm-none-eabi-gcc is not on PATH. Install the Arm GNU toolchain." 1

[[ -f "$PROJ/Makefile" ]] || die "no Makefile at $PROJ" 1

if [[ $CLEAN -eq 1 ]]; then
    hdr "Clean"
    make -C "$PROJ" clean >/dev/null
fi

hdr "Build"
# -j is safe here; the Makefile's only generated prerequisite (memory_regions.ld)
# is declared, so make orders it correctly.
if [[ $QUIET -eq 1 ]]; then
    make -C "$PROJ" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" >/dev/null \
        || die "build failed (re-run without --quiet to see why)"
else
    make -C "$PROJ" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
        || die "build failed"
fi

[[ -f "$BIN" ]] || die "build reported success but $BIN is missing"

# The whole point of this guard: a flat `objcopy -O binary` of this ELF is ~16 MB
# because the image spans the option-setting region at 0x0100_A2CC, and it carries
# 0x00 at BPS (0x0100_A1C0) and PBPS (0x0100_A1E0). Programming that at 0x0
# permanently block-protects code flash. The Makefile drops those sections, so a
# correct image is ~54 KB. If this ever trips, do not flash the result.
SIZE=$(wc -c < "$BIN" | tr -d ' ')
if [[ "$SIZE" -gt 524288 ]]; then
    die "$BIN is $SIZE bytes - larger than code flash, so it is a flat objcopy
     binary carrying 0x00 at BPS/PBPS. Programming it bricks the part.
     The Makefile's -R .option_setting... flags are what keep it safe." 3
fi

hdr "Ready"
say "  elf   $BUILD/CMSIS_DAP_RA4M2.elf"
say "  srec  $BUILD/CMSIS_DAP_RA4M2.srec"
say "  bin   $BIN  ($SIZE bytes, code flash only)"
say ""
say "  Flash it with: scripts/flash.sh"
