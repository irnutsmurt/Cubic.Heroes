#!/usr/bin/env python3
"""Read guest-memory snapshots written by SPURS_SNAP (runtime patch; docs/BULLET_NATIVE.md).

  from snap_read import Snap; s = Snap("build/snap/s1_pre.bin")
  s.u32(ea), s.f32(ea), s.dump(ea, n, floats=True)

A snapshot is records of (u32 ea, u32 size, little-endian) followed by the bytes;
guest values are big-endian. Snapshots are game memory: keep them out of git."""
import bisect
import struct


class Snap:
    def __init__(self, path):
        data = open(path, "rb").read()
        self.ranges, self.bufs = [], []
        i = 0
        while i + 8 <= len(data):
            ea, n = struct.unpack_from("<II", data, i)
            i += 8
            self.ranges.append((ea, ea + n))
            self.bufs.append(memoryview(data)[i:i + n])
            i += n
        self.starts = [r[0] for r in self.ranges]

    def _find(self, ea):
        k = bisect.bisect_right(self.starts, ea) - 1
        if k < 0 or ea >= self.ranges[k][1]:
            raise KeyError(hex(ea))
        return self.bufs[k], ea - self.ranges[k][0]

    def valid(self, ea, n=4):
        try:
            b, o = self._find(ea)
            return o + n <= len(b)
        except KeyError:
            return False

    def bytes(self, ea, n):
        b, o = self._find(ea)
        return bytes(b[o:o + n])

    def u32(self, ea): return struct.unpack(">I", self.bytes(ea, 4))[0]
    def u16(self, ea): return struct.unpack(">H", self.bytes(ea, 2))[0]
    def f32(self, ea): return struct.unpack(">f", self.bytes(ea, 4))[0]

    def dump(self, ea, n, floats=False):
        out = []
        for row in range(0, n, 16):
            ws = [self.u32(ea + row + k) for k in range(0, 16, 4) if row + k < n]
            line = f"  {ea + row:08X}: " + " ".join(f"{w:08X}" for w in ws)
            if floats:
                line += "   " + " ".join(f"{struct.unpack('>f', struct.pack('>I', w))[0]:10.4g}" for w in ws)
            out.append(line)
        return "\n".join(out)
