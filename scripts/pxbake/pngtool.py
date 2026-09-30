"""Minimal pure-python PNG read/write (PIL is broken on this machine).

read(path) -> (w, h, rows) where rows[y] is a list of (r,g,b,a).
write(path, w, h, rows)
contact(path_in, path_out, frames, cols, scale): lay out a vertical strip as a grid.
"""
import struct, zlib, sys


def read(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos = 8
    idat = b""
    plte = None
    trns = None
    while pos < len(data):
        n, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    assert interlace == 0, "interlaced not supported"
    raw = zlib.decompress(idat)
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp_bits = chans * depth
    stride = (w * bpp_bits + 7) // 8
    bpp = max(1, bpp_bits // 8)
    prev = bytearray(stride)
    rows = []
    p = 0
    for y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        prev = line
        px = []
        for x in range(w):
            if depth == 8:
                v = line[x * chans:(x + 1) * chans]
            elif depth == 16:
                v = [line[(x * chans + k) * 2] for k in range(chans)]
            else:
                bit = x * depth
                v = [(line[bit // 8] >> (8 - depth - bit % 8)) & ((1 << depth) - 1)]
            if ctype == 6:
                px.append(tuple(v))
            elif ctype == 2:
                px.append((v[0], v[1], v[2], 255))
            elif ctype == 0:
                g = v[0] * 255 // ((1 << depth) - 1) if depth < 8 else v[0]
                px.append((g, g, g, 255))
            elif ctype == 4:
                px.append((v[0], v[0], v[0], v[1]))
            elif ctype == 3:
                r, g, b = plte[v[0]]
                al = trns[v[0]] if trns and v[0] < len(trns) else 255
                px.append((r, g, b, al))
        rows.append(px)
    return w, h, rows


def write(path, w, h, rows):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for (r, g, b, a) in rows[y]:
            raw += bytes((r, g, b, a))
    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(out)


def contact(src, dst, frames, cols, scale=2, bg=(40, 40, 40, 255)):
    w, h, rows = read(src)
    fh = h // frames
    nrows = (frames + cols - 1) // cols
    cw, ch = w * scale + 2, fh * scale + 2
    W, H = cols * cw, nrows * ch
    out = [[bg] * W for _ in range(H)]
    for f in range(frames):
        ox, oy = (f % cols) * cw + 1, (f // cols) * ch + 1
        for y in range(fh * scale):
            for x in range(w * scale):
                p = rows[f * fh + y // scale][x // scale]
                if p[3] > 128:
                    out[oy + y][ox + x] = p
    write(dst, W, H, out)


if __name__ == "__main__":
    contact(sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]) if len(sys.argv) > 5 else 2)
