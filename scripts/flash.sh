#!/usr/bin/env bash
#
# Build if needed, recover if needed, flash. Run it as many times as you like.
#
# It copes with every state a board on this bench turns up in:
#   - never built            -> builds first, once
#   - blank/erased chip      -> programs it
#   - healthy chip           -> reprograms it, touching nothing else
#   - poisoned chip          -> unlocks it first (BPS = PBPS = 0x00000000, the
#                               damage a flat `objcopy -O binary` image does),
#                               then programs it
#   - already-unlocked chip  -> notices, and does not redo the unlock
#
#   scripts/flash.sh                     auto-detect the probe if only one is attached
#   scripts/flash.sh --sn 821000843      pick a probe (needed when several are plugged in)
#   scripts/flash.sh --rebuild           force a rebuild first
#   scripts/flash.sh --no-build          refuse to build; fail if there is no image
#
# Exit codes: 0 flashed, 1 bad usage/environment, 2 unrecoverable (replace the
# IC), 3 a step failed.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PROJ="$ROOT/ra4m2"
BUILD="$PROJ/Debug"
BIN="$BUILD/CMSIS_DAP_RA4M2.bin"
ELF="$BUILD/CMSIS_DAP_RA4M2.elf"
RECOVER="$ROOT/tools/recovery/recover.sh"

PROBE_SN="${RA_JLINK_SN:-}"
DEVICE="R7FA4M2AB"
SPEED="1000"
REBUILD=0
NOBUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sn)      PROBE_SN="$2"; shift 2 ;;
        --bin)     BIN="$2"; shift 2 ;;
        --device)  DEVICE="$2"; shift 2 ;;
        --speed)   SPEED="$2"; shift 2 ;;
        --rebuild) REBUILD=1; shift ;;
        --no-build) NOBUILD=1; shift ;;
        -h|--help) sed -n '2,/^$/p' "$0" | sed 's|^# \{0,1\}||'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 1 ;;
    esac
done

hdr()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ok()   { printf '  \033[32m%s\033[0m\n' "$*"; }
warn() { printf '  \033[33m%s\033[0m\n' "$*"; }
die()  { printf '\033[31mFAIL: %s\033[0m\n' "$1" >&2; exit "${2:-3}"; }

command -v JLinkExe >/dev/null || die "JLinkExe is not on PATH" 1

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

jlink() {  # jlink <script-file>; output tee'd to $WORK/last.log
    local args=(-ExitOnError 0 -NoGui 1)
    [[ -n "$PROBE_SN" ]] && args+=(-SelectEmuBySN "$PROBE_SN")
    JLinkExe "${args[@]}" -CommandFile "$1" >"$WORK/last.log" 2>&1 || true
}
readval() { grep -iE "^0*${1} = " "$WORK/last.log" | tail -1 | awk '{print $3}' | tr 'a-f' 'A-F'; }

# ------------------------------------------------------------------- 1. build
hdr "1/4  Firmware image"
need_build=0
if [[ ! -f "$BIN" ]]; then
    need_build=1
    reason="no image at $BIN"
elif [[ $REBUILD -eq 1 ]]; then
    need_build=1
    reason="--rebuild"
else
    # Rebuild only when something actually changed. This is what makes the script
    # cheap to run in a loop: make is not invoked at all on the common path.
    newest=$(find "$PROJ/src" "$PROJ/ra_gen" "$PROJ/ra_cfg" "$PROJ/Makefile" \
                  -newer "$BIN" -print -quit 2>/dev/null || true)
    if [[ -n "$newest" ]]; then
        need_build=1
        reason="$(basename "$newest") is newer than the image"
    fi
fi

if [[ $need_build -eq 1 ]]; then
    [[ $NOBUILD -eq 0 ]] || die "$reason, and --no-build was given" 1
    echo "  $reason - building"
    "$HERE/build.sh" --quiet || die "build failed - run scripts/build.sh to see why"
    ok "built $(wc -c < "$BIN" | tr -d ' ') bytes"
else
    ok "using existing image ($(wc -c < "$BIN" | tr -d ' ') bytes)"
fi

SIZE=$(wc -c < "$BIN" | tr -d ' ')
[[ "$SIZE" -le 524288 ]] || die "$BIN is $SIZE bytes - that is a flat objcopy binary
     carrying 0x00 at BPS/PBPS, and programming it is what poisons boards." 1

# ------------------------------------------------------------------- 2. probe
hdr "2/4  Probe"
if [[ -z "$PROBE_SN" ]]; then
    printf 'ShowEmuList\nq\n' > "$WORK/list.jlink"
    JLinkExe -NoGui 1 -CommandFile "$WORK/list.jlink" >"$WORK/emus.log" 2>&1 || true
    # No mapfile: macOS ships bash 3.2, where it does not exist.
    SNS=()
    while IFS= read -r sn; do
        [[ -n "$sn" ]] && SNS+=("$sn")
    done < <(grep -oE 'Serial number: [0-9]+' "$WORK/emus.log" | awk '{print $3}' | sort -u)
    case "${#SNS[@]}" in
        0) die "no J-Link found. Plug one in, or pass --sn." 1 ;;
        1) PROBE_SN="${SNS[0]}"; ok "auto-detected J-Link $PROBE_SN" ;;
        *) die "several J-Links attached (${SNS[*]}).
     Pass --sn <serial> so this cannot program the wrong board." 1 ;;
    esac
else
    ok "using J-Link $PROBE_SN"
fi

# --------------------------------------------------------------- 3. preflight
hdr "3/4  Block protection"
cat > "$WORK/pre.jlink" <<EOF
device $DEVICE
si SWD
speed $SPEED
connect
mem32 0100A1C0 1
mem32 0100A1E0 1
mem32 0100A240 1
mem32 0100A260 1
mem32 0100A2C0 1
mem32 400E002C 1
q
EOF
jlink "$WORK/pre.jlink"
BPS=$(readval 0100A1C0); PBPS=$(readval 0100A1E0)
BPS_SEC=$(readval 0100A240); PBPS_SEC=$(readval 0100A260)
BPS_SEL=$(readval 0100A2C0)
[[ -n "$BPS" ]] || die "cannot read option-setting memory. Is the probe wired to a
     powered board? (J-Link $PROBE_SN)" 1

printf '  BPS %s  PBPS %s  BPS_SEL %s\n' "$BPS" "$PBPS" "$BPS_SEL"

POISONED=0
if [[ "$BPS" == "00000000" || "$PBPS" == "00000000" ]]; then
    if [[ "$BPS_SEL" != "FFFFFFFF" ]]; then
        # BPS/PBPS can never be un-zeroed, so a recovered board reads poisoned
        # forever. BPS_SEL is what decides whether anyone consults them.
        ok "poisoned but already redirected via BPS_SEL - no unlock needed"
    else
        [[ "$BPS_SEC" == "FFFFFFFF" && "$PBPS_SEC" == "FFFFFFFF" ]] \
            || die "BPS and PBPS are zeroed AND the secure pair is too
     (BPS_SEC=$BPS_SEC PBPS_SEC=$PBPS_SEC). There is no escape route. Replace the IC." 2
        POISONED=1
        warn "poisoned (BPS=PBPS=0) - will unlock before flashing"
    fi
else
    ok "healthy"
fi

# ------------------------------------------------------------------- 4. flash
if [[ $POISONED -eq 1 ]]; then
    hdr "4/4  Unlock and flash"
    [[ -x "$RECOVER" ]] || die "need $RECOVER to unlock this part" 1
    # recover.sh owns the unlock: it runs 01-preflight / 02-unlock / 03-verify,
    # checks FSTATR after the Configuration Set command, and programs with
    # loadbin. Duplicating that here would mean two copies to keep correct.
    "$RECOVER" --sn "$PROBE_SN" --bin "$BIN" --device "$DEVICE" --speed "$SPEED" \
        || die "recovery failed" $?
else
    hdr "4/4  Flash"
    # NEVER a bare `erase`. J-Link's Erase Chip also erases the config area,
    # which blanks BPS_SEL back to FFFFFFFF and instantly re-locks a recovered
    # part - and BPS/PBPS cannot be un-zeroed to compensate. loadbin erases only
    # the sectors it is about to write.
    cat > "$WORK/flash.jlink" <<EOF
device $DEVICE
si SWD
speed $SPEED
connect
r
h
loadbin $BIN 0x0
mem32 00000000 1
mem32 0100A2C0 1
r
g
qc
EOF
    jlink "$WORK/flash.jlink"
    if grep -qE '^\*+ Error|^ERROR:|Failed to erase' "$WORK/last.log"; then
        sed -n '/Error/,$p' "$WORK/last.log" | head -5
        die "programming failed. If it says \"Failed to erase sectors\", the part is
     poisoned and this run's preflight disagreed - re-run, or use
     tools/recovery/recover.sh --sn $PROBE_SN --bin $BIN"
    fi
    EXPECT=$(od -An -tx4 -N4 "$BIN" | tr -d ' \n' | tr 'a-f' 'A-F')
    GOT=$(readval 00000000)
    [[ "$GOT" == "$EXPECT" ]] || die "vector table mismatch: flash=$GOT bin=$EXPECT"
    SEL=$(readval 0100A2C0)
    if [[ "$BPS_SEL" != "FFFFFFFF" && "$SEL" != "$BPS_SEL" ]]; then
        die "BPS_SEL changed from $BPS_SEL to $SEL - a chip erase ran and re-locked the part"
    fi
    ok "programmed, initial SP $GOT"
fi

# ------------------------------------------------------- post-flash: did it come up?
hdr "Status"
# Symbol addresses move with every build, so resolve them from the ELF that was
# actually programmed rather than hardcoding them.
BUSB=""; DAPB=""
if command -v arm-none-eabi-nm >/dev/null && [[ -f "$ELF" ]]; then
    BUSB=$(arm-none-eabi-nm "$ELF" | awk '$3=="b_usb_configured"{print $1}')
    DAPB=$(arm-none-eabi-nm "$ELF" | awk '$3=="g_dap_bytes"{print $1}')
fi

sleep 3   # let the host enumerate it
{
    echo "device $DEVICE"; echo "si SWD"; echo "speed $SPEED"; echo "connect"
    echo "mem16 40090040 1"
    [[ -n "$BUSB" ]] && echo "mem8 $BUSB 1"
    [[ -n "$DAPB" ]] && echo "mem32 $DAPB 1"
    echo "q"
} > "$WORK/status.jlink"
jlink "$WORK/status.jlink"

INTSTS0=$(readval 40090040)
if [[ -n "$INTSTS0" ]]; then
    DVSQ=$(( (0x$INTSTS0 >> 4) & 7 ))
    case "$DVSQ" in
        3) ok "USB enumerated (DVSQ=Configured). Status LED should be SOLID." ;;
        7) ok "USB enumerated, host has suspended the bus. LED stays solid." ;;
        0|4) warn "USB NOT enumerated (DVSQ=Powered): the host has never bus-reset it.
     The device asserts its pull-up, so nothing is answering on D+/D-.
     That is a cable or connector fault, not firmware - a power-only USB
     cable produces exactly this. The LED will slow-blink until it is fixed." ;;
        *) warn "USB mid-enumeration (DVSQ=$DVSQ); re-run to see where it settles." ;;
    esac
fi
if [[ -n "$BUSB" ]]; then
    V=$(readval "$BUSB")
    [[ "$V" == "01" ]] && ok "b_usb_configured = 1" || warn "b_usb_configured = ${V:-?}"
fi

printf '\n\033[1m== Done\033[0m\n'
echo "  Expected LED: slow blink = not enumerated, solid = enumerated and idle,"
echo "                solid with brief blanks = SWD traffic (rate tracks throughput)."
