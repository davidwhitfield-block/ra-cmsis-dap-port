# Build and flash scripts

Two entry points. `flash.sh` is the one you want; it calls `build.sh` for you.

```sh
scripts/flash.sh --sn 821000843
```

Run it as often as you like. It is idempotent, and it is designed so that the
same command works on every state a board turns up in:

| Board state | What happens |
|---|---|
| Never built | Builds first, once |
| Blank / erased chip | Programs it |
| Healthy chip | Reprograms it, touching nothing else |
| Poisoned (`BPS = PBPS = 0`) | Unlocks via `tools/recovery`, then programs |
| Already unlocked | Notices `BPS_SEL`, skips the unlock |

## `flash.sh`

```
--sn <serial>    J-Link to program through. Required when more than one probe
                 is attached; auto-detected when exactly one is.
--rebuild        Force a rebuild first.
--no-build       Never build; fail if there is no image.
--device <name>  J-Link device name (default R7FA4M2AB).
--bin <path>     Program something other than the default image.
```

Exit codes: `0` flashed, `1` bad usage/environment, `2` unrecoverable (replace
the IC), `3` a step failed.

It only invokes `make` when a source file is newer than the image, so the common
path costs one J-Link session and nothing else.

After programming it resets the part, waits for the host, and reports whether
USB actually came up — reading `USBFS INTSTS0.DVSQ` and the firmware's own
`b_usb_configured`, whose address it resolves from the ELF that was just
programmed rather than hardcoding it. That distinguishes the two failures that
look identical from the outside: firmware that did not start, and firmware that
started perfectly but has nothing on the other end of D+/D-.

## `build.sh`

```
--clean    Build from scratch.
--quiet    Only complain if something goes wrong.
```

Refuses to hand back an image larger than code flash. That guard is the whole
reason it exists — see below.

## The trap these scripts exist to avoid

Never program a flat `objcopy -O binary` image at address 0. The FSP ELF spans
`0x0`–`0x0100_A2CC`, so a plain `objcopy -O binary` emits a ~16 MB file
gap-filled with `0x00`, including at BPS (`0x0100_A1C0`) and PBPS
(`0x0100_A1E0`). Programming it permanently block-protects code flash. A correct
code-flash-only image is ~54 KB; `ra4m2/Makefile` produces it with
`-R .option_setting -R .option_setting_ns -R .option_setting_s -R .data_flash`.
Both `.srec` and `.elf` are safe — neither format carries gap padding.

Two related hazards, both handled here:

- **Never run a bare J-Link `erase` on a recovered part.** Erase Chip also
  erases the config area, which blanks `BPS_SEL` back to `FFFFFFFF` and
  re-locks the board instantly — and `BPS`/`PBPS` can never be un-zeroed to
  compensate. `flash.sh` uses `loadbin`, which erases only the sectors it is
  about to write, and it fails loudly if `BPS_SEL` changes during a flash.
  `ra4m2/Makefile`'s own `flash` target *does* run a bare `erase`, so prefer
  this script.
- A recovered board reads `BPS = PBPS = 00000000` forever. That is not a
  failure; `BPS_SEL` is what decides whether anyone consults them.

## Reading the status LED

DS11 on P111, active low.

| LED | Meaning |
|---|---|
| Slow blink (1 Hz) | Not enumerated |
| Solid | Enumerated, no SWD traffic |
| Solid with brief blanks | SWD traffic; the blank *rate* tracks throughput |

A board that slow-blinks while the host claims it is enumerated is almost always
a cable: a power-only USB cable leaves VBUS present and the D+ pull-up asserted,
so the firmware does everything right and no host ever answers. `INTSTS0.DVSQ`
stuck at `0b100` (suspended-from-Powered, i.e. never bus-reset) with `FRMNUM`
stuck at `0` confirms it. `flash.sh` prints exactly this diagnosis.
