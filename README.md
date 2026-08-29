# ra-cmsis-dap-port

## Overview

A port of Arm's CMSIS-DAP debug probe firmware to the Renesas RA MCU family
(https://www.renesas.com/ra). This fork targets an **R7FA4M2AB3CNE** (48-pin QFN,
256 KB code flash, 128 KB SRAM, 8 KB data flash) fitted as **U21, the on-board
debug MCU of the X2C Automation board**, where it debugs the SPE MCU over SWD:
`DEBUG_SPE_SWDIO` on P101, `DEBUG_SPE_SWCLK` on P102, `DEBUG_SPE_SWO` on P100 and
`SPE_MCU_RESET_L` on P112. Probe and target are on the same PCB, so in the normal
case there is nothing to wire.

Every authoritative artefact agrees on the target: `ra4m2/configuration.xml` sets
`#Board# = board.x2c_automation` and `#TargetName# = R7FA4M2AB3CNE`,
`ra4m2/Makefile` sets `DEVICE = R7FA4M2AB`, and `ra4m2/src/board_cfg.h` includes
`../ra/board/x2c_automation/board.h`.

It began as Renesas' EK-RA4M2 example (the `upstream` git remote here is
ersatzavian/ra-cmsis-dap-port); the pinout, LED behaviour, DAP packet sizing and
SWD clock calibration have all changed since — see
[ra4m2/src/CMSIS-DAP/DAP_config.h](ra4m2/src/CMSIS-DAP/DAP_config.h) and
[ra4m2/src/dap_thread_entry.c](ra4m2/src/dap_thread_entry.c). Sections below that
still describe the EK-RA4M2 are labelled as such.

The port is an RA Flexible Software Package (FSP) project, buildable in the
e<sup>2</sup> studio IDE (installers at https://github.com/renesas/fsp), and it
also carries a standalone GNU make build ([ra4m2/Makefile](ra4m2/Makefile)) that
needs neither e<sup>2</sup> studio nor the FSP installer.

Background information on CMSIS-DAP can be found in Arm's documentation:
https://arm-software.github.io/CMSIS_5/DAP/html/index.html

The CMSIS-DAP sources used as the basis of this port are taken from the CMSIS
5.9.0 Pack file at https://github.com/ARM-software/CMSIS_5/releases, and have
**local modifications** — see
[ra4m2/src/CMSIS-DAP/README.md](ra4m2/src/CMSIS-DAP/README.md) for the per-file
record.

> **A note on `ra4m2/R7FA4M2AD3CFP.pincfg`.** It is a leftover 100-LQFP/JTAG pin
> file from the EK-RA4M2 target and `configuration.xml` still names it under
> `#pinconfiguration#`, but that `pincfg` block is marked `active="false"
> selected="false"`. The live pin set is the `X2C_AUTOMATION` block
> (`active="true" symbol="g_bsp_pin_cfg"`), which is what `ra4m2/ra_gen/pin_data.c`
> emits and what actually runs. The filename is stale noise, not evidence.

## Repo layout

| Path | What is in it |
| --- | --- |
| `ra4m2/` | The FSP project. `src/` is ours; `src/CMSIS-DAP/` is Arm's reference implementation **with local modifications**; `ra/` and `ra_gen/` are generated or vendored — do not hand-edit them |
| `ra4m2/Makefile` | Standalone GNU make build, mirroring `.cproject`. Use it to *build* only: its `flash` target runs a bare J-Link `erase`, which on a recovered part blanks `BPS_SEL` and re-locks the chip. Use `scripts/flash.sh` instead |
| `scripts/` | [`build.sh` and `flash.sh`](scripts/README.md) — the everyday path |
| `tools/` | [Host-side tools](tools/README.md): `daptest` (functional and throughput suite over CMSIS-DAP v2 bulk, in C), `rttpull.py` (RTT drain and SystemView capture over pyOCD, no J-Link needed), [`recovery/`](tools/recovery/README.md) (unlock a block-protected part) |
| `docs/` | [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) — the failures that have actually happened on this bench, and what was ruled out |
| `Supporting Documents/parts/` | Renesas RA4M2 datasheet, hardware user's manual, standard boot firmware, device lifecycle management |
| `.claude/skills/ra4m2-unbrick/` | Agent skill wrapping the recovery procedure |
| `pics/` | Photos and screenshots inherited from the upstream EK-RA4M2 port |

## Port Details

### CMSIS-DAP Version

This port is based on the CMSIS-DAP 2.1.1 sources from the CMSIS 5.9.0 Pack file
(`ra4m2/src/CMSIS-DAP/DAP.h`: `$Revision: V2.1.1`, `DAP_FW_VER "2.1.1"`). It is a
**v2 probe**: USB bulk endpoints bound to WinUSB, with SWO Trace support.

> This repo is a fork and carries no releases of its own — `git tag -l` is empty
> here and on `upstream`. The original Renesas repo also shipped a USB HID variant
> (PID 0x201D, no SWO). It was removed in commit `05d2703` "Use Vendor Class
> instead of HID" and cannot be built from this tree: there is no HID descriptor
> anywhere in it, and `DAP_FW_V1` is never defined.

### SWD only

`DAP_JTAG` is `0` (`ra4m2/src/CMSIS-DAP/DAP_config.h`), so the JTAG bit is clear in
the `DAP_Info` Capabilities word and a `DAP_Connect` request for JTAG cannot be
honoured; `DAP_DEFAULT_PORT` is `1U` (SWD). The `PIN_CMSIS_DAP_TDI` / `_TDO` /
`_NTRST` defines in `ra4m2/ra_cfg/fsp_cfg/bsp/bsp_pin_cfg.h` are FSP default label
placements on P000/P001/P002 that arrived with the X2C retarget; the active
`X2C_AUTOMATION` pin configuration selects `debug0.mode.swd` and applies no port-0
pin data at all, so `ra4m2/ra_gen/pin_data.c` configures none of those pads. They
exist only to keep the vendored `PIN_TDI_*` / `PIN_TDO_*` / `PIN_nTRST_*` stubs
compiling. On the X2C board P000/P001/P002 are no-connects and the probe MCU's own
DEBUG0 TDI (P110) is a no-connect too, so JTAG is unreachable in hardware as well
as in software.

### VCOM

The port provides a VCOM (virtual COM) port, allowing a UART to USB bridge link
from the target device's UART to the host PC via the CMSIS-DAP Probe. It is SCI2
(`ra4m2/ra_gen/dap_thread.c`: `.channel = 2`), TXD on P302 and RXD on P301.

### VID / PID

USB VID = `0x045B` (the Renesas VID) with PID = `0x201F`, a PID reserved by
Renesas for this CMSIS-DAP port
(`ra4m2/src/r_usb_pcdc_pvnd_descriptor.c`). The device is a composite: a
vendor-class (0xFF) CMSIS-DAP interface, advertised to Windows as WinUSB via the
MS OS 1.0 compatible-ID descriptor, plus a CDC VCOM pair. The older HID variant
used PID `0x201D`; the PID was changed so Windows would not reuse the cached HID
driver binding after the class changed to WinUSB.

The literal substring `CMSIS-DAP` in the product **and interface** strings is
functional, not decorative — host tools filter on it. See the comments in
`r_usb_pcdc_pvnd_descriptor.c` before renaming anything.

## SWO Support

SWO Trace is supported, UART SWO only (`SWO_UART 1`, `SWO_MANCHESTER 0` in
`ra4m2/src/CMSIS-DAP/DAP_config.h`). The maximum SWO clock frequency is 2.5 MHz
(`SWO_UART_MAX_BAUDRATE 2500000U`). The capture path is SCI0 RXD on P100
(`ra4m2/ra_gen/swo_thread.c`: `.channel = 0`); SCI0 has no TXD assigned, so P100
is receive-only.

[![MDK SWO Trace Configuration](pics/MDK_SWO_Config-sm.jpg)](pics/MDK_SWO_Config.jpg)

### Status LED

One LED: DS11 on P111 (`JLINK_OB_LED_L`), wired 3V3 → DS11 → R53 470R → P111, so
it is **active low** — the MCU sinks the current. Semantics match a SEGGER J-Link:

| LED | Meaning |
| --- | --- |
| Slow blink, 1 Hz | Not enumerated |
| Solid | Enumerated, no SWD traffic |
| Solid with brief blanks | SWD traffic; the blank *rate* tracks throughput |

The activity indication is a blank, not a square wave. A symmetric toggle fast
enough to track a bulk transfer lands near 12 Hz and the eye integrates it into a
dim steady glow, so instead one 45 ms blank is emitted per `ACTIVITY_LED_BYTES`
(48 KiB) moved, with the on phase clamped between 150 ms and 1200 ms
(`ra4m2/src/dap_thread_entry.c`).

The CMSIS-DAP connect, target-running and VCOM indicators are disabled
(`LED_INDEX_CONNECTED` / `_RUNNING` / `_VCOM` are `-1` in
`ra4m2/src/CMSIS-DAP/DAP_config.h`) so they do not fight the USB status indicator
for the one LED.

A board that slow-blinks while the host claims it is enumerated is almost always a
power-only USB cable — see [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).

### Pin Usage

The probe firmware in this repo is built for the X2C Automation board
(`R7FA4M2AB3CNE`), not the EK-RA4M2. Connections to the target:

| Probe pin | Target signal | Notes |
| --- | --- | --- |
| GND | GND | |
| P101 | SWDIO | Fast path: direct `R_PORT1` PCNTR1/2/3 access, no PFS |
| P102 | SWCLK | Fast path |
| P112 | nRESET | CMOS push-pull, not open-drain — there is no pull-up on the 3.3 V side (commit `ac60203`) |
| P100 | SWO / target TDO | SCI0 RXD. UART SWO only |
| P302 | target UART RX | VCOM TXD, SCI2 |
| P301 | target UART TX | VCOM RXD, SCI2. On this board the net reaches TP105 only and nothing drives it (`ra4m2/ra_gen/pin_data.c`), so host-to-target VCOM works out of the box but target-to-host needs a wire to that test point |

The SWD pin assignments are hard-coded for speed in
`ra4m2/src/CMSIS-DAP/DAP_config.h` (`DAP_SWD_PORT R_PORT1`, `DAP_SWDIO_BIT 1U`,
`DAP_SWCLK_BIT 2U`, `DAP_NRESET_BIT 12U`) and static-asserted against the assembly
fast path in `ra4m2/src/CMSIS-DAP/SW_DP.c`. They must stay in step with
`ra4m2/ra_cfg/fsp_cfg/bsp/bsp_pin_cfg.h`.

All target-side signals pass through one level shifter, U16 (TXB0104PWR), whose
`OE` (`SPE_JLINK_LEVEL_SHIFTER_EN`) is driven by board logic and reaches **no MCU
pin** — this firmware cannot enable it. With the target's 1.8 V rail down every
`DEBUG_SPE_*` line is undriven.

Not target connections, listed so they are not mistaken for one:

- **P111** — status LED DS11, active low, push-pull, idle high = off.
- **P108 / P109 / P300** — the probe MCU's own SWD port (SWDIO / SWO / SWCLK),
  used by the J-Link that programs this board.
- **P407** — USB VBUS sense. Without it the device does not enumerate.

> **The pre-`ecfba94` EK-RA4M2 pinout was different, and following it is
> dangerous.** That build put SWCLK on P402, RESET on P403, SWDIO on P406 and the
> VCOM pair on P113/P112. P112 is now nRESET, driven push-pull, so wiring by the
> old table connects a reset output straight into the target's UART RX pin. The
> deactivated `RA4M2 EK` block in `ra4m2/configuration.xml` is the only place that
> mapping still survives.

## Building

Two ways in. `scripts/build.sh` wraps the Makefile and adds one safety check, so
prefer it.

```sh
scripts/build.sh              # --clean to build from scratch, --quiet to only report failures
cd ra4m2 && make              # BUILD=Release selects the output directory only
```

Artefacts land in `ra4m2/Debug/` (or `ra4m2/Release/`): `.elf`, `.srec`, and a
code-flash-only `.bin` (54,020 bytes at the time of writing). `build.sh` refuses to
hand back a `.bin` larger than code flash — see
[scripts/README.md](scripts/README.md) for why that guard exists.

Do not run `make flash`. It issues a bare J-Link `erase`, which also erases the
config area and re-locks a previously recovered board. Use `scripts/flash.sh`,
which uses `loadbin` and erases only the sectors it writes.

Prerequisites:

- `arm-none-eabi-gcc` on `PATH`. `ra4m2/configuration.xml` records
  `#ToolchainVersion# = 10.3.1.20210824`, the version the e<sup>2</sup> studio
  project was generated against; the checked-in `Debug/` tree was built with a
  much newer one. Both work.
- `python3`, which the Makefile runs to generate `memory_regions.ld` from
  `.secure_rzone`. `scripts/build.sh` does not check for it, so a missing python3
  surfaces as a failure at the `GEN` step.
- `JLinkExe`, for programming only.

The GNU make build works from a bare clone — the X2C Automation BSP is vendored at
`ra4m2/ra/board/x2c_automation/`. Re-generating the configuration in e<sup>2</sup>
studio is a different matter: `ra4m2/configuration.xml` names the
`Block##BSP##Board##x2c_automation##` fragment and a
`Block.X2C_AUTOMATION.6.2.0.pack` component, so that non-Renesas pack must be
installed alongside FSP 4.6.0. Its `bsp_linker.c` must stay excluded from the
build — FSP 4.6 does not emit the `bsp_linker_info.h` it needs, and it would
duplicate the `.option_setting_*` arrays already in `bsp_rom_registers.c`.

`BUILD=Release` names the output directory and nothing else: `.cproject`'s Debug
and Release configurations carry identical flags (both `-O2`, neither defines
`NDEBUG`), and every script here hardcodes `Debug/`. All measured figures in this
repo come from the Debug tree.

## Flashing

```sh
scripts/flash.sh              # --sn <jlink-serial> only when more than one probe is attached
```

Builds if needed, unlocks the board if its block protection was poisoned, then
programs it. Safe to run repeatedly and on any board state — blank, healthy, or
locked. See [scripts/README.md](scripts/README.md).

## Recovering a block-protected board

> **Never program a flat `objcopy -O binary` image at address 0.** The FSP ELF
> spans `0x0`–`0x0100_A2CC`, so a plain `objcopy -O binary` emits a 16 MB file
> gap-filled with `0x00` — including at BPS (`0x0100_A1C0`) and PBPS
> (`0x0100_A1E0`). Programming it permanently block-protects code flash. A
> correct code-flash-only `.bin` is ~54 KB.
>
> `.srec` and `.elf` cannot reach BPS or PBPS, so on a **virgin** part they are
> safe. They are **not** safe on a *recovered* part: both carry
> `g_bsp_rom_bps_sel0 = 0xFFFFFFFF` at `0x0100_A2C0`, which is exactly the value
> that re-selects the poisoned register pair. `loadfile` of an ELF or SREC would
> erase and rewrite that word and re-lock the board.

If a board's code flash refuses to erase or program (`Failed to erase sectors`,
`error code -5`, `FSTATR = 0x0080C000`), it can be recovered without replacing
the IC:

```sh
cd tools/recovery
./recover.sh --sn <jlink-serial> --bin ../../ra4m2/Debug/CMSIS_DAP_RA4M2.bin
```

See [tools/recovery/README.md](tools/recovery/README.md) for the mechanism and
the manual references. A recovered board reads `BPS = PBPS = 0` forever after —
that is a healthy steady state, not a fault, because `BPS_SEL` decides whether
anyone consults them.

## Verified hosts and targets

The port has been retargeted since the upstream EK-RA4M2 build, so the upstream
results below have not been re-run. This tree changed `DAP_PACKET_SIZE` 64 → 1024
and `DAP_PACKET_COUNT` 255 → 8, disabled JTAG (`DAP_JTAG 0`), recalibrated
`CPU_CLOCK` to 96 MHz and `IO_PORT_WRITE_CYCLES` to 13, and moved SWD off the
EK-RA4M2 header onto P101/P102/P112, driven by direct `R_PORT1` register writes
(all in `ra4m2/src/CMSIS-DAP/DAP_config.h`).

| Host | Target | Status |
| --- | --- | --- |
| `tools/daptest` (libusb, CMSIS-DAP v2 bulk) | Silicon Labs EFR32MG24, Cortex-M33 | **Verified on this build.** 5330 DAP round trips/s, 312.8 KiB/s read, 313.5 KiB/s write, 0 errors. See [tools/README.md](tools/README.md) |
| pyOCD via `tools/rttpull.py` | same EFR32MG24, `--target cortex_m` | **Verified for memory read/write only** — RTT drained at 241 KiB/s. Flash programming through pyOCD has not been exercised here, and no pyOCD version is pinned |
| Keil MDK 5.38a (RA DFP flash loaders) | RA Family | Upstream result against the EK-RA4M2 build. Not retested since the retarget |
| IAR EWARM 9.32.x (IAR flash loaders) | RA Family | Upstream result against the EK-RA4M2 build. Not retested since the retarget |
| PyOCD 0.34.1 (RA DFP flash loaders) | RA Family | Upstream result against the EK-RA4M2 build. Not retested since the retarget |

Nothing in this repo records an RA Family target being debugged with *this* build.

The SWD ceiling is ~3.7 MHz, or ~313 KiB/s: 46 SWD clocks per 32-bit word is the
ADIv5 floor (8 request + 1 turnaround + 3 ACK + 32 data + 1 parity + 1 turnaround),
and the RA4M2's I/O ports sit on PLBIU at PCLKB, which prices a port access at
2–5 PCLKB. The derivation is in [tools/README.md](tools/README.md#reference-numbers).

### Tools

The project builds with FSP 4.6.0 and e<sup>2</sup> studio 2023-07
(https://github.com/renesas/fsp/releases/tag/v4.6.0), or from the command line
with `arm-none-eabi-gcc` — see `scripts/build.sh`. Re-running RA content
generation additionally requires the X2C Automation BSP pack
(`ra4m2/configuration.xml`, vendor "Block", version 6.2.0) for the
`R7FA4M2AB3CNE` target.

### Pictures (upstream EK-RA4M2 port)

These images are inherited from the upstream EK-RA4M2 project and are kept for
reference only. They do **not** show the X2C Automation board this fork targets,
and the P4xx wiring visible in ProbeConnections_1/2 is **not** the pinout this fork
uses. The authoritative pin assignments are the table above and
`ra4m2/ra_cfg/fsp_cfg/bsp/bsp_pin_cfg.h`.

The below show an EK-RA4M2 board being used as a CMSIS-DAP debug probe for
debugging an EK-RA4M3 board:

[![EK-RA4M2 in use as a CMSIS-DAP Probe #1](pics/ProbeConnections_1-sm.jpg)](pics/ProbeConnections_1.jpg) [![EK-RA4M2 in use as a CMSIS-DAP Probe #2](pics/ProbeConnections_2-sm.jpg)](pics/ProbeConnections_2.jpg)

Due to the pitch of the pins on a standard Cortex-M debug header, an EK-RA4M2 wired
up this way is easier to use with an adapter such as Embedded Artists' 10-pin to
20-pin JTAG Adapter (https://www.embeddedartists.com/products/10-pin-to-20-pin-jtag-adapter/).
On the X2C Automation board the probe and the SPE MCU share a PCB, so this does not
apply.

The below show the board as it appears in Windows 10 Devices and Printers:

[![Probe in Devices & Printers](pics/Probe_Devices_Printers-sm.jpg)](pics/Probe_Devices_Printers.jpg)

winusb driver — what this tree builds today (VID `0x045B`, PID `0x201F`, CDC plus
vendor class with a `WINUSB` compatible ID):

[![Probe Properties (winusb)](pics/Probe_Properties_winusb-sm.jpg)](pics/Probe_Properties_winusb.jpg)
