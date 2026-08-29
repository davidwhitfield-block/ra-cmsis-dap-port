#!/usr/bin/env python3
"""Pull SEGGER RTT off a target through any pyOCD-supported probe.

The point of this tool is that it does not need a J-Link. SEGGER's own RTT
Logger and SystemView both speak to a J-Link and nothing else, which means a
board wired to a CMSIS-DAP probe cannot be traced at all. RTT is a pure
memory protocol, though - a control block in target RAM and a pair of ring
buffers - so any debug probe that can read memory while the core runs can
drain it. pyOCD can, so this works over CMSIS-DAP, ST-Link, or a J-Link
equally.

Subcommands:

    log        drain a text channel to stdout or a file
    sysview    record a SystemView session to a .SVDat file
    bench      measure how fast this host can drain a channel
    selftest   prove the reader is byte-exact, over the real probe, with no
               target firmware support required

Run ``rttpull.py <subcommand> --help`` for the options of each.
"""

from __future__ import annotations

import argparse
import array
import os
import struct
import sys
import time

# pyOCD reaches libusb through ctypes, and dyld only consults
# DYLD_FALLBACK_LIBRARY_PATH as it was at process start. Homebrew installs
# libusb outside the default search path, so re-exec once with the variable
# set rather than failing with an opaque "no backend available".
_HOMEBREW_LIB = '/opt/homebrew/lib'
if (sys.platform == 'darwin' and not os.environ.get('RTTPULL_REEXEC')
        and _HOMEBREW_LIB not in os.environ.get('DYLD_FALLBACK_LIBRARY_PATH', '')
        and os.path.isdir(_HOMEBREW_LIB)):
    os.environ['RTTPULL_REEXEC'] = '1'
    os.environ['DYLD_FALLBACK_LIBRARY_PATH'] = ':'.join(
        p for p in (os.environ.get('DYLD_FALLBACK_LIBRARY_PATH'), _HOMEBREW_LIB) if p)
    print(f"rttpull: re-exec with DYLD_FALLBACK_LIBRARY_PATH={_HOMEBREW_LIB}", file=sys.stderr)
    os.execv(sys.executable, [sys.executable] + sys.argv)

from pyocd.core.helpers import ConnectHelper          # noqa: E402
from pyocd.core.memory_map import MemoryType          # noqa: E402


RTT_ID = b'SEGGER RTT'

# SEGGER_RTT_CB: char acID[16]; int MaxNumUpBuffers; int MaxNumDownBuffers.
CB_HEADER_SIZE = 24
# SEGGER_RTT_BUFFER_UP/DOWN: sName, pBuffer, SizeOfBuffer, WrOff, RdOff, Flags.
RING_DESC_SIZE = 24
WROFF_OFFSET = 12
RDOFF_OFFSET = 16

# SEGGER SystemView host-to-target command bytes (SEGGER_SYSVIEW_ConfDefaults.h).
SYSVIEW_CMD_START = 1
SYSVIEW_CMD_STOP = 2

# SystemView's own channel by convention; also the name it registers.
SYSVIEW_CHANNEL_NAME = 'SysView'

# 'I' is 4 bytes everywhere that matters, but the check costs nothing and the
# failure it prevents - silently misassembled trace data - is expensive.
_U32_TYPECODE = 'I' if array.array('I').itemsize == 4 else 'L'


class RTTError(Exception):
    """Anything that makes the control block or a ring buffer unusable."""


class UpChannel:
    """A target-to-host ring buffer.

    Only ``RdOff`` is ever written back, which is the one field the target
    treats as owned by the host. Everything else is read-only here, so a
    running target is never disturbed by being traced.
    """

    def __init__(self, target, desc_addr, index):
        self.target = target
        self.desc_addr = desc_addr
        self.index = index
        name_ptr, self.buffer_addr, self.size, _wr, _rd, self.flags = struct.unpack(
            '<6I', bytes(target.read_memory_block8(desc_addr, RING_DESC_SIZE)))
        self.name = _read_cstring(target, name_ptr)
        # Counters for the caller's report. near_full is the honest version of
        # "did we drop data": RTT in skip mode discards silently at the target,
        # so the host can never see a gap directly - it can only see that it
        # let the ring get close enough to full for that to have happened.
        self.total_bytes = 0
        self.high_water = 0
        self.near_full = 0

    def __repr__(self):
        return (f"<up {self.index} {self.name!r} buffer=0x{self.buffer_addr:08X} "
                f"size={self.size}>")

    @property
    def valid(self):
        return self.size > 0 and self.buffer_addr != 0

    def _offsets(self):
        wr, rd = self.target.read_memory_block32(self.desc_addr + WROFF_OFFSET, 2)
        if wr >= self.size or rd >= self.size:
            raise RTTError(
                f"up channel {self.index} offsets out of range "
                f"(WrOff={wr}, RdOff={rd}, size={self.size})")
        return wr, rd

    def read(self):
        """Drains everything currently in the ring.

        :returns: bytes -- the data read, empty if the ring was empty.
        """
        if not self.valid:
            return b''

        wr, rd = self._offsets()
        if wr == rd:
            return b''

        if wr > rd:
            data = read_bytes(self.target, self.buffer_addr + rd, wr - rd)
        else:
            # Wrapped: tail of the ring, then the head.
            data = read_bytes(self.target, self.buffer_addr + rd, self.size - rd)
            data += read_bytes(self.target, self.buffer_addr, wr)

        # Publishing RdOff last is what frees the space, so it has to happen
        # after the bytes are safely in hand.
        self.target.write32(self.desc_addr + RDOFF_OFFSET, wr)

        used = len(data)
        self.total_bytes += used
        self.high_water = max(self.high_water, used)
        if used >= (self.size - 1) * 0.9:
            self.near_full += 1
        return data


class DownChannel:
    """A host-to-target ring buffer. Used to send SystemView commands."""

    def __init__(self, target, desc_addr, index):
        self.target = target
        self.desc_addr = desc_addr
        self.index = index
        name_ptr, self.buffer_addr, self.size, _wr, _rd, self.flags = struct.unpack(
            '<6I', bytes(target.read_memory_block8(desc_addr, RING_DESC_SIZE)))
        self.name = _read_cstring(target, name_ptr)

    def __repr__(self):
        return (f"<down {self.index} {self.name!r} buffer=0x{self.buffer_addr:08X} "
                f"size={self.size}>")

    @property
    def valid(self):
        return self.size > 0 and self.buffer_addr != 0

    def write(self, data):
        """Writes as much of ``data`` as fits.

        :returns: int -- bytes accepted by the ring.
        """
        if not self.valid:
            return 0

        wr, rd = self.target.read_memory_block32(self.desc_addr + WROFF_OFFSET, 2)
        if wr >= self.size or rd >= self.size:
            raise RTTError(f"down channel {self.index} offsets out of range")

        written = 0
        data = bytes(data)
        if wr >= rd:
            # One slot is always left empty so that full and empty stay
            # distinguishable, hence the -1 when RdOff is at the origin.
            space = self.size - wr - (1 if rd == 0 else 0)
            chunk = data[:space]
            if chunk:
                self.target.write_memory_block8(self.buffer_addr + wr, chunk)
                written += len(chunk)
                wr = (wr + len(chunk)) % self.size
                data = data[len(chunk):]

        space = max(rd - wr - 1, 0)
        chunk = data[:space]
        if chunk:
            self.target.write_memory_block8(self.buffer_addr + wr, chunk)
            written += len(chunk)
            wr += len(chunk)

        self.target.write32(self.desc_addr + WROFF_OFFSET, wr)
        return written


def read_bytes(target, addr, size):
    """Reads ``size`` bytes, using word transfers wherever it can.

    pyOCD's own ``read_memory_block8`` reads words underneath and then expands
    them into a Python list with one int object per byte, which the caller then
    has to pack back into bytes. At trace rates that is two O(n) interpreter
    passes over every byte pulled off the target, and it shows up directly in
    the drain rate. Going through ``array`` instead keeps the aligned middle -
    which is nearly all of it - in one buffer conversion.
    """
    if size <= 0:
        return b''

    head = min((-addr) & 3, size)
    out = bytearray()
    if head:
        out += bytes(target.read_memory_block8(addr, head))

    words = (size - head) // 4
    if words:
        buf = array.array(_U32_TYPECODE, target.read_memory_block32(addr + head, words))
        if sys.byteorder != 'little':
            buf.byteswap()
        out += buf.tobytes()

    tail = size - head - words * 4
    if tail:
        out += bytes(target.read_memory_block8(addr + head + words * 4, tail))

    return bytes(out)


def _read_cstring(target, addr, limit=64):
    """Reads a NUL-terminated string from the target, tolerating junk."""
    if not addr:
        return None
    raw = bytes(target.read_memory_block8(addr, limit))
    raw = raw.split(b'\0', 1)[0]
    return raw.decode('utf-8', 'backslashreplace')


def find_control_block(target, addr=None, size=None, block_id=RTT_ID):
    """Locates the RTT control block in target memory.

    pyOCD's own search reads 32 bytes per transfer, which is thousands of USB
    round trips across a real RAM region. This reads in large blocks instead,
    which turns a multi-second scan into well under a second.

    :param addr: base of the search range, or the exact address if size is 0.
    :param size: bytes to search; ``None`` means the whole default RAM region.
    :returns: int -- address of the control block.
    :raises RTTError: if it is not found.
    """
    if addr is not None and size == 0:
        return addr

    if addr is None:
        region = target.get_memory_map().get_default_region_of_type(MemoryType.RAM)
        if region is None:
            raise RTTError("target has no RAM region; pass --rtt-addr")
        addr = region.start
        if size is None:
            size = region.length
    elif size is None:
        size = 0x40000

    pad = block_id + b'\0' * (16 - len(block_id))
    chunk = 16 * 1024
    # Overlap consecutive blocks so an ID straddling a boundary is still seen.
    overlap = len(pad)
    pos = addr
    end = addr + size
    while pos < end:
        n = min(chunk, end - pos)
        try:
            data = read_bytes(target, pos, n)
        except Exception:
            # Unmapped holes inside a region are normal; skip and continue.
            pos += n
            continue
        # The target writes acID last, so a partially-initialised block never
        # matches the padded form - which is exactly the guarantee we want.
        hit = data.find(pad)
        if hit == -1:
            hit = data.find(block_id)
        if hit != -1:
            return pos + hit
        pos += max(n - overlap, 1)

    raise RTTError(f"no RTT control block in 0x{addr:08X}..0x{addr + size:08X}")


def open_channels(target, cb_addr):
    """Parses a control block into up and down channel objects."""
    header = bytes(target.read_memory_block8(cb_addr, CB_HEADER_SIZE))
    num_up, num_down = struct.unpack('<ii', header[16:24])
    if not (0 < num_up <= 16) or not (0 <= num_down <= 16):
        raise RTTError(
            f"control block at 0x{cb_addr:08X} is implausible "
            f"(up={num_up}, down={num_down})")

    up_base = cb_addr + CB_HEADER_SIZE
    ups = [UpChannel(target, up_base + i * RING_DESC_SIZE, i) for i in range(num_up)]
    down_base = up_base + num_up * RING_DESC_SIZE
    downs = [DownChannel(target, down_base + i * RING_DESC_SIZE, i) for i in range(num_down)]
    return ups, downs


def select_channel(channels, wanted, kind):
    """Resolves a channel given either an index or a name."""
    if wanted is None:
        return channels[0]
    if isinstance(wanted, str) and wanted.isdigit():
        wanted = int(wanted)
    if isinstance(wanted, int):
        if not 0 <= wanted < len(channels):
            raise RTTError(f"no {kind} channel {wanted}; target has {len(channels)}")
        return channels[wanted]
    for channel in channels:
        if channel.name == wanted:
            return channel
    names = ', '.join(repr(c.name) for c in channels)
    raise RTTError(f"no {kind} channel named {wanted!r}; target has {names}")


def default_scratch(target):
    """Picks a scratch address in the middle of the target's RAM region."""
    region = target.get_memory_map().get_default_region_of_type(MemoryType.RAM)
    if region is None:
        raise RTTError("target has no RAM region; pass --scratch")
    # Well above the reset vector's stack and any low globals.
    return (region.start + region.length // 2) & ~0xFF


def plant_control_block(target, base, up_size, down_size):
    """Writes a synthetic RTT control block into target RAM.

    Lets the reader be exercised against a target whose firmware has no RTT in
    it at all, which is what makes ``selftest`` and ``bench`` runnable on any
    board rather than only on one that already emits a trace.

    :returns: tuple -- (saved bytes, layout size) for restoring afterwards.
    """
    # Header, both descriptors, the name blob, both rings, and slack for the
    # alignment of the first ring.
    layout_size = CB_HEADER_SIZE + 2 * RING_DESC_SIZE + 32 + up_size + down_size + 4
    saved = bytes(target.read_memory_block8(base, layout_size))

    up_desc = base + CB_HEADER_SIZE
    down_desc = up_desc + RING_DESC_SIZE
    names = down_desc + RING_DESC_SIZE
    up_ring = (names + 32 + 3) & ~3
    down_ring = up_ring + up_size

    target.write_memory_block8(names, b'Up\0Down\0' + b'\0' * 24)
    target.write_memory_block8(up_desc, struct.pack(
        '<6I', names, up_ring, up_size, 0, 0, 0))
    target.write_memory_block8(down_desc, struct.pack(
        '<6I', names + 3, down_ring, down_size, 0, 0, 0))
    # acID last, exactly as the target's own RTT init does, so a half-written
    # block is never discoverable.
    target.write_memory_block8(base, struct.pack('<16sii', RTT_ID, 1, 1))
    return saved, layout_size


class Link:
    """A connected pyOCD session, opened without disturbing the target.

    RTT is only useful against a *running* core, so this attaches rather than
    halting, and leaves the target running on the way out.
    """

    def __init__(self, args):
        self.args = args
        self.session = None
        self.target = None

    def __enter__(self):
        # Callers open the link to find the control block and then hand it to a
        # ``with``, so entry has to be idempotent. Without this guard the second
        # entry builds a second session and fails to claim an interface the
        # first one is still holding.
        if self.session is not None:
            return self

        options = {
            'target_override': self.args.target,
            'frequency': self.args.frequency,
            'connect_mode': 'attach',
            'resume_on_disconnect': True,
        }
        if self.args.probe_type:
            options['probe_type'] = self.args.probe_type
        if self.args.probe:
            options['unique_id'] = self.args.probe

        self.session = ConnectHelper.session_with_chosen_probe(
            blocking=False, return_first=True, **options)
        if self.session is None:
            raise RTTError("no debug probe found")
        self.session.open()
        self.target = self.session.target
        return self

    def __exit__(self, *exc):
        session, self.session = self.session, None
        self.target = None
        if session is not None:
            try:
                session.close()
            except Exception:
                pass
        return False


def attach(args, quiet=False):
    """Connects and resolves the control block. Returns (link, ups, downs)."""
    link = Link(args).__enter__()
    try:
        cb_addr = find_control_block(link.target, args.rtt_addr, args.rtt_size)
        ups, downs = open_channels(link.target, cb_addr)
    except Exception:
        link.__exit__(None, None, None)
        raise

    if not quiet:
        print(f"probe    {link.session.probe.unique_id} ({link.session.probe.product_name})",
              file=sys.stderr)
        print(f"rtt      control block at 0x{cb_addr:08X}, "
              f"{len(ups)} up / {len(downs)} down", file=sys.stderr)
        for channel in ups + downs:
            print(f"         {channel!r}", file=sys.stderr)
    return link, ups, downs


def cmd_log(args):
    """Drains a text channel until interrupted or the time limit expires."""
    link, ups, _downs = attach(args)
    with link:
        channel = select_channel(ups, args.channel, 'up')
        sink = open(args.output, 'wb') if args.output else None
        deadline = time.monotonic() + args.seconds if args.seconds else None
        started = time.monotonic()
        try:
            while deadline is None or time.monotonic() < deadline:
                data = channel.read()
                if data:
                    if sink:
                        sink.write(data)
                        sink.flush()
                    else:
                        sys.stdout.write(data.decode('utf-8', 'backslashreplace'))
                        sys.stdout.flush()
                else:
                    time.sleep(args.poll)
        except KeyboardInterrupt:
            pass
        finally:
            if sink:
                sink.close()

        elapsed = time.monotonic() - started
        _report(channel, elapsed)
    return 0


def cmd_sysview(args):
    """Records a SystemView session to a .SVDat file.

    The file is the raw channel byte stream, which is exactly what SEGGER's
    own J-Link RTT Logger writes and what the SystemView application opens.
    """
    link, ups, downs = attach(args)
    with link:
        channel = select_channel(
            ups, args.channel if args.channel is not None else SYSVIEW_CHANNEL_NAME, 'up')
        command_channel = None
        if not args.no_start:
            try:
                command_channel = select_channel(
                    downs, args.channel if args.channel is not None else SYSVIEW_CHANNEL_NAME,
                    'down')
            except RTTError as exc:
                print(f"rttpull: {exc}; recording without sending START", file=sys.stderr)

        if command_channel is not None:
            command_channel.write(bytes([SYSVIEW_CMD_START]))
            print(f"sysview  START sent on down channel {command_channel.index}",
                  file=sys.stderr)

        deadline = time.monotonic() + args.seconds if args.seconds else None
        started = time.monotonic()
        with open(args.output, 'wb') as sink:
            try:
                while deadline is None or time.monotonic() < deadline:
                    data = channel.read()
                    if data:
                        sink.write(data)
                    else:
                        time.sleep(args.poll)
            except KeyboardInterrupt:
                print(file=sys.stderr)
            finally:
                if command_channel is not None:
                    try:
                        command_channel.write(bytes([SYSVIEW_CMD_STOP]))
                    except Exception:
                        pass
                # Anything the target produced between the last drain and STOP
                # still belongs in the file.
                try:
                    sink.write(channel.read())
                except Exception:
                    pass

        elapsed = time.monotonic() - started
        _report(channel, elapsed)
        print(f"wrote    {args.output} ({channel.total_bytes} bytes) - "
              f"open it with SystemView", file=sys.stderr)
    return 0


def _report(channel, elapsed):
    rate = channel.total_bytes / 1024.0 / elapsed if elapsed > 0 else 0.0
    print(f"drained  {channel.total_bytes} bytes in {elapsed:.1f} s = {rate:.1f} KiB/s; "
          f"largest single drain {channel.high_water} of {channel.size} B, "
          f"{channel.near_full} near-full reads", file=sys.stderr)


def cmd_bench(args):
    """Measures how fast this host can pull bytes out of a ring buffer.

    That number is what decides whether a trace survives: RTT costs the target
    almost nothing, so a capture drops data when the *host* cannot keep up with
    the producer. The ring is filled from the host so the measurement does not
    depend on target firmware doing anything.
    """
    link = Link(args).__enter__()
    with link:
        target = link.target
        planted = None

        if args.scratch is not None or args.rtt_addr is None:
            # Plant a ring so the number does not depend on the target's
            # firmware having RTT compiled in.
            base = args.scratch if args.scratch is not None else default_scratch(target)
            was_halted = target.is_halted()
            # A planted ring lives in RAM the running firmware owns, so it has
            # to be halted or the pattern is rewritten underneath the read and
            # every comparison fails. A real channel is left running, since the
            # whole point of RTT is tracing a live system.
            if not was_halted:
                target.halt()
            saved, _size = plant_control_block(target, base, args.ring, 16)
            planted = (base, saved, was_halted)
            cb_addr = base
        else:
            cb_addr = find_control_block(target, args.rtt_addr, args.rtt_size)

        try:
            ups, _downs = open_channels(target, cb_addr)
            channel = select_channel(ups, args.channel, 'up')
            if not channel.valid:
                raise RTTError(f"up channel {channel.index} has no buffer allocated")

            print(f"bench    ring {channel.size} B at 0x{channel.buffer_addr:08X}"
                  f"{' (planted)' if planted else ''}", file=sys.stderr)

            # Word-aligned: pyOCD reads an unaligned tail a byte at a time, one
            # USB round trip each, which would price in an artefact of the
            # measurement rather than the transport.
            span = (channel.size - 1) & ~3
            saved_ring = bytes(target.read_memory_block8(channel.buffer_addr, channel.size))
            saved_offsets = target.read_memory_block32(channel.desc_addr + WROFF_OFFSET, 2)
            pattern = bytes((i * 7 + 13) & 0xFF for i in range(channel.size))

            target_bytes = int(args.mb * 1024 * 1024)
            moved = 0
            errors = 0
            started = time.monotonic()
            try:
                target.write_memory_block8(channel.buffer_addr, pattern)
                while moved < target_bytes:
                    # Present a full ring, then drain it through the same code
                    # path a real capture uses. RdOff=0/WrOff=span makes the
                    # whole buffer readable as one contiguous run, which is the
                    # best case a capture ever sees.
                    target.write32(channel.desc_addr + RDOFF_OFFSET, 0)
                    target.write32(channel.desc_addr + WROFF_OFFSET, span)
                    data = channel.read()
                    if data != pattern[:span]:
                        errors += 1
                    moved += len(data)
            finally:
                target.write_memory_block8(channel.buffer_addr, saved_ring)
                target.write32(channel.desc_addr + WROFF_OFFSET, saved_offsets[0])
                target.write32(channel.desc_addr + RDOFF_OFFSET, saved_offsets[1])
        finally:
            if planted is not None:
                base, saved, was_halted = planted
                target.write_memory_block8(base, saved)
                if not was_halted and target.is_halted():
                    target.resume()

        elapsed = time.monotonic() - started
        rate = moved / 1024.0 / elapsed
        verdict = 'PASS' if errors == 0 and rate >= args.min_kib else 'FAIL'
        print(f"{verdict} bench  {moved / 1048576.0:.2f} MiB in {elapsed:.1f} s = "
              f"{rate:.1f} KiB/s (floor {args.min_kib}), {errors} mismatches, "
              f"ring {channel.size} B")
        if rate < args.produce:
            print(f"     warning: slower than the assumed {args.produce} KiB/s producer; "
                  f"a capture would fall behind", file=sys.stderr)
        return 0 if verdict == 'PASS' else 1


def cmd_selftest(args):
    """Proves the reader is byte-exact without needing RTT in target firmware.

    A synthetic control block is planted in target RAM and the host plays both
    parts: producer (advancing WrOff) and consumer (this tool's own channel
    code). That covers control-block discovery, descriptor parsing, the
    wrapped and unwrapped read paths, and RdOff publication - over the real
    probe and the real transport - on any target with some spare RAM.
    """
    link = Link(args).__enter__()
    with link:
        target = link.target
        base = args.scratch if args.scratch is not None else default_scratch(target)
        ring_size = args.ring
        was_halted = target.is_halted()
        saved, _layout_size = plant_control_block(target, base, ring_size, ring_size)

        try:
            found = find_control_block(target, base, 0x1000)
            if found != base:
                raise RTTError(
                    f"discovery found 0x{found:08X}, planted at 0x{base:08X}")

            ups, downs = open_channels(target, found)
            channel = ups[0]
            if channel.name != 'Up' or channel.size != ring_size:
                raise RTTError(
                    f"descriptor parsed wrong: name={channel.name!r} size={channel.size}")

            # A prime chunk against a power-of-two ring guarantees the wrapped
            # read path is exercised, and at several different alignments.
            expected = bytes((i * 31 + 7) & 0xFF for i in range(args.bytes))
            chunk = 251
            produced = 0
            drained = bytearray()
            wraps = 0
            while len(drained) < len(expected):
                if produced < len(expected):
                    wr = target.read32(channel.desc_addr + WROFF_OFFSET)
                    rd = target.read32(channel.desc_addr + RDOFF_OFFSET)
                    free = (rd - wr - 1) % ring_size
                    n = min(chunk, free, len(expected) - produced)
                    if n:
                        payload = expected[produced:produced + n]
                        tail = min(n, ring_size - wr)
                        target.write_memory_block8(channel.buffer_addr + wr, payload[:tail])
                        if tail < n:
                            target.write_memory_block8(channel.buffer_addr, payload[tail:])
                            wraps += 1
                        target.write32(channel.desc_addr + WROFF_OFFSET,
                                       (wr + n) % ring_size)
                        produced += n

                got = channel.read()
                if got:
                    start = len(drained)
                    if expected[start:start + len(got)] != got:
                        bad = next(i for i in range(len(got))
                                   if expected[start + i] != got[i])
                        raise RTTError(
                            f"byte {start + bad} mismatch: got 0x{got[bad]:02X}, "
                            f"expected 0x{expected[start + bad]:02X}")
                    drained += got

            if wraps == 0:
                raise RTTError("selftest never wrapped the ring; it proves less than it should")

            # The down path carries SystemView's START/STOP, so check it too.
            downs[0].write(b'\x01\x02\x03')
            wr = target.read32(downs[0].desc_addr + WROFF_OFFSET)
            echo = bytes(target.read_memory_block8(downs[0].buffer_addr, 3))
            if wr != 3 or echo != b'\x01\x02\x03':
                raise RTTError(f"down channel wrote {echo!r} and left WrOff={wr}")
        finally:
            target.write_memory_block8(base, saved)
            if not was_halted and target.is_halted():
                target.resume()

        print(f"PASS selftest  {len(drained)} bytes byte-exact through a {ring_size} B ring "
              f"at 0x{base:08X}, {wraps} wraps, down channel verified")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.split('\n')[0],
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--probe', help='probe unique ID (default: the only one attached)')
    parser.add_argument('--probe-type', help='cmsis-dap, jlink, stlink, ...')
    parser.add_argument('--target', default='cortex_m',
                        help='pyOCD target type (default: cortex_m)')
    parser.add_argument('--frequency', type=int, default=10000000,
                        help='SWD clock in Hz (default: 10000000)')
    parser.add_argument('--rtt-addr', type=lambda s: int(s, 0),
                        help='control block address, or the base of the search range')
    parser.add_argument('--rtt-size', type=lambda s: int(s, 0),
                        help='search range in bytes; 0 means --rtt-addr is exact')
    parser.add_argument('--poll', type=float, default=0.005,
                        help='seconds to sleep when a channel is empty (default: 0.005)')

    sub = parser.add_subparsers(dest='command', required=True)

    p_log = sub.add_parser('log', help='drain a text channel')
    p_log.add_argument('--channel', default=None, help='up channel index or name (default: 0)')
    p_log.add_argument('--output', help='write raw bytes here instead of stdout')
    p_log.add_argument('--seconds', type=float, help='stop after this long')
    p_log.set_defaults(func=cmd_log)

    p_sv = sub.add_parser('sysview', help='record a SystemView .SVDat')
    p_sv.add_argument('--channel', default=None,
                      help=f'channel index or name (default: {SYSVIEW_CHANNEL_NAME!r})')
    p_sv.add_argument('--output', default='trace.SVDat', help='output file')
    p_sv.add_argument('--seconds', type=float, help='stop after this long')
    p_sv.add_argument('--no-start', action='store_true',
                      help='do not send the START command; just record')
    p_sv.set_defaults(func=cmd_sysview)

    p_bench = sub.add_parser('bench', help='measure host drain throughput')
    p_bench.add_argument('--channel', default=None, help='up channel index or name')
    p_bench.add_argument('--scratch', type=lambda s: int(s, 0),
                         help='plant a ring here instead of using the target\'s own RTT')
    p_bench.add_argument('--ring', type=int, default=4096,
                         help='planted ring size (default: 4096)')
    p_bench.add_argument('--mb', type=float, default=4.0, help='MiB to move (default: 4)')
    p_bench.add_argument('--min-kib', type=float, default=200.0,
                         help='pass/fail floor in KiB/s (default: 200)')
    p_bench.add_argument('--produce', type=float, default=160.0,
                         help='assumed producer rate in KiB/s (default: 160)')
    p_bench.set_defaults(func=cmd_bench)

    p_self = sub.add_parser('selftest', help='verify the reader against planted RTT')
    p_self.add_argument('--scratch', type=lambda s: int(s, 0),
                        help='scratch RAM address (default: middle of the RAM region)')
    p_self.add_argument('--ring', type=int, default=1024, help='ring size (default: 1024)')
    p_self.add_argument('--bytes', type=int, default=16384,
                        help='bytes to push through it (default: 16384)')
    p_self.set_defaults(func=cmd_selftest)

    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except RTTError as exc:
        print(f"rttpull: {exc}", file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
