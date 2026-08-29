#!/usr/bin/env bash
#
# Unbrick an RA4M2 whose code flash was permanently block-protected by a flat
# `objcopy -O binary` image being programmed at 0x0 (BPS = PBPS = 0x00000000).
#
# Redirects BPS_SEL to the pristine secure block-protect pair, then erases and
# reflashes. See README.md in this directory for the full explanation.
#
#   ./recover.sh --sn 821000843 --bin ../../ra4m2/Debug/CMSIS_DAP_RA4M2.bin
#
# Exit codes: 0 recovered, 1 bad usage/environment, 2 not recoverable
# (secure pair also poisoned - replace the IC), 3 a step failed.

set -euo pipefail

PROBE_SN=""
FW_BIN=""
DEVICE="R7FA4M2AB"
SPEED="1000"
BPS_SEL_MASK=""
SKIP_FLASH=0

usage() {
    sed -n '2,/^$/p' "$0" | sed 's|^# \{0,1\}||'
    cat <<'EOF'

Options:
  --sn <serial>     J-Link probe serial number. Required if more than one
                    probe is attached, and you almost always have two.
  --bin <path>      Firmware .bin to program. MUST be a code-flash-only image
                    (~54 KB), never a flat objcopy binary. Omit with
                    --skip-flash to unlock only.
  --device <name>   J-Link device name. Default R7FA4M2AB (256 KB).
  --speed <kHz>     SWD speed. Default 1000.
  --mask <hex>      Override BPS_SEL value. Default is derived from --device:
                      256 KB blocks 0-13 -> 0xFFFFC000
                      384 KB blocks 0-17 -> 0xFFFC0000
                      512 KB blocks 0-21 -> 0xFFC00000
  --skip-flash      Unlock and verify only, do not program firmware.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sn)         PROBE_SN="$2"; shift 2 ;;
        --bin)        FW_BIN="$2"; shift 2 ;;
        --device)     DEVICE="$2"; shift 2 ;;
        --speed)      SPEED="$2"; shift 2 ;;
        --mask)       BPS_SEL_MASK="$2"; shift 2 ;;
        --skip-flash) SKIP_FLASH=1; shift ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 1 ;;
    esac
done

command -v JLinkExe >/dev/null || { echo "JLinkExe not on PATH" >&2; exit 1; }

if [[ -z "$BPS_SEL_MASK" ]]; then
    case "$DEVICE" in
        R7FA4M2AB*) BPS_SEL_MASK="0xFFFFC000" ;;  # 256 KB, blocks 0-13
        R7FA4M2AD*) BPS_SEL_MASK="0xFFC00000" ;;  # 512 KB, blocks 0-21
        *) echo "cannot derive BPS_SEL mask for '$DEVICE'; pass --mask" >&2; exit 1 ;;
    esac
fi
# Split into the two 16-bit data words the Configuration Set command wants.
MASK_DEC=$((BPS_SEL_MASK))
WD1=$(printf '0x%04X' $((MASK_DEC & 0xFFFF)))
WD2=$(printf '0x%04X' $(((MASK_DEC >> 16) & 0xFFFF)))

if [[ $SKIP_FLASH -eq 0 ]]; then
    [[ -n "$FW_BIN" ]] || { echo "--bin is required (or pass --skip-flash)" >&2; exit 1; }
    [[ -f "$FW_BIN" ]] || { echo "no such file: $FW_BIN" >&2; exit 1; }
    FW_BIN="$(cd "$(dirname "$FW_BIN")" && pwd)/$(basename "$FW_BIN")"
    size=$(wc -c < "$FW_BIN" | tr -d ' ')
    if [[ $size -gt 524288 ]]; then
        echo "REFUSING: '$FW_BIN' is $size bytes." >&2
        echo "That is larger than code flash, so it is a flat objcopy binary" >&2
        echo "carrying 0x00 at BPS/PBPS. Programming it is what bricks boards." >&2
        exit 1
    fi
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# The .jlink files are standalone - they carry their own device/si/speed/connect
# preamble so they can be run by hand with plain JLinkExe -CommandFile. prep()
# rewrites that preamble to honour --device / --speed before we run one.
prep() {  # prep <src.jlink> <dst.jlink>
    sed -e "s/^device .*/device $DEVICE/" -e "s/^speed .*/speed $SPEED/" "$1" > "$2"
}

jlink() {  # jlink <script-file> -> stdout, also tee'd to $WORK/last.log
    local args=(-ExitOnError 0 -NoGui 1)
    [[ -n "$PROBE_SN" ]] && args+=(-SelectEmuBySN "$PROBE_SN")
    JLinkExe "${args[@]}" -CommandFile "$1" 2>&1 | tee "$WORK/last.log"
}

# Pull "AAAAAAAA = VVVVVVVV" out of J-Link Commander output.
readval() { grep -iE "^0*${1} = " "$WORK/last.log" | tail -1 | awk '{print $3}' | tr 'a-f' 'A-F'; }

hdr() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
die() { printf '\033[31mFAIL: %s\033[0m\n' "$1" >&2; exit "${2:-3}"; }

# ---------------------------------------------------------------- preflight
hdr "1/4  Preflight"
prep "$HERE/01-preflight.jlink" "$WORK/preflight.jlink"
jlink "$WORK/preflight.jlink" >/dev/null
BPS=$(readval 0100A1C0);      PBPS=$(readval 0100A1E0)
BPS_SEC=$(readval 0100A240);  PBPS_SEC=$(readval 0100A260)
BPS_SEL=$(readval 0100A2C0);  DLM=$(readval 400E002C)
[[ -n "$BPS" ]] || die "could not read option-setting memory - is the probe connected?" 1

printf '  BPS      %s\n  PBPS     %s\n  BPS_SEC  %s\n  PBPS_SEC %s\n  BPS_SEL  %s\n  DLMMON   %s\n' \
       "$BPS" "$PBPS" "$BPS_SEC" "$PBPS_SEC" "$BPS_SEL" "$DLM"

if [[ "$BPS" == "FFFFFFFF" && "$PBPS" == "FFFFFFFF" ]]; then
    echo "  Block protection is not poisoned; nothing to unlock."
    if [[ $SKIP_FLASH -eq 1 ]]; then exit 0; fi
elif [[ "$BPS_SEC" != "FFFFFFFF" || "$PBPS_SEC" != "FFFFFFFF" ]]; then
    die "secure pair is ALSO poisoned (BPS_SEC=$BPS_SEC PBPS_SEC=$PBPS_SEC).
     There is no escape route left. Replace the IC." 2
fi
# DBG2 is needed to program the secure region of option-setting memory.
[[ "$DLM" == "00000001" || "$DLM" == "00000002" ]] \
    || die "DLMMON=$DLM is not CM(1) or SSD(2); debug level is below DBG2." 2

# ------------------------------------------------------------------- unlock
if [[ "$BPS_SEL" != "FFFFFFFF" ]]; then
    echo "  BPS_SEL already programmed ($BPS_SEL); skipping unlock."
else
    hdr "2/4  Redirect BPS_SEL -> $BPS_SEL_MASK"
    prep "$HERE/02-unlock.jlink" "$WORK/unlock.raw"
    sed -e "s/^w2 0x407E0000, 0xC000.*/w2 0x407E0000, $WD1/" \
        -e "s/^w2 0x407E0000, 0xFFFF *\/\/ WD2.*/w2 0x407E0000, $WD2/" \
        "$WORK/unlock.raw" > "$WORK/unlock.jlink"
    jlink "$WORK/unlock.jlink" >/dev/null
    FSTATR=$(readval 407FE080); SEL=$(readval 0100A2C0)
    case "$FSTATR" in
        00008000) : ;;
        0080C000) die "ILGCOMERR+ILGLERR - malformed command (byte-write cycles?)" ;;
        00008040) die "FLWEERR - FWEPROR not set; it lives at 0x4001E416, not 0x407FE416" ;;
        *)        die "unexpected FSTATR=$FSTATR" ;;
    esac
    [[ "$SEL" == "${BPS_SEL_MASK#0x}" ]] || die "BPS_SEL reads $SEL, expected ${BPS_SEL_MASK#0x}"
    echo "  BPS_SEL = $SEL, FSTATR = $FSTATR"
fi

# ------------------------------------------------------------------- verify
hdr "3/4  Reset and prove block 0 erases"
prep "$HERE/03-verify.jlink" "$WORK/verify.jlink"
jlink "$WORK/verify.jlink" >/dev/null
grep -q "Erasing done" "$WORK/last.log" || die "block 0 still refuses to erase"
[[ "$(readval 00000000)" == "FFFFFFFF" ]] || die "block 0 did not read back erased"
echo "  Block 0 erased. Code flash is programmable again."

# -------------------------------------------------------------------- flash
if [[ $SKIP_FLASH -eq 1 ]]; then
    hdr "Done (unlock only)"
    exit 0
fi

hdr "4/4  Flash $(basename "$FW_BIN")"
# NEVER a bare `erase` here. J-Link's Erase Chip also erases the config area,
# which blanks BPS_SEL back to FFFFFFFF and instantly re-locks the part. BPS and
# PBPS cannot be un-zeroed by that erase (Table 44.22), so you land straight
# back in the poisoned state. loadbin erases only the sectors it needs.
cat > "$WORK/flash.jlink" <<EOF
device $DEVICE
si SWD
speed $SPEED
connect
r
h
loadbin $FW_BIN 0x0
mem32 0x00000000 2
mem32 0x0100A2C0 1
r
g
qc
EOF
jlink "$WORK/flash.jlink" >/dev/null
grep -qE "O\.K\.|range affected" "$WORK/last.log" || die "programming failed"
if grep -qE '^\*+ Error|^ERROR:' "$WORK/last.log"; then
    die "J-Link reported an error while programming (see output above)"
fi
SEL=$(readval 0100A2C0)
[[ "$SEL" == "${BPS_SEL_MASK#0x}" ]] || die "BPS_SEL was lost during flash (now $SEL) - a chip erase ran"

EXPECT=$(od -An -tx4 -N4 "$FW_BIN" | tr -d ' \n' | tr 'a-f' 'A-F')
GOT=$(readval 00000000)
[[ "$GOT" == "$EXPECT" ]] || die "vector table mismatch: flash=$GOT bin=$EXPECT"

hdr "Recovered"
printf '  Initial SP  %s (matches %s)\n  BPS_SEL     %s\n' "$GOT" "$(basename "$FW_BIN")" "$SEL"
echo "  BPS/PBPS remain 00000000 forever; they are no longer consulted."
echo "  Do not run a bare J-Link \`erase\` on this part - it re-locks it."
