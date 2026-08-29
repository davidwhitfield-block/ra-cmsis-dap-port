---
name: ra4m2-unbrick
description: Recover an RA4M2 (R7FA4M2xx) whose code flash refuses to erase or program because BPS/PBPS were zeroed by a flat objcopy binary. Use when a J-Link erase or flash fails with "Failed to erase sectors" / "error code -5", when FSTATR reads 0x0080C000 and FASTAT reads 0x10, when BPS (0x0100A1C0) or PBPS (0x0100A1E0) read 0x00000000, or when an RA board is described as bricked, block-protected, permanently locked, or unflashable.
---

# Unbricking an RA4M2 with poisoned block protection

**Do not conclude the part is scrap.** A part with `BPS = PBPS = 0x00000000` is
recoverable in about 30 seconds. The manual calls that state permanent, and it
is — but only for the *applied* register pair, and a third register chooses
which pair is applied.

## Just run the script

```sh
cd tools/recovery
./recover.sh --sn <jlink-serial> --bin <code-flash-only.bin>
```

It is idempotent, verifies every step, and refuses to program an image large
enough to be a flat objcopy binary. Exit codes: `0` recovered, `1` bad
usage/environment, `2` unrecoverable (replace the IC), `3` a step failed.

On this project's bench there are two probes attached — always pass
`--sn 821000843`, never the STM32U5 one.

Read `tools/recovery/README.md` before deviating from the script. It documents
every register, every manual citation, and the two mistakes that cost the most
time.

## Confirm the diagnosis first

Symptom is any code-flash erase/program failing with:

```
****** Error: Failed to erase sectors.
ERROR: Erase returned with error code -5.
```

Read these and match against the table:

| Register | Address | Poisoned |
|---|---|---|
| BPS | `0x0100A1C0` | `00000000` |
| PBPS | `0x0100A1E0` | `00000000` |
| BPS_SEC | `0x0100A240` | `FFFFFFFF` ← required for recovery |
| PBPS_SEC | `0x0100A260` | `FFFFFFFF` ← required for recovery |
| BPS_SEL | `0x0100A2C0` | `FFFFFFFF` |
| DLMMON | `0x400E002C` | `00000001` (CM) or `2` (SSD) |

**Read BPS and PBPS, not the `_SEC` copies.** The `_SEC` pair reads `FFFFFFFF`
on a poisoned part; mistaking them for the applied pair leads to "block
protection is fine, must be something else" and hours of dead ends.

If `BPS_SEC`/`PBPS_SEC` are *not* `FFFFFFFF`, there is no escape route — the
IC must be replaced. That is the only genuinely unrecoverable case.

## Why it works, in one paragraph

`BPS_SEL` (`0x0100A2C0`) selects, per block, whether the applied block-protect
and permanent-block-protect settings come from the non-secure pair (poisoned) or
the secure pair (pristine). Setting its bits to 0 switches to the secure pair,
which is blank, so erase and program become valid again. RA4M2 User's Manual
R01UH0892EJ0150 §6.2.5; Table 44.18 lists `BPS_SEL` as unconditionally
"Writable" while every other protect register carries a "cannot be restored to
1" note; Table 44.22's freeze covers only `BPS[n]`/`PBPS[n]`. The poisoned
BPS/PBPS stay zero forever and are simply never consulted again.

## Hard-won gotchas

- **`FWEPROR` is at `0x4001E416`** — SYSC base, *not* the FACI base
  `0x407F_E000`. Writing `0x407FE416` silently does nothing and shows up later
  as FLWEERR, `FSTATR = 0x00008040`.
- **FACI command cycles are BYTE writes.** The opcode, the `N` count and the
  trailing `0xD0` must be 8-bit; only the data words are 16-bit. A 16-bit
  opcode gives `FSTATR = 0x0080C000` (ILGCOMERR+ILGLERR).
- **Reset after writing BPS_SEL.** It applies at reset, not immediately.
- **Never run a bare J-Link `erase`** on a recovered part. Erase Chip also
  erases the config area, blanking BPS_SEL and instantly re-locking the board.
  Use a ranged erase or plain `loadbin`. If it happens, just re-run the script.
- **`FENTRYR = 0xAA01`** is code flash. `0xAA80` is data flash; `0xAA81`
  deliberately faults.

## Preventing recurrence

The cause is `objcopy -O binary` on an FSP ELF that spans `0x0`–`0x0100_A2CC`:
it emits a 16 MB file gap-filled with `0x00`, which carries zeros at the BPS and
PBPS offsets, and `loadbin ... 0x0` programs them.

A correct code-flash-only `.bin` is ~54 KB. `ra4m2/Makefile` builds it with
`-R .option_setting -R .option_setting_ns -R .option_setting_s -R .data_flash`.
**Keep it that way.**

The `.srec` and `.elf` cannot reach BPS or PBPS — neither format carries gap
padding — but they are **not** unconditionally safe. Both place
`g_bsp_rom_bps_sel0 = 0xFFFFFFFF` at `0x0100A2C0` = BPS_SEL, via
`.option_setting_bps_sel0` inside the loadable `.option_setting_s` section
(`script/fsp.ld`, FSP's `bsp_rom_registers.c`). Verify on any build:

```sh
arm-none-eabi-nm ra4m2/Debug/*.elf | grep bps_sel0   # 0100a2c0 r g_bsp_rom_bps_sel0
grep 0100A2C0 ra4m2/Debug/*.srec                     # S3110100A2C0FFFFFFFF...
```

On a **virgin** part that is a no-op. On a **recovered** part `loadfile` of either
format rewrites BPS_SEL to `0xFFFFFFFF`, which re-selects the poisoned
BPS/PBPS pair and re-locks the board at the next reset. Recoverable — just re-run
`recover.sh` — but do not reach for the `.elf` or `.srec` as the "safe" option on a
board that has already been through recovery. Use the `.bin`.

Tell-tale that distinguishes this from any other failure: **code flash above
the image reads `0x00`, not `0xFF`.** Erased flash is always `0xFF`, so `0x00`
padding proves a zero-filled image was programmed.

## After recovery

Confirm the board enumerates:

```sh
ioreg -p IOUSB -l -w 0 | grep -A3 "RA4M2 CMSIS_DAP Probe"
```

Expect Renesas VID `0x045B`, PID `0x201F`. Note that halting the core does
**not** drop the USB device from `ioreg` — the D+ pullup stays asserted — so
that is not a valid way to tell two boards apart. Unplug the others instead.
