# RA4M2 block-protect recovery

Unbricks an RA4M2 whose code flash was permanently block-protected by flashing
a flat `objcopy -O binary` image at address 0.

```sh
./recover.sh --sn 821000843 --bin ../../ra4m2/Debug/CMSIS_DAP_RA4M2.bin
```

All register references are to the RA4M2 User's Manual **R01UH0892EJ0150 rev
1.50**, saved at `Supporting Documents/parts/Jlink OB/ra4m2-users-manual-hardware.pdf`.

## Symptom

Any erase or program of code flash fails:

```
****** Error: Failed to erase sectors.
ERROR: Erase returned with error code -5.
```

with `FSTATR` (`0x407FE080`) = `0x0080C000` and `FASTAT` (`0x407FE010`) = `0x10`.
Reading the option-setting memory shows:

| Register | Address | Poisoned value |
|---|---|---|
| BPS | `0x0100A1C0` | `00000000` |
| PBPS | `0x0100A1E0` | `00000000` |

A second tell-tale: **code flash above the image reads `0x00`, not `0xFF`.**
Erased flash is always `0xFF`, so `0x00` padding proves a zero-filled image was
actually programmed.

## Cause

The FSP ELF spans `0x0` to `0x0100_A2CC`. A plain `objcopy -O binary` therefore
emits a **16 MB** file and fills every gap with `0x00` — objcopy's default, not
`0xFF`. That file holds `0x00000000` at file offsets `0x100A1C0` (**BPS**) and
`0x100A1E0` (**PBPS**), so `loadbin <it> 0x0` programs the block-protect and
*permanent* block-protect registers to zero.

Per Table 6.2, applied PBPS=0 with applied BPS=0 means "programming and erasure
to the corresponding block is invalid **permanently**".

The `.srec` and `.elf` are both safe — neither format carries gap padding, so
neither can reach `0x0100_A1C0`. **Only the flat `.bin` is dangerous.** A
correct code-flash-only `.bin` is ~54 KB; `recover.sh` refuses anything over
512 KB for exactly this reason.

## Why recovery is possible

The lock applies to the **applied** register pair, and one register chooses
which pair that is.

§6.2.5: *"The applied setting value is determined by the setting value of the
corresponding bit in **BPS_SEL** register. The security attribution register is
same BPS_SEL register between the block protection and permanent block
protection."*

| Register | Address | Value |
|---|---|---|
| BPS / PBPS (non-secure) | `0x0100A1C0` / `0x0100A1E0` | `00000000` poisoned |
| **BPS_SEC / PBPS_SEC (secure)** | `0x0100A240` / `0x0100A260` | **`FFFFFFFF` pristine** |
| **BPS_SEL** | `0x0100A2C0` | `FFFFFFFF` → selects the poisoned pair |

Setting a BPS_SEL bit to 0 makes the *secure* pair apply for that block. Those
are blank, so Table 6.2 row 1 applies: programming and erasure valid.

Two separate places in the manual enumerate the permanent-lock restrictions and
**exclude BPS_SEL from both**:

- **Table 44.18** lists every Configuration Set target. BPS, PBPS, BPS_SEC,
  PBPS_SEC and SAS each carry a "cannot be restored to 1" note. BPS_SEL is
  listed as plain *"Writable / Writable"*, no note, timing *"At a reset"*.
- **Table 44.22** gives the write/clear freeze — and its scope is `BPS[n]` and
  `PBPS[n]` only.

§6.3.4 then describes programming BPS_SEL to 0 as normal operation.

The poisoned BPS/PBPS stay `00000000` forever. They are simply never consulted
again.

### What does *not* work

- **FBPROT0 / FBPROT1 cancellation.** §44.11.1.3: with permanent protection the
  command locks *"regardless of the FBPROT0 and FBPROT1 register settings"*.
- **Boot-mode Initialize.** §46.3.1: it does not execute while any permanently
  locked block exists. Also moot on the X2C board — MD (P201) has a 10K pull-up
  and no switch, jumper or testpoint.
- **RMA.** An RMA_REQ transition explicitly *preserves* permanently locked blocks.
- **DLM transitions.** CM→SSD does not erase code flash.

## The sequence

`recover.sh` drives three standalone `.jlink` files, each runnable by hand.

**`01-preflight.jlink`** — read-only. Aborts if `BPS_SEC`/`PBPS_SEC` are not
`FFFFFFFF` (secure pair also poisoned → replace the IC), or if `DLMMON`
(`0x400E002C`) is not CM(1)/SSD(2), since DBG2 is required to program the
secure region (Table 6.1, Table 46.8, Figure 44.33).

**`02-unlock.jlink`** — the fix:

1. `FMEPROT` (`0x407FE044`) ← `0xD900` — key `0xD9`, CEPROT=0. Its reset value
   protects `FENTRYR.FENTRYC`; without this you cannot enter P/E mode.
2. `FWEPROR` (`0x4001E416`) ← `0x01` — FLWE=`01b`, the only value that permits
   Configuration Set. **Base is SYSC `0x4001_E000`, not FACI `0x407F_E000`.**
   Writing `0x407FE416` is a silent no-op that surfaces as FLWEERR
   (`FSTATR` = `0x00008040`).
3. `FENTRYR` (`0x407FE084`) ← `0xAA01` — key `0xAA`, FENTRYC=1, code-flash P/E
   mode. `0xAA80` is *data* flash; `0xAA81` deliberately raises ILGLERR.
4. Check `FSTATR` = `0x00008000` before issuing anything.
5. Status Clear (`0x50`), then `FSADDR` (`0x407FE030`) ← `0x0100A2C0`.
6. Configuration Set — Table 44.14, 11 write accesses: `0x40`, `N=0x08`,
   `WD1..WD8`, `0xD0`. **The command, N and `0xD0` cycles must be BYTE writes**;
   only `WD1..WD8` are 16-bit. A 16-bit first cycle gives ILGCOMERR+ILGLERR
   (`0x0080C000`) — Table 44.21, *"An undefined size is specified in the first
   cycle of the command. (not byte-write)"*.
   `WD1`=`0xC000`, `WD2`=`0xFFFF` → BPS_SEL = `0xFFFFC000`; `WD3..WD8`=`0xFFFF`
   pad the rest of the 16-byte unit (reserved, must be 1).
7. `FENTRYR` ← `0xAA00` to leave P/E mode.

**`03-verify.jlink`** — **reset first** (mandatory: BPS_SEL applies at reset),
then a *ranged* erase of block 0.

### BPS_SEL mask by flash size

Clear one bit per block, leave reserved bits 1 (§6.2.4, Figure 6.2/6.3):

| Part | Code flash | Blocks | BPS_SEL |
|---|---|---|---|
| R7FA4M2AB | 256 KB | 0–13 | `0xFFFFC000` |
| — | 384 KB | 0–17 | `0xFFFC0000` |
| R7FA4M2AD | 512 KB | 0–21 | `0xFFC00000` |

## Traps

**Never run a bare J-Link `erase` (Erase Chip) on a recovered part.** It also
erases the config area, blanking BPS_SEL back to `FFFFFFFF` and instantly
re-locking the board. BPS/PBPS cannot be un-zeroed by that erase (Table 44.22),
so you land straight back in the poisoned state. Use a ranged erase, or just
`loadbin`, which erases only the sectors it needs. If it happens, re-run
`recover.sh` — it is idempotent.

**Never point a programmer at `0x0100_A1xx`** on an RA part unless you intend
the result.

**When an RA code-flash erase is rejected, read BPS (`0x0100_A1C0`) and PBPS
(`0x0100_A1E0`) first — not the `_SEC` copies** at `0x0100_A240` / `0x0100_A260`.
Reading the wrong pair shows `FFFFFFFF` and sends you down a dead end.

## Verified

Recovered a part with `BPS = PBPS = 00000000` that had previously failed every
erase. After the redirect, block 0 erased, a full reflash reported
`1 range affected (57344 bytes)` … `O.K.` with verify passing, and the board
re-enumerated as `RA4M2 CMSIS_DAP Probe` (VID `0x045B`, PID `0x201F`). The
re-lock trap was then reproduced deliberately with a chip erase and recovered
again from scratch.
