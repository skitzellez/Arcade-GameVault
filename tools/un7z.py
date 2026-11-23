# Copyright (c) 2026 Skitzelles - part of Game Vault's arcade loader, GPL-3.0.
# un7z.py <archive.7z> <out folder>: unpacks 7z archives made with 7-Zip ZS / NanaZip (Zstandard inside), which the
# standard 7-Zip cannot open. TeknoParrot game packs come like this. Needs Python 3.14+ (compression.zstd), e.g.
# MSYS2's C:/msys64/ucrt64/bin/python.exe. Methods: Copy, LZMA, LZMA2, Zstandard, with the x86 BCJ and Delta filters.
import lzma, os, struct, sys, zlib
from compression import zstd


class R:
    def __init__(self, b):
        self.b, self.p = b, 0

    def byte(self):
        v = self.b[self.p]
        self.p += 1
        return v

    def bytes(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def u32(self):
        return struct.unpack("<I", self.bytes(4))[0]

    def num(self):   # 7z NUMBER
        first = self.byte()
        mask = 0x80
        for i in range(8):
            if not first & mask:
                hi = first & (mask - 1)
                v = int.from_bytes(self.bytes(i), "little") if i else 0
                return v | (hi << (8 * i))
            mask >>= 1
        return int.from_bytes(self.bytes(8), "little")

    def bits(self, n):
        out, cur, m = [], 0, 0
        for _ in range(n):
            if m == 0:
                cur, m = self.byte(), 0x80
            out.append(bool(cur & m))
            m >>= 1
        return out

    def defined(self, n):
        return [True] * n if self.byte() else self.bits(n)


def digests(r, n):
    d = r.defined(n)
    for x in d:
        if x:
            r.u32()


def folder(r):
    coders, tin, tout = [], 0, 0
    for _ in range(r.num()):
        f = r.byte()
        cid = int.from_bytes(r.bytes(f & 0xF), "big")
        nin, nout = (r.num(), r.num()) if f & 0x10 else (1, 1)
        props = r.bytes(r.num()) if f & 0x20 else b""
        coders.append({"id": cid, "in": nin, "out": nout, "props": props, "in0": tin, "out0": tout})
        tin += nin
        tout += nout
    binds = [(r.num(), r.num()) for _ in range(tout - 1)]
    npacked = tin - len(binds)
    if npacked == 1:
        bound = {i for i, _ in binds}
        packed = [next(i for i in range(tin) if i not in bound)]
    else:
        packed = [r.num() for _ in range(npacked)]
    return {"coders": coders, "binds": binds, "packed": packed, "tout": tout}


def streams_info(r):
    s = {"packpos": 0, "packsizes": [], "folders": [], "unpack": [], "nsub": None, "subsizes": None}
    while True:
        t = r.byte()
        if t == 0:
            return s
        if t == 6:   # pack info
            s["packpos"] = r.num()
            n = r.num()
            while True:
                u = r.byte()
                if u == 0:
                    break
                if u == 9:
                    s["packsizes"] = [r.num() for _ in range(n)]
                elif u == 10:
                    digests(r, n)
        elif t == 7:   # unpack info
            assert r.byte() == 0x0B
            nf = r.num()
            assert r.byte() == 0, "external folders"
            s["folders"] = [folder(r) for _ in range(nf)]
            assert r.byte() == 0x0C
            for f in s["folders"]:
                f["sizes"] = [r.num() for _ in range(f["tout"])]
            while True:
                u = r.byte()
                if u == 0:
                    break
                if u == 10:
                    d = r.defined(nf)
                    for i, x in enumerate(d):
                        if x:
                            r.u32()
                            s["folders"][i]["crc"] = True
        elif t == 8:   # substreams
            nf = len(s["folders"])
            nsub = [1] * nf
            sizes = None
            while True:
                u = r.byte()
                if u == 0:
                    break
                if u == 0x0D:
                    nsub = [r.num() for _ in range(nf)]
                elif u == 9:
                    sizes = []
                    for fi, n in enumerate(nsub):
                        if not n:
                            continue
                        tot = 0
                        for _ in range(n - 1):
                            v = r.num()
                            sizes.append(v)
                            tot += v
                        sizes.append(main_size(s["folders"][fi]) - tot)
                elif u == 10:
                    digests(r, sum(n for fi, n in enumerate(nsub) if not (n == 1 and s["folders"][fi].get("crc"))))
            if sizes is None:
                sizes = []
                for fi, n in enumerate(nsub):
                    if n:
                        sizes.append(main_size(s["folders"][fi]))
            s["nsub"], s["subsizes"] = nsub, sizes
        else:
            raise ValueError("streams info %#x" % t)


def main_size(f):
    bound_out = {o for _, o in f["binds"]}
    return f["sizes"][next(i for i in range(f["tout"]) if i not in bound_out)]


def bcj_x86(data):   # x86 branch filter, decoding (7-Zip Bra86.c)
    b = bytearray(data)
    n = len(b)
    i, prev_mask, prev_pos = 0, 0, -5
    mask_ok = [True, True, True, False, True, False, False, False]
    bit_num = [0, 1, 2, 2, 3, 3, 3, 3]
    while i <= n - 5:
        if b[i] & 0xFE != 0xE8:
            i += 1
            continue
        d = i - prev_pos
        prev_pos = i
        if d > 3:
            prev_mask = 0
        else:
            prev_mask = (prev_mask << (d - 1)) & 7
            if prev_mask and (not mask_ok[prev_mask] or b[i + 4 - bit_num[prev_mask]] in (0, 0xFF)):
                prev_pos = i
                prev_mask = ((prev_mask << 1) & 7) | 1
                i += 1
                continue
        if b[i + 4] in (0, 0xFF):
            src = b[i + 1] | b[i + 2] << 8 | b[i + 3] << 16 | b[i + 4] << 24
            while True:
                dest = (src - (i + 5)) & 0xFFFFFFFF
                if not prev_mask:
                    break
                idx = bit_num[prev_mask] * 8
                bb = (dest >> (24 - idx)) & 0xFF
                if bb not in (0, 0xFF):
                    break
                src = dest ^ ((1 << (32 - idx)) - 1)
            dest = dest & 0x01FFFFFF
            if dest & 0x01000000:
                dest |= 0xFF000000
            b[i + 1:i + 5] = struct.pack("<I", dest & 0xFFFFFFFF)
            i += 5
            prev_mask = 0
        else:
            prev_mask = ((prev_mask << 1) & 7) | 1
            i += 1
    return bytes(b)


def delta(data, dist):
    b = bytearray(data)
    for i in range(dist, len(b)):
        b[i] = (b[i] + b[i - dist]) & 0xFF
    return bytes(b)


def run_coder(c, data, size):
    cid, props = c["id"], c["props"]
    if cid == 0:
        return data[:size]
    if cid == 0x4F71101:
        return zstd.decompress(data)[:size]
    if cid == 0x030101:
        f = lzma._decode_filter_properties(lzma.FILTER_LZMA1, props)
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[f]).decompress(data, size)
    if cid == 0x21:
        f = lzma._decode_filter_properties(lzma.FILTER_LZMA2, props)
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[f]).decompress(data, size)
    if cid == 0x03030103:
        return bcj_x86(data)
    if cid == 0x03:
        return delta(data, props[0] + 1)
    raise ValueError("method %#x not supported" % cid)


def decode_folder(arc, base, f, packpos):
    packed = []
    pos = base + packpos
    for size in f["packsizes"]:
        arc.seek(pos)
        packed.append(arc.read(size))
        pos += size
    def out_of(ci):   # (simple coders: one in, one out)
        c = f["coders"][ci]
        src = c["in0"]
        bind = next((o for i, o in f["binds"] if i == src), None)
        if bind is None:
            data = packed[f["packed"].index(src)]
        else:
            k = next(j for j, cc in enumerate(f["coders"]) if cc["out0"] <= bind < cc["out0"] + cc["out"])
            data = out_of(k)
        return run_coder(c, data, f["sizes"][c["out0"]])
    bound_out = {o for _, o in f["binds"]}
    top = next(i for i in range(f["tout"]) if i not in bound_out)
    return out_of(next(j for j, c in enumerate(f["coders"]) if c["out0"] == top))


def main(path, out):
    arc = open(path, "rb")
    sig = arc.read(32)
    assert sig[:6] == b"7z\xbc\xaf\x27\x1c", "not a 7z archive"
    off, size = struct.unpack_from("<QQ", sig, 12)
    base = 32
    arc.seek(base + off)
    hdr = arc.read(size)
    r = R(hdr)
    t = r.byte()
    while t == 0x17:   # encoded header
        s = streams_info(r)
        f = s["folders"][0]
        f["packsizes"] = s["packsizes"][:len(f["packed"])]
        r = R(decode_folder(arc, base, f, s["packpos"]))
        t = r.byte()
    assert t == 0x01
    s = None
    files = []
    while True:
        t = r.byte()
        if t == 0:
            break
        if t == 2:   # archive properties
            while r.byte():
                r.bytes(r.num())
        elif t in (3, 4):
            si = streams_info(r)
            if t == 4:
                s = si
        elif t == 5:
            n = r.num()
            empty = [False] * n
            emptyfile = None
            names = []
            attrs = [None] * n
            while True:
                pt = r.byte()
                if pt == 0:
                    break
                ps = r.num()
                q = R(r.bytes(ps))
                if pt == 0x0E:
                    empty = q.bits(n)
                elif pt == 0x0F:
                    emptyfile = q.bits(sum(empty))
                elif pt == 0x11:
                    q.byte()
                    names = q.b[q.p:].decode("utf-16le").split("\0")[:n]
                elif pt == 0x15:
                    d = q.defined(n)
                    q.byte()
                    for i in range(n):
                        if d[i]:
                            attrs[i] = q.u32()
            ei = 0
            for i in range(n):
                is_dir = False
                if empty[i]:
                    is_dir = not (emptyfile and emptyfile[ei])
                    if attrs[i] is not None:
                        is_dir = bool(attrs[i] & 0x10)
                    ei += 1
                files.append((names[i], empty[i], is_dir))
    # packed stream offsets per folder
    pi, pos = 0, s["packpos"] if s else 0
    if s:
        for f in s["folders"]:
            k = len(f["packed"])
            f["packsizes"] = s["packsizes"][pi:pi + k]
            f["packpos"] = pos
            pos += sum(f["packsizes"])
            pi += k
    stream_files = [x for x in files if not x[1]]
    si = 0
    done = 0
    for fi, f in enumerate(s["folders"] if s else []):
        n = s["nsub"][fi]
        if not n:
            continue
        data = decode_folder(arc, base, f, f["packpos"])
        at = 0
        for _ in range(n):
            name = stream_files[si][0]
            size = s["subsizes"][si]
            dst = os.path.join(out, name.replace("\\", os.sep))
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            with open(dst, "wb") as w:
                w.write(data[at:at + size])
            at += size
            si += 1
            done += 1
        print("block %d/%d: %d files" % (fi + 1, len(s["folders"]), n), flush=True)
    for name, emptystream, is_dir in files:
        if not emptystream:
            continue
        dst = os.path.join(out, name.replace("\\", os.sep))
        if is_dir:
            os.makedirs(dst, exist_ok=True)
        else:
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            open(dst, "wb").close()
    print("done: %d files" % done)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
