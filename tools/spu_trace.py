"""spu_trace.py <trace.bin> [--blocks N]: a summary of an SPU channel trace (ps3recomp spu_channels.c,
SPU_TRACE_IMAGE): the header, the records by kind, channel and DMA command, the stops (stop-and-signal codes:
one 0x110 receive a block for FMOD's mixer), the DMA bytes; checks the file's structure end to end.
--blocks N: also the channel sequence of the first N blocks (the records between two 0x110 stops), compactly."""
import collections, struct, sys

KIND = {1: "rdch", 2: "wrch", 3: "dma", 4: "stop", 5: "end"}
CHAN = {0: "RdEventStat", 1: "WrEventMask", 2: "WrEventAck", 3: "RdSigNotify1", 4: "RdSigNotify2", 7: "WrDec",
        8: "RdDec", 11: "RdEventMask", 13: "RdMachStat", 14: "WrSRR0", 15: "RdSRR0", 16: "MFC_LSA", 17: "MFC_EAH",
        18: "MFC_EAL", 19: "MFC_Size", 20: "MFC_TagID", 21: "MFC_Cmd", 22: "MFC_WrTagMask", 23: "MFC_WrTagUpdate",
        24: "MFC_RdTagStat", 25: "MFC_RdListStallStat", 26: "MFC_WrListStallAck", 27: "MFC_RdAtomicStat",
        28: "WrOutMbox", 29: "RdInMbox", 30: "WrOutIntrMbox"}
CMD = {0x20: "PUT", 0x21: "PUTB", 0x22: "PUTF", 0x24: "PUTL", 0x40: "GET", 0x41: "GETB", 0x42: "GETF", 0x44: "GETL",
       0xD0: "GETLLAR", 0xB4: "PUTLLC", 0xB0: "PUTLLUC", 0xB8: "PUTQLLUC", 0xC0: "BARRIER", 0xCC: "SYNC"}


def main():
    path = sys.argv[1]
    nblocks = int(sys.argv[sys.argv.index("--blocks") + 1]) if "--blocks" in sys.argv else 0
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"SPUTRC01":
        sys.exit("not a trace (magic %r)" % data[:8])
    image, entry, _, _ = struct.unpack_from("<IIII", data, 8)
    off = 24 + 128 * 16 + 0x40000
    print("%s: %.1f MB, image %d, entry pc 0x%05X" % (path, len(data) / 1048576.0, image, entry))
    kinds, rd, wr, cmds, stops = (collections.Counter() for _ in range(5))
    dma_bytes = collections.Counter()
    end = None
    block, shown, seq = 0, 0, []
    n = 0
    while off + 24 <= len(data):
        kind, a, b, c, d, ln = struct.unpack_from("<IIIIII", data, off)
        off += 24
        if kind not in KIND or off + ln > len(data):
            sys.exit("bad record at offset %d (kind %d, n %d)" % (off - 24, kind, ln))
        n += 1
        kinds[kind] += 1
        if kind == 1: rd[a] += 1
        elif kind == 2: wr[a] += 1
        elif kind == 3:
            cmds[a & 0xFF] += 1
            dma_bytes[a & 0xFF] += ln
        elif kind == 4:
            stops[a] += 1
            if a == 0x110:
                block += 1
                if nblocks and shown < nblocks and seq:
                    print("  block %d: %s" % (block - 1, " ".join(seq)))
                    shown += 1
                seq = []
        elif kind == 5:
            end = a
        if nblocks and shown < nblocks:
            if kind == 1: seq.append("r%s=%X" % (CHAN.get(a, a), b))
            elif kind == 2 and a != 21: seq.append("w%s=%X" % (CHAN.get(a, a), b))
            elif kind == 3: seq.append("%s(%X<-%08X,%X)" % (CMD.get(a & 0xFF, hex(a & 0xFF)), b, c, d))
            elif kind == 4: seq.append("stop%X" % a)
        off += ln
    if off != len(data):
        sys.exit("trailing %d bytes" % (len(data) - off))
    print("records: %d; %s; ended: %s" % (n, ", ".join("%s %d" % (KIND[k], v) for k, v in sorted(kinds.items())),
                                          {1: "the size limit", 2: "the thread ended", None: "no end record (cut off)"}[end]))
    print("stops: %s; blocks (0x110 receives): %d" % (", ".join("0x%X x%d" % (k, v) for k, v in sorted(stops.items())), stops[0x110]))
    print("rdch: " + ", ".join("%s %d" % (CHAN.get(k, k), v) for k, v in rd.most_common()))
    print("wrch: " + ", ".join("%s %d" % (CHAN.get(k, k), v) for k, v in wr.most_common()))
    print("dma: " + ", ".join("%s %d (%.1f MB)" % (CMD.get(k, hex(k)), v, dma_bytes[k] / 1048576.0) for k, v in cmds.most_common()))


if __name__ == "__main__":
    main()
