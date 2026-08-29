# Host tools

- [`daptest`](#daptest) — functional and performance suite for the probe itself
- [`rttpull.py`](#rttpullpy) — pull RTT and record SystemView without a J-Link

## `daptest`

Functional and performance test suite for the probe, driven straight over
CMSIS-DAP v2 bulk endpoints with libusb.

```sh
brew install libusb                  # macOS
sudo apt install libusb-1.0-0-dev    # Debian/Ubuntu
make
./daptest all
```

Exit status is the number of failed cases, so it drops into CI unchanged.

It is written in C rather than Python on purpose. At a 1024-byte
`DAP_PACKET_SIZE`, 313 KiB/s is ~315 USB round trips per second in each
direction; a Python harness spends more time in its own event loop than the
probe spends on the wire, and ends up measuring itself.

### Cases

| Case | What it asserts |
|---|---|
| `transport` | DAP round-trip rate with no SWD work — separates USB/firmware overhead from wire time |
| `coherence` | A constant register is stable over 500 reads, and a 2 KiB block read straddling a TAR page boundary matches word-by-word reads |
| `read` | 10 MiB pulled and verified byte-for-byte, ≥ 280 KiB/s, zero errors |
| `write` | 10 MiB written and verified, ≥ 280 KiB/s — non-destructive (halt, save, write, verify, restore, verify restore, resume) |
| `resetloop` | 20 nRESET pulses; SWD reconnects and CPUID matches every time, and nRESET reads back low while asserted |
| `srst` | 10 SWD-only reset+halt cycles via vector catch, with no nRESET wire |
| `halt` | Reset and hold at the reset vector; recovery for a target whose firmware closes the debug port |
| `depth` | Every depth up to `DAP_PACKET_COUNT` is byte-clean, and exceeding it **corrupts** |
| `clock` | The delivered SWD rate tracks the requested rate to within 15% below saturation |
| `fault` | A bus fault comes back as a clean FAULT ack and the link survives an ABORT |
| `churn` | 100 connect/disconnect cycles with a stable DPIDR |
| `soak` | 64 MiB verified, judged by peak backlog against an RTT producer (below) |

### Two things worth knowing before you change it

**Every read case verifies its bytes.** The predecessor to this tool reported
249 KiB/s while silently returning corrupt data: it pipelined more DAP commands
than `DAP_PACKET_COUNT`, overran the device's `USB_Request[]` ring, and stopped
matching responses to the commands that produced them. Full throughput, wrong
bytes, no error anywhere. The `depth` case exists to keep that honest — it
proves both that staying within the limit is clean *and* that exceeding it
corrupts. Never trust a throughput number that did not check its bytes.

**`soak` is judged by backlog, not by a floor on every window.** Asserting that
no 100 ms window over three minutes ever dips is an assertion about the host's
USB scheduler, not about the probe; it fails occasionally for reasons nothing in
this repo controls. What actually decides whether a SystemView capture drops
data is whether a stall outlasts the target's RTT buffer, so the case runs the
measured drain rate against a constant producer (`--produce`, default
160 KiB/s — the rate of a riker payment soak) and asserts the peak backlog fits
in `--buffer` (default 64 KiB).

### Options

```
--clock HZ      SWD clock request (default 10000000)
--rdbase HEX    static read window base (default 0x08000000; autodetected if unreadable)
--wrbase HEX    scratch RAM base for the write case (default: autodetect)
--mb N          MiB per read/write bandwidth case (default 10)
--soak-mb N     MiB for the soak case (default 64)
--produce K     assumed RTT producer rate, KiB/s, for the soak (default 160)
--buffer K      assumed RTT buffer size, KiB, for the soak (default 64)
--depth N       pipeline depth, clamped to DAP_PACKET_COUNT (default 8)
--iters N       iterations for resetloop/srst/churn (default 20)
--min-read K    read bandwidth floor, KiB/s (default 280)
--min-write K   write bandwidth floor, KiB/s (default 280)
-v              verbose: DP/AP state at startup and per-error detail
```

### TrustZone targets

On an ARMv8-M part the AP's `CSW[30]` (HNONSEC) decides whether accesses are
secure. Its value after a target reset is *not* the value running firmware
leaves behind, so inheriting whatever happens to be in CSW means the first read
after a reset asks for non-secure access to a now-secure region and FAULTs.
This was not theoretical: it broke every block read in this suite until it was
handled, while single-word reads of the PPB kept working and made it look like a
transport bug.

`daptest` therefore sets the field deliberately and picks the attribute by
trying secure first and falling back to non-secure. If the configured read
window is unreachable it probes known aliases and, failing that, catches the
target at its reset vector before its firmware can close the port.

### Reference numbers

Measured against a Silicon Labs EFR32MG24 target (Cortex-M33, `DEVINFO.PART`
family 24 / device 1010) at `DAP_PACKET_SIZE` 1024, `DAP_PACKET_COUNT` 8. Three
consecutive `daptest all` runs were 11/11:

```
transport   5330 DAP round trips/s (0.19 ms each)
read        312.8 KiB/s, 0 errors, worst 100 ms 309 KiB/s
write       313.5 KiB/s, 0 errors
clock       1 MHz -> 0.99, 2 -> 1.98, 4 -> 3.69, saturates at 3.68 MHz
soak        64 MiB, 312.9 KiB/s, peak backlog 0.0 KiB vs a 160 KiB/s producer
```

The SWD ceiling is ~3.7 MHz, or ~313 KiB/s: 46 SWD clocks per 32-bit word is the
ADIv5 floor (8 request + 1 turnaround + 3 ACK + 32 data + 1 parity + 1
turnaround), and the RA4M2's I/O ports sit on PLBIU at PCLKB, which prices a
port access at 2-5 PCLKB.

## `rttpull.py`

Pulls SEGGER RTT off a target through any pyOCD-supported probe, and records
SystemView sessions.

The point is that it does not need a J-Link. SEGGER's RTT Logger and the
SystemView application both speak to a J-Link and nothing else, so a board wired
to a CMSIS-DAP probe cannot be traced at all. RTT is a pure memory protocol — a
control block in target RAM and a pair of ring buffers — so any probe that can
read memory while the core runs can drain it.

```sh
pip install pyocd
./rttpull.py log                                   # channel 0 to stdout
./rttpull.py sysview --output trace.SVDat          # open this in SystemView
./rttpull.py --probe 59BF042D36335 log --seconds 10
```

| Subcommand | What it does |
|---|---|
| `log` | Drains a text channel to stdout or a file |
| `sysview` | Sends START, records the channel to a `.SVDat`, sends STOP |
| `bench` | Measures how fast this host can drain a channel |
| `selftest` | Proves the reader is byte-exact, over the real probe |

`selftest` and `bench` need no RTT in the target's firmware: they plant a
control block in scratch RAM, exercise the real reader against it over the real
probe, and restore the memory afterwards. `selftest` pushes a known sequence
through a small ring so the wrapped read path is hit repeatedly, and fails if it
never wraps — a test that silently stopped covering wraparound would prove much
less than it claims.

### Why it reads words, not bytes

pyOCD's `read_memory_block8` reads words underneath and then expands them into a
Python list with one `int` object per byte, which the caller packs back into
`bytes`. At trace rates that is two O(n) interpreter passes over every byte
pulled off the target, and it dominates. Going through `array` instead took the
drain rate from 134 to 241 KiB/s — against `daptest`'s 311.6 KiB/s raw C
measured on the same target at the same moment, so this is at 77% of what the
transport itself can do, and comfortably ahead of the ~160 KiB/s a SystemView
capture produces.

### On macOS

SIP strips `DYLD_*` from the environment when it executes a protected binary, so
running `pyocd` through the pyenv shim (a `#!/usr/bin/env bash` script) loses
`DYLD_FALLBACK_LIBRARY_PATH` and libusb is never found — pyOCD then reports
"STLink, CMSIS-DAPv2 and PicoProbe probes are not supported because no libusb
library was found" and simply does not list the probe. `rttpull.py` re-execs
itself once with the variable set, which survives because the interpreter is not
a protected binary.
