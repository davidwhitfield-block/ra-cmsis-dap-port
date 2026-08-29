# Host tools

- [`daptest`](#daptest) — functional and performance suite for the probe itself
- [`rttpull.py`](#rttpullpy) — pull RTT and record SystemView without a J-Link
- [`recovery/`](recovery/README.md) — unbrick an RA4M2 whose code flash was
  permanently block-protected by a flat `objcopy -O binary` image at address 0
  (separate doc)

## `daptest`

Functional and performance test suite for the probe, driven straight over
CMSIS-DAP v2 bulk endpoints with libusb.

```sh
brew install libusb                  # macOS
sudo apt install libusb-1.0-0-dev    # Debian/Ubuntu
make
./daptest all
```

Exit status is the number of failed cases, so it drops into CI unchanged — with
one caveat. A fatal error also exits 2, and so do both usage errors and an
unrecognised case name, which is indistinguishable from two failed cases. The
common fatals are an unplugged probe and an interface already claimed by pyOCD.
If that matters, match on the output rather than the status alone: a fatal
prints `FATAL: ...` to stderr and never reaches the `N/M cases passed` line on
stdout.

It is written in C rather than Python on purpose. At a 1024-byte
`DAP_PACKET_SIZE` a block carries at most 255 words (254 on writes), and ADIv5
only guarantees TAR auto-increment inside a 1 KiB page, so a TAR write precedes
every single block: 313 KiB/s is ~630 DAP commands, and so ~630 USB round trips,
per second in each direction. A Python harness spends more time in its own event
loop than the probe spends on the wire, and ends up measuring itself.

### Cases

| Case | What it asserts |
|---|---|
| `transport` | DAP round-trip rate with no SWD work — separates USB/firmware overhead from wire time |
| `coherence` | A constant register is stable over 500 reads, and a 2 KiB block read straddling a TAR page boundary matches word-by-word reads |
| `read` | 10 MiB pulled at ≥ 280 KiB/s with zero errors; the traffic is ~640 passes over a single 16 KiB window, every pass compared byte-for-byte against the first |
| `write` | 10 MiB written and verified, ≥ 280 KiB/s — non-destructive (halt, save, write, verify, restore, verify restore, resume) |
| `resetloop` | 20 nRESET pulses; SWD reconnects and CPUID matches every time, and nRESET reads back low while asserted |
| `srst` | 10 SWD-only reset+halt cycles via vector catch, with no nRESET wire |
| `halt` | Reset and hold at the reset vector; recovery for a target whose firmware closes the debug port |
| `depth` | Every depth up to `DAP_PACKET_COUNT` is byte-clean, and exceeding it **corrupts** |
| `clock` | Below saturation the delivered rate stays inside 0.85–1.30× the request; at and above saturation it only has to hold the ceiling (`--min-read`, default 280 KiB/s), which is how the 10 and 30 MHz points pass at the ~3.7 MHz wall |
| `fault` | A bus fault comes back as a clean FAULT ack and the link survives an ABORT. Up to five candidate addresses are probed in turn until one faults; on a target where none of them does, the case passes without exercising the ABORT/recovery path and says so in its note — read the note, do not just count the PASS |
| `churn` | 100 connect/disconnect cycles with a stable DPIDR |
| `soak` | 64 MiB verified, judged by peak backlog against an RTT producer (below) |

### What it leaves behind

`all` runs 11 of the 12 cases — `halt` is excluded, because leaving the target
stopped is the whole point of it. That is why a clean run of `all` reports 11/11
against a table of 12.

A normal run ends with `cm_release_debug()`: C_DEBUGEN cleared, target
free-running. Three exceptions:

- `halt` leaves the core stopped at the reset vector, and does so even when the
  case *fails* — the flag is set before the pass is judged. It is also sticky
  for the whole invocation, so `daptest halt read` leaves the target stopped
  too.
- Any of `coherence`, `read`, `depth`, `clock` or `soak` leaves the core stopped
  if it had to climb the recovery ladder to reach the read window. That only
  happens on a run where the window was unreachable, so the same command can end
  with the target running one day and halted the next.
- `write` halts the core for the length of the pass — about 33 s for the default
  10 MiB at 313 KiB/s — saves the 16 KiB scratch window, and restores and
  re-verifies it before resuming. A restore that does not verify is reported
  ("restore FAILED") and not retried, so the window may be left holding the test
  pattern. Separately, there is no signal handler: Ctrl-C or a crash during
  `write` skips the restore entirely and leaves up to 16 KiB of pattern in the
  target's RAM.

`srst` is the only case that resumes and releases debug on its own.

### Two things worth knowing before you change it

**Every read case verifies its bytes.** The predecessor to this tool reported
249 KiB/s while silently returning corrupt data: it pipelined more DAP commands
than `DAP_PACKET_COUNT`, overran the device's `USB_Request[]` ring, and stopped
matching responses to the commands that produced them. Full throughput, wrong
bytes, no error anywhere. The `depth` case exists to keep that honest — it
proves both that staying within the limit is clean *and* that exceeding it
corrupts. Never trust a throughput number that did not check its bytes.

The reference is the first pass over a 16 KiB window (`WINDOW_BYTES`), not a
known image. `read`, `depth` and `soak` all cycle that one window and compare
every later pass against the first pass of the same run, because the window is
target flash and the host has no independent copy of it. That catches desync,
corruption and dropped bytes — the failure class that mattered — but not a
target that returns the same wrong value every time. `write` is the exception:
it compares the readback against the deterministic pattern it wrote, so that one
is against a known image, but it reads back once at the end, so it proves the
last pass landed rather than all ~645 of them.

Block geometry also leaves a gap at each page boundary. At `DAP_PACKET_SIZE`
1024 a read block is 255 words and a write block 254 (a write block carries a
5-byte header, a read block's is 4), so 1020 of every 1024 bytes are read and
1016 are written; the last 4 bytes of each page are never read and the last 8
never written. The startup `geom` line prints words/block, blocks/page and bytes
covered per page — read-direction geometry, for whatever packet size the probe
negotiates.

**`soak` is judged by backlog, not by a floor on every window.** Asserting that
no 100 ms window over three minutes ever dips is an assertion about the host's
USB scheduler, not about the probe; it fails occasionally for reasons nothing in
this repo controls. What actually decides whether a SystemView capture drops
data is whether a stall outlasts the target's RTT buffer, so the case runs the
measured drain rate against a constant producer (`--produce`, default
160 KiB/s) and asserts the peak backlog fits in `--buffer` (default 64 KiB).

### Bench topology

Two RA4M2 CMSIS-DAP boards exist here, and they are easy to confuse.

| Board | USB serial | Debugger on it | Target it debugs |
|---|---|---|---|
| A | `5196032D34385` | J-Link `821000843` | nothing wired |
| B | `59BF042D36335` | none | Silicon Labs EFR32MG24 |

Only board A can be reflashed. Every measurement in this file was taken through
board B, because it is the one with a target.

`ioreg` cannot tell them apart by halting: halting the core over J-Link does
**not** drop the device from `ioreg`, because the D+ pull-up stays asserted. What
does distinguish them is a *fresh* `daptest transport` while one board is
halted — if it still passes, the halted board is not the probe you are talking
to. The USB serial string is the other discriminator; the firmware fills it from
the MCU unique ID (`R_BSP_UniqueIdGet`).

### One probe at a time

`daptest` opens the first device matching `045b:201f` and stops looking. There is
no serial filter and no `--probe`. Both probe boards run this firmware and so
enumerate with the same VID:PID, which means with two attached, which one gets
tested is decided by libusb enumeration order. The header line names the
*target* (DPIDR/CPUID) but never the probe, so the output will not tell you
which board you measured. Unplug the one you are not testing.

`rttpull.py` does not have this limitation: `--probe <serial>` selects a board by
USB serial, and `log` and `sysview` print the serial of whichever probe they
opened. Without `--probe`, pyOCD also takes the first probe it finds.

### Options

```
--clock HZ      SWD clock request (default 10000000)
--rdbase HEX    static read window base (default 0x08000000). Without this flag an
                unreadable default falls back to 0x0C000000, 0x00000000, 0x0FE08000,
                0x1FF00000 in that order. Passing it turns the fallback off: an
                explicit base is taken as deliberate, and coherence/read/depth/clock/
                soak fail with "read window ... unreachable even after reset+halt"
--wrbase HEX    scratch RAM base for the write case (default: autodetect)
--mb N          MiB per read/write bandwidth case (default 10)
--soak-mb N     MiB for the soak case (default 64)
--produce K     assumed RTT producer rate, KiB/s, for the soak (default 160)
--buffer K      assumed RTT buffer size, KiB, for the soak (default 64)
--depth N       pipeline depth, clamped to DAP_PACKET_COUNT (default 8)
--iters N       iteration scale (default 20): resetloop runs N nRESET pulses,
                srst runs min(N,10) cycles, churn runs N*5 cycles -- so the
                default gives 20 / 10 / 100
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
port access at 2–5 PCLKB.

`clock` does not measure the wire. It infers the delivered rate from bytes/s at
those same 46 clocks per word, so USB round-trip time and the once-per-block TAR
write — which costs wall time and contributes no counted bytes — are both
charged to SWD. Because 46 clocks/word is the ADIv5 floor, the estimate can only
read low, never high: every point above is under 1.0 (1 MHz → 0.99, 4 → 3.69 =
0.92). The 0.85 bound is what absorbs that; the 1.30 bound is the separate guard
against a probe clocking faster than it was asked to. The check is a plain OR,
not a regime switch — any request that reaches `--min-read` KiB/s passes
regardless of ratio.

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
pyocd pack install <part>                                   # once, for a real target map
./rttpull.py --target <part> log                            # channel 0 to stdout
./rttpull.py --target <part> sysview --output trace.SVDat   # open this in SystemView
./rttpull.py --rtt-addr 0x20000000 --rtt-size 0x40000 log   # or bound the search by hand
./rttpull.py --probe 59BF042D36335 --target <part> log --seconds 10
```

where `<part>` is the target's pyOCD target type — for the EFR32MG24 on this
bench, the `efr32mg24…` name that `pyocd list --targets` prints once the pack is
installed.

`--target` defaults to `cortex_m`, which is not a device: pyOCD maps it to the
generic `CoreSightTarget`, which builds only the architectural Cortex-M address
map. Its first default RAM region is `Code` at `0x00000000` for 512 MiB, ahead of
`SRAM` at `0x20000000`, and `get_default_region_of_type(RAM)` returns the
lowest-addressed default match. So an unbounded search covers the code alias,
never reaches the SRAM where a control block actually lives, and takes roughly
33,000 mostly-faulting 16 KiB transfers to fail; `bench` and `selftest` place
their scratch ring in the middle of that same region, `0x10000000`, which is not
memory on most parts. Name a real pyOCD target, or bound the search with
`--rtt-addr`/`--rtt-size` and place scratch with `--scratch`. (Read from the
pyOCD 0.44.0 source, not reproduced against hardware.)

pyOCD 0.44.0 ships no built-in EFR32 target, so `--target efr32…` only resolves
after `pyocd pack install`; without it pyOCD fails with "Target type … not
recognized".

| Subcommand | What it does |
|---|---|
| `log` | Drains a text channel to stdout or a file |
| `sysview` | Sends START, records the channel to a `.SVDat`, sends STOP |
| `bench` | Measures how fast this host can drain a channel |
| `selftest` | Proves the reader is byte-exact, over the real probe |

`selftest` and `bench` need no RTT in the target's firmware: they plant a
control block in scratch RAM, exercise the real reader against it over the real
probe, and restore the memory afterwards. Both write to the target, and they
differ in ways worth knowing before pointing either at a live board. When
`bench` plants, it halts the core first: a planted ring lives in RAM the running
firmware owns, and a running core rewrites the pattern underneath the read.
`selftest` never halts. It plants ~2.1 KiB (two `--ring` buffers, default 1024
each, plus a 24-byte header, two 24-byte descriptors and the name blob) into
live RAM at `--scratch` — default the middle of the RAM region — while the
firmware runs, so pass `--scratch` at an address you know is dead. `bench
--rtt-addr <cb>` with no `--scratch` does not plant at all: it drives the
target's own ring, overwriting its contents and forcing WrOff/RdOff on a running
core, which destroys a capture in flight. Both restore in a `finally`, so a
SIGKILL or a probe that dies mid-run leaves the target's RAM as the tool left
it. `selftest` pushes a known sequence through a small ring so the wrapped read
path is hit repeatedly, and fails if it never wraps — a test that silently
stopped covering wraparound would prove much less than it claims.

### Options

Global options are parsed by the top-level parser, so they must come *before*
the subcommand: `rttpull.py --probe ID log`, never `rttpull.py log --probe ID`.

```
global (before the subcommand)
  --probe ID        probe unique ID (default: the only one attached)
  --probe-type T    cmsis-dap, jlink, stlink, ...
  --target NAME     pyOCD target type (default: cortex_m)
  --frequency HZ    SWD clock in Hz (default 10000000)
  --rtt-addr HEX    control block address, or the base of the search range
  --rtt-size HEX    search range in bytes; 0 means --rtt-addr is exact
  --poll S          seconds to sleep when a channel is empty (default 0.005)
log
  --channel N|NAME  up channel index or name (default 0)
  --output FILE     write raw bytes here instead of stdout
  --seconds S       stop after this long
sysview
  --channel N|NAME  channel index or name (default 'SysView')
  --output FILE     output file (default trace.SVDat)
  --seconds S       stop after this long
  --no-start        record without sending the START command
bench
  --channel N|NAME  up channel index or name
  --scratch HEX     plant a ring here instead of using the target's own RTT
  --ring N          planted ring size (default 4096)
  --mb N            MiB to move (default 4)
  --min-kib K       pass/fail floor in KiB/s (default 200)
  --produce K       assumed producer rate in KiB/s, for the warning (default 160)
selftest
  --scratch HEX     scratch RAM address (default: middle of the RAM region)
  --ring N          ring size (default 1024)
  --bytes N         bytes to push through it (default 16384)
```

Exit status is 0 on success and 2 from any subcommand on an RTT error; `bench`
additionally exits 1 when it finishes below `--min-kib` or sees a mismatch.
`bench` and `selftest` therefore drop into CI the same way `daptest` does.

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
a protected binary. This affects any pyOCD invocation on macOS, not just
`rttpull.py`; see [../docs/TROUBLESHOOTING.md](../docs/TROUBLESHOOTING.md).
