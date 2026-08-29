# Troubleshooting

Failures that have actually happened on this bench, what they looked like, and —
just as usefully — what was ruled out.

## The status LED slow-blinks forever

The firmware slow-blinks whenever `b_usb_configured` is false, i.e. the host has
never sent `SET_CONFIGURATION`. Two very different causes produce that, and they
are indistinguishable from across the room.

### Cause 1: a power-only USB cable

This cost most of a session. The board was doing everything right; the cable had
no data lines. The registers that prove it, on RA4M2 USBFS:

| Register | Address | Reading | Means |
|---|---|---|---|
| `SYSCFG` | `0x40090000` | `0x0411` | `USBE=1`, `DPRPU=1` (D+ pull-up asserted), `SCKE=1` |
| `INTSTS0` | `0x40090040` | `DVSQ` = bits **[6:4]** | see below |
| `FRMNUM` | `0x4009004C` | `0` | no SOF ever received |

`DVSQ` decoding:

| `DVSQ` | State |
|---|---|
| `0b000` | Powered |
| `0b001` | Default |
| `0b010` | Address |
| `0b011` | **Configured — enumerated and live** |
| `0b100` | Suspended *from Powered* — **the host has never sent a bus reset** |
| `0b111` | Enumerated, then suspended |

`DVSQ = 0b100` with `FRMNUM` stuck at `0` is the signature: the device has
asserted its pull-up and is waiting, and nothing is answering on D+/D−.

Read `FRMNUM` **16 bits wide**. `UFRMNUM` sits immediately above it at
`0x4009004E`, so a `mem32` read straddles both registers.

Confirming test — a synthetic re-plug. Toggle `SYSCFG.DPRPU` off for ~1 s and
back on. A real host bus-resets within ~100 ms. Four seconds later still at
`0b100` means nothing is on the data lines. Swapping the cable took it straight
to `DVSQ = 0b011`, `FRMNUM = 0x059E`, `b_usb_configured = 1`, LED solid.

`scripts/flash.sh` decodes the `DVSQ` half of this automatically after every
flash and names the cable case in its output.

### Cause 2: the USB suspend/detach bug (fixed)

`USB_STATUS_SUSPEND` used to share a case body with `USB_STATUS_DETACH` in
`ra4m2/src/dap_thread_entry.c`, so a suspend cleared `b_usb_configured` — and
`USB_STATUS_RESUME` had nothing to put it back. It was a one-way door.

A suspend is not an unplug: the device keeps its address, its configuration and
its pipes, and the host resumes without re-issuing `SET_CONFIGURATION`. Hosts
suspend an idle interface within seconds, so a probe that had enumerated
perfectly would drop to the "never enumerated" slow blink and stay there for the
rest of the session — and the `if (b_usb_configured)` guards on the read and
write completion paths would silently discard every transfer that arrived
afterwards.

Fixed by splitting the two cases. If you see the LED drop to slow blink *after*
a period of working, check that split first.

### Ruled out — do not re-check these

- **UCLK / the PLL.** Verified correct. XTAL 12 MHz → PLL ×16 = 192 MHz → ICLK
  /2 = 96 MHz. PLL2 = 12 × 20 = 240 MHz, `BSP_CFG_UCK_SOURCE = PLL2`,
  `BSP_CFG_UCK_DIV = /5` → **UCLK = 48 MHz**. Measured on hardware:
  `SCKDIVCR = 0x21021221`, `SCKDIVCR2 = 0x00050000`.
- **`usb_mode.host` in `ra4m2/configuration.xml`.** A red herring —
  `ra4m2/ra_gen/dap_thread.c` generates `.usb_mode = USB_MODE_PERI`.
- **P407.** It must be USB VBUS sense, and it is. Without it the device does not
  enumerate at all.

## Which board am I actually talking to?

Two RA4M2 CMSIS-DAP boards exist here and both enumerate as `045b:201f`.

| Board | USB serial | Debugger on it | Target it debugs |
|---|---|---|---|
| A | `5196032D34385` | J-Link `821000843` | nothing wired |
| B | `59BF042D36335` | none | Silicon Labs EFR32MG24 |

**`ioreg` is not a discriminator.** Halting a board over J-Link does *not* drop it
from macOS `ioreg` — the D+ pull-up stays asserted. What works:

1. Halt one board over J-Link, then run a **fresh** `daptest transport`. It needs
   control transfers *plus* real DAP command execution. If it still passes, the
   halted board is not the probe you are talking to.
2. Read the USB serial string descriptor out of RAM at
   `g_apl_string_descriptor_serial_number` (UTF-16LE) and compare it with
   `ioreg`. Before trusting a screenful of zeros, confirm reads are live by
   watching `xTickCount` advance across a `sleep` — an all-zero read of a halted
   or unpowered part looks exactly like a firmware bug.

Only board A can be reflashed, so `scripts/flash.sh` always programs board A no
matter which board is enumerated at the time.

## Code flash refuses to erase or program

```
****** Error: Failed to erase sectors.
ERROR: Erase returned with error code -5.
```

Read `BPS` (`0x0100A1C0`) and `PBPS` (`0x0100A1E0`) — **not** the `_SEC` copies —
before concluding anything. `00000000` in either means block-protection
poisoning; see [../tools/recovery/README.md](../tools/recovery/README.md), which
also covers the two other causes that produce the same string.

A recovered board reads `BPS = PBPS = 00000000` forever. That is a healthy steady
state, not a fault.

## pyOCD does not list the probe (macOS)

> STLink, CMSIS-DAPv2 and PicoProbe probes are not supported because no libusb
> library was found

SIP strips `DYLD_*` from the environment when it executes a protected binary, so
running `pyocd` through the pyenv shim (a `#!/usr/bin/env bash` script) loses
`DYLD_FALLBACK_LIBRARY_PATH` and libusb is never found. This affects any pyOCD
invocation on macOS, not just this repo's tools.

`tools/rttpull.py` works around it by re-execing itself once with the variable
set, which survives because the interpreter is not a protected binary. For a bare
`pyocd` invocation, set `DYLD_FALLBACK_LIBRARY_PATH` and call the real
interpreter rather than the shim.

## `daptest` says the interface is already claimed

Something else has the probe open — usually a pyOCD session, including one left
behind by a crashed `rttpull.py`. Close it. `daptest` exits 2 on any fatal,
which is indistinguishable from two failed cases by exit status alone; match on
`FATAL:` on stderr if you need to tell them apart in CI.

## Reading RAM through J-Link returns all zeros

Before diagnosing a firmware fault, prove the reads are live. Read `xTickCount`
twice across a `sleep 0.5` and check it advanced by ~500. If it did, the CPU is
running and the reads are real; if it did not, you are looking at a halted or
unpowered part and every value you read is meaningless.

The same discipline applies to flash: compare a handful of scattered addresses
against the ELF (`arm-none-eabi-objdump -s`) rather than assuming a blank read
means blank flash. Erased flash reads `0xFF`; `0x00` padding means a zero-filled
image was actually programmed — which is the block-protect poisoning signature
above.
