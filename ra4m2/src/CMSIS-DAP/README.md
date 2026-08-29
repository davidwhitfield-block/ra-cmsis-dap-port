# Vendored CMSIS-DAP, with local modifications

These files are ARM's CMSIS-DAP reference implementation, edited in place.
Nothing in the files themselves says which ones were edited: every ARM-derived
file here carries the same stock Apache header and a `$Revision:` line, and the
root `README.md` only records that "The CMSIS-DAP sources used as the basis of
this port are taken from the CMSIS 5.9.0 Pack file". This file is that record.

Upstream: CMSIS-DAP 2.1.1 from the ARM CMSIS 5.9.0 Pack
(https://github.com/ARM-software/CMSIS_5/releases).

## The `$Revision:` banner is not maintained

The banner near the top of each ARM-derived file is the *upstream* revision and
is never bumped when the file is edited:

| File | Banner | Actually |
|---|---|---|
| `DAP_config.h` | `V2.1.0` | ~708 lines, +135/−40 against the import |
| `SW_DP.c` | `V2.0.0` | +104/−8, contains a hand-written `SWD_ReadData32()` |
| `SWO.c` | `V2.0.1` | +6/−3 |
| `cmsis_os2.h` | `V2.1.3` | a 30-line stub, not the CMSIS-RTOS2 API |
| `DAP.c` | `V2.1.1` | genuinely untouched |

Do not read the banner as evidence a file is clean. Read the tables below, or
regenerate them.

## Regenerating this record

Three points matter. `9ef5816` ("Beta 06.02.2023") is the commit that first
imported CMSIS-DAP, under the path prefix `src/CMSIS-DAP/`; `fbc4240` moved the
tree to `ra4m2/src/CMSIS-DAP/`. `5fa94fd` is the last upstream commit before this
fork begins at `ecfba94`.

```sh
# Everything that diverges from the ARM Pack import
git diff --stat 9ef5816:src/CMSIS-DAP HEAD:ra4m2/src/CMSIS-DAP

# Only what upstream renesas/ra-cmsis-dap-port changed
git diff --stat 9ef5816:src/CMSIS-DAP 5fa94fd:ra4m2/src/CMSIS-DAP

# Only what this fork changed
git diff --stat 5fa94fd:ra4m2/src/CMSIS-DAP HEAD:ra4m2/src/CMSIS-DAP
```

`osObjects.h` shows as an addition rather than a modification: it has no blob at
`9ef5816`.

| File | Import → fork point | Fork point → HEAD |
|---|---|---|
| `DAP_config.h` | +4 / −5 | +132 / −36 |
| `SW_DP.c` | — | +104 / −8 |
| `SWO.c` | +6 / −3 | — |
| `cmsis_os2.h` | +76 / −0 | — |
| `osObjects.h` | +1 / −0 | — |

(Counts predate this documentation pass, which added comments to `DAP.c`,
`DAP_config.h`, `SWO.c` and `device.h` without changing any code.)

## Modified files

Re-vendoring from a newer Pack means re-applying every row.

### `DAP_config.h`

Substantially rewritten. Upstream ships this file as a *template* — its own title
line says `DAP_config.h CMSIS-DAP Configuration File (Template)` — so divergence
is expected. What is **not** obvious is that four of its constants are *measured*
values with their reasoning written out in the file. Reverting any of them
silently costs throughput or mis-scales the SWD clock.

| Constant | Here | Upstream | Why |
|---|---|---|---|
| `CPU_CLOCK` | 96 MHz | 100 MHz | Real ICLK. Feeds `MAX_SWJ_CLOCK()` and the `clock_delay` calculation in `DAP.c` |
| `IO_PORT_WRITE_CYCLES` | 13 | 2 | Measured. RA4M2 I/O ports are on PLBIU at PCLKB |
| `DAP_PACKET_SIZE` | 1024 | 64 | v2 bulk; a DAP packet is a USB *transfer*, not a 64-byte HID report |
| `DAP_PACKET_COUNT` | 8 | 255 | 2 × 8 × 1024 = 16 KB of bss on a 128 KB part |
| `DAP_JTAG` | 0 | 1 | Physically impossible on this board — TDI (P110) is a no-connect |
| SWD pin map | `R_PORT1`, PCNTR1/2/3 fast macros | generic | P101/P102/P112 |
| Status LED block | active-low P111 | EK-RA4M2 LEDs | DS11 sinks current |
| `SWO_UART` | 1 | 0 | Turning SWO on is what makes `SWO.c` and the SWO pipe live at all |
| `SWO_STREAM` | 1 | 0 | " |
| `SWO_UART_MAX_BAUDRATE` | 2.5 MHz | 10 MHz | **No recorded rationale** in the file or the commit message, and not re-derived here |

Origin: this fork `ecfba94`, `dc4a81b`, `8fd9ef0`, `083ad48`, `e7dd16f`;
upstream `05d2703`, `5029f06`, `d3c1b6f`.

### `SW_DP.c` (this fork, `e7dd16f`)

Adds `SWD_ReadData32()` plus a `SW_READ_DATA32()` macro redefined per
instantiation — `SWD_ReadData32()` for Fast, the portable loop for Slow. The 8
removed lines are the old inline read loop, lifted verbatim into
`SW_READ_DATA32_GENERIC()` so Slow still has it.

8 instructions per bit down to ~3; the full argument, including why it stays in
flash rather than `.code_in_ram`, is the comment block at the top of the file.
Fast path only — Slow must carry `PIN_DELAY_SLOW` on every edge to honour a
sub-maximum clock request. Two `_Static_assert`s tie the hard-coded
`SWD_SWDIO_CARRY_SHIFT` back to `DAP_SWDIO_BIT`, so a pin-map change fails the
build instead of returning garbage.

### `SWO.c` (upstream `d3c1b6f`)

Two one-liners in a file that otherwise reads as untouched vendor code, so a
re-vendor silently reverts them. Both sites now carry their own `LOCAL CHANGE`
comment.

- `USB_BLOCK_SIZE` 512 → 64. 64 is the `wMaxPacketSize` of the endpoint SWO
  actually streams on (EP6 IN bulk, `USB_MXPS_BULK_FULL` in
  `../r_usb_pcdc_pvnd_descriptor.c`), and also the full-speed bulk maximum, so it
  cannot be raised.
- An `if (TransferBusy)` guard around `TraceIndexO += TransferSize` in
  `SWO_TransferComplete()`. Stops `TraceIndexO` advancing on a completion
  callback that arrives with no transfer outstanding, which would underflow
  `GetTraceCount()`.

### `cmsis_os2.h` (upstream `5029f06`)

Zero-byte at import. Now the stock ARM CMSIS-RTOS2 V2.1.3 licence and
version-history banner followed by hand-written stubs: `osThreadId_t` as
`#define … int`, `osWaitForever`, `osFlagsWaitAny`, the six `osFlagsError*`
values, `enum osStatus_t`, and prototypes for `osThreadFlagsSet` /
`osThreadFlagsWait`. Only enough of CMSIS-RTOS2 to compile `SWO.c` against
FreeRTOS. The real `cmsis_os2.h` from a newer Pack declares the whole API and
will not compile here.

## Port-local files (no counterpart in the ARM Pack)

Neither comes from ARM, so a Pack upgrade will not supply or overwrite them.
Both predate this fork.

| File | Purpose |
|---|---|
| `device.h` | Satisfies the `#include "device.h"` in `DAP_config.h` — the `#else` branch of its `#ifdef _RTE_`, which is the branch taken here. The only live line is `#include "bsp_api.h"` |
| `osObjects.h` | One line, `/* No content */`. Exists only because `SWO.c` includes it under `SWO_STREAM`. Added by `5029f06` |

Neither carries an "ARM Limited" Apache banner, which makes them look like
strays. Removing either fails the build in a way that reads as a missing CMSIS
pack.

## Unmodified — safe to overwrite from a newer Pack

`DAP.c`, `DAP.h`, `DAP_vendor.c`, `JTAG_DP.c`, `UART.c`, `Driver_Common.h`,
`Driver_USART.h`. All byte-identical to the `9ef5816` import. (`DAP.c` now
carries two explanatory comment blocks added by this documentation pass; no code
in it has changed.)

`DAP.c` being untouched is worth stating explicitly: it is the file that consumes
`CPU_CLOCK` and `IO_PORT_WRITE_CYCLES` in `MAX_SWJ_CLOCK()` and the `clock_delay`
calculation, so the entire clock-calibration fix lives in `DAP_config.h` and none
of it in `DAP.c`.

## Known limitations carried from upstream

**No response in `DAP.c` is bounded against the buffer it is written into.**
`DAP_PACKET_SIZE` appears four times in that file — two compile-time `#error`
range checks and the two bytes of the `DAP_Info` reply that *tell* the host what
the limit is. There is no runtime check anywhere.

The worst case is `DAP_SWD_TransferBlock()`: `request_count` is taken as a 16-bit
value off the wire with no clamp, and the read path stores 4 bytes per word into
`response`. A 5-byte request asking for 65535 words produces 262,144 response
bytes into a 1024-byte buffer. `DAP_SWD_Transfer()` (~2042 bytes) and
`DAP_SWD_Sequence()` (~2042 bytes) are smaller amplifiers on the same live path.

This is safe only because hosts honour the packet size `DAP_Info` reports; it is
not robust against a buggy or hostile host, and one 1024-byte USB packet is
enough to trigger it. The defect is upstream's and is deliberately left in place
here rather than being forked further; the full analysis, including what a real
fix would have to do, is in the comment block above `DAP_ExecuteCommand()` in
`DAP.c`.

**`TIMESTAMP_CLOCK` is 100 MHz against an ICLK of 96 MHz**, and `DWT->CYCCNT` is
never enabled — nothing in this firmware, in `ra_gen/`, or in the FSP BSP sets
`DEMCR.TRCENA` or `DWT_CTRL.CYCCNTENA`. So `TIMESTAMP_GET()` returns a constant
unless a debugger is attached to the *probe*, and `DAP_SWJ_Pins`' timeout has no
working exit other than the pin already matching. See the comments on
`TIMESTAMP_CLOCK` and `TIMESTAMP_GET()` in `DAP_config.h`. Not fixed here: the
value change needs a rebuild and a reflash to validate.
