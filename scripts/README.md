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
--speed <kHz>    SWD speed (default 1000). Used for every J-Link session this
                 script opens, and passed through to recover.sh.
--bin <path>     Program something other than the default image.
```

`RA_JLINK_SN` supplies the default for `--sn`. If it is set to a non-empty
value, probe auto-detection and the "several J-Links attached" guard never run —
the script reports `using J-Link <serial>` without saying the serial came from
the environment. `--sn` on the command line still wins, and an empty
`RA_JLINK_SN` behaves as if unset. `tools/recovery/recover.sh` does *not* read
this variable; it only ever sees the serial `flash.sh` hands it via `--sn`.

Requires `JLinkExe` on PATH, and `arm-none-eabi-gcc` (plus `python3`) whenever it
has to build.

Exit codes: `0` flashed, `1` bad usage/environment, `2` unrecoverable (replace
the IC), `3` a step failed.

### When it rebuilds

`make` is invoked only when something under `ra4m2/src`, `ra4m2/ra_gen`,
`ra4m2/ra_cfg` or `ra4m2/Makefile` is newer than the image, so the common path
skips the toolchain entirely.

Three build inputs are *not* watched, and editing them will not trigger a
rebuild:

- `ra4m2/ra` — the FSP tree. Every `.c` under it is compiled in, and its headers
  are prerequisites through the generated `.d` files (`-MMD -MP`).
- `ra4m2/script/fsp.ld` — the linker script, a prerequisite of the ELF.
- `ra4m2/.secure_rzone` and `ra4m2/script/gen_memory_regions.py` — they generate
  `Debug/memory_regions.ld`, which `fsp.ld` includes.

After touching any of those, pass `--rebuild`, or run `make -C ra4m2` yourself.
Otherwise `flash.sh` prints `using existing image` and programs the previous
build.

The common path opens three J-Link sessions, not one — preflight, flash and
status — plus a fourth `ShowEmuList` session when `--sn` is omitted.

### The post-flash status report

After programming it resets the part, waits three seconds for the host, and
reports whether USB actually came up — reading `USBFS INTSTS0.DVSQ`
(`0x40090040`, bits [6:4]) and the firmware's own `b_usb_configured`, whose
address it resolves with `arm-none-eabi-nm` from
`ra4m2/Debug/CMSIS_DAP_RA4M2.elf` rather than hardcoding it. That distinguishes
the two failures that look identical from the outside: firmware that did not
start, and firmware that started perfectly but has nothing on the other end of
D+/D−.

Two caveats on that second reading. If `arm-none-eabi-nm` is not on PATH, or the
ELF is missing (fresh clone, after `make clean`, or `--no-build` against a
hand-supplied image), the symbol lookup is skipped without comment and you get
the `DVSQ` verdict only. And `--bin` changes the image programmed but not the
ELF the symbols come from, so under `--bin` the `b_usb_configured` line is read
from the *default* build's address and should be ignored.

## `build.sh`

```
--clean    Build from scratch.
--quiet    Only complain if something goes wrong.
```

Requires `arm-none-eabi-gcc` on PATH; a missing toolchain is a hard exit 1. It
does **not** check for `python3`, which the Makefile needs to generate
`memory_regions.ld`, so a missing python3 shows up as a build failure at the
`GEN` step rather than as a clean environment error.

Exit codes: `0` built or already current, `1` bad usage/environment (unknown
option, no toolchain, no `ra4m2/Makefile`), `3` the build failed, produced no
image, or produced one over 512 KB.

That size guard is the whole reason this script exists. It is not a fit check —
this part has 256 KB of code flash — it is a flat-binary trip-wire, sized to
catch a ~16 MB image with a wide margin. See below.

## The trap these scripts exist to avoid

Never program a flat `objcopy -O binary` image at address 0. The FSP ELF spans
`0x0`–`0x0100_A2CC`, so a plain `objcopy -O binary` emits a ~16 MB file
gap-filled with `0x00`, including at BPS (`0x0100_A1C0`) and PBPS
(`0x0100_A1E0`), and programming it block-protects code flash. `build.sh`
refuses any image over 512 KB; a correct code-flash-only image is ~54 KB (54,020
bytes as built today), which `ra4m2/Makefile` produces with `-R .option_setting
-R .option_setting_ns -R .option_setting_s -R .data_flash`.

`.srec` and `.elf` cannot reach BPS or PBPS, but they are **not** unconditionally
safe: both carry `BPS_SEL = 0xFFFFFFFF` at `0x0100_A2C0`, which re-locks a
recovered board. The registers, the manual citations and the recovery itself are
documented once, in
[tools/recovery/README.md](../tools/recovery/README.md#cause). This file does
not restate them.

## Where these scripts differ from `make flash`

- **Never run a bare J-Link `erase` on a recovered part.** Erase Chip also
  erases the config area, which blanks `BPS_SEL` back to `FFFFFFFF` and re-locks
  the board instantly — and `BPS`/`PBPS` can never be un-zeroed to compensate.
  `flash.sh` uses `loadbin`, which erases only the sectors it is about to write,
  and it fails loudly if `BPS_SEL` changes during a flash. `ra4m2/Makefile`'s own
  `flash` target *does* run a bare `erase`, so prefer this script.
- **`make flash` also passes a JLinkScript; these scripts do not.**
  `ra4m2/Makefile` passes `-JLinkScriptFile script/ra4m2_flash.JLinkScript`,
  which neither `flash.sh` nor `tools/recovery/recover.sh` supplies. That script
  hooks `SetupTarget()` *and* `ResetTarget()`, so before every erase it raises
  ICK/FCK to MOCO/1 (~8 MHz) via `SCKDIVCR`, clears `FMEPROT` so code-flash P/E
  mode can be entered at all, sets `FPCKAR` to match FCLK, and cancels
  `FBPROT0`/`FBPROT1`. Its header argues the clock step is required because the
  reset dividers leave FCLK near 2 MHz, under the sequencer's 4 MHz floor. The
  scripts here program correctly without any of it: `tools/recovery/02-unlock.jlink`
  hand-issues a raw FACI Configuration Set and `03-verify.jlink` runs a ranged
  code-flash erase, both bare, and `tools/recovery/README.md` records both
  passing along with a full reflash — so the J-Link RA4M2 device support in use
  evidently sets up whatever it needs itself. That premise has not been re-tested
  against the J-Link version currently installed.
- **`Failed to erase sectors` does not identify its own cause.** This README and
  `tools/recovery/README.md` teach it as the signature of zeroed `BPS`/`PBPS`,
  but `ra4m2/script/ra4m2_flash.JLinkScript` blames the same string on a too-slow
  FCLK, and names a third candidate: an `FPCKAR` that does not match FCLK. Read
  `BPS` (`0x0100_A1C0`) and `PBPS` (`0x0100_A1E0`) before concluding a part is
  poisoned — `flash.sh` does exactly that in its preflight, which is why it can
  tell the cases apart.
- A recovered board reads `BPS = PBPS = 00000000` forever. That is not a
  failure; `BPS_SEL` is what decides whether anyone consults them.

## Reading the status LED

DS11 on P111, active low. The user-facing table lives in the
[root README](../README.md#status-led); the interpretation of a board that
slow-blinks anyway is in [docs/TROUBLESHOOTING.md](../docs/TROUBLESHOOTING.md).

What `flash.sh` itself prints is narrower than that page: it decodes
`INTSTS0.DVSQ` (`0x40090040`, bits [6:4]) and reads `b_usb_configured`, and
nothing else. It does **not** read `FRMNUM`. If you want that second witness,
read `0x4009004C` by hand and read it **16 bits wide** — `UFRMNUM` sits
immediately above it at `0x4009004E`, so a `mem32` straddles both registers.
