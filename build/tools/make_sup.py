# Writes a minimal PGS (.sup) file with a few boxed captions, for testing only.
import struct, sys

def seg(pts, typ, data):
    return b'PG' + struct.pack('>IIBH', int(pts * 90000), 0, typ, len(data)) + data

def rle(rows):
    out = bytearray()
    for row in rows:
        i = 0
        while i < len(row):
            c = row[i]; n = 1
            while i + n < len(row) and row[i + n] == c and n < 16383: n += 1
            if c == 0:
                out += bytes([0, n]) if n < 64 else bytes([0, 0x40 | (n >> 8), n & 0xff])
            elif n < 3:
                out += bytes([c]) * n
            else:
                out += bytes([0, 0x80 | n, c]) if n < 64 else bytes([0, 0xC0 | (n >> 8), n & 0xff, c])
            i += n
        out += b'\x00\x00'
    return bytes(out)

W, H, BW, BH = 1280, 720, 500, 70
rows = []
for y in range(BH):
    row = []
    for x in range(BW):
        border = x < 4 or y < 4 or x >= BW - 4 or y >= BH - 4
        stripe = (x // 25) % 2 == 0 and 20 < y < 50
        row.append(2 if border else (1 if stripe else 3))
    rows.append(row)
obj = rle(rows)
x, y = (W - BW) // 2, H - BH - 40

def show(t, n):
    pcs = struct.pack('>HHBHBBBB', W, H, 0x10, n, 0x80, 0, 0, 1) + struct.pack('>HBBHH', 0, 0, 0, x, y)
    wds = struct.pack('>BBHHHH', 1, 0, x, y, BW, BH)
    pds = struct.pack('>BB', 0, 0) + bytes([1, 235, 128, 128, 255, 2, 16, 128, 128, 255, 3, 81, 90, 240, 200])
    ods = struct.pack('>HBB', 0, 0, 0xC0) + struct.pack('>I', len(obj) + 4)[1:] + struct.pack('>HH', BW, BH) + obj
    return seg(t, 0x16, pcs) + seg(t, 0x17, wds) + seg(t, 0x14, pds) + seg(t, 0x15, ods) + seg(t, 0x80, b'')

def clear(t, n):
    pcs = struct.pack('>HHBHBBBB', W, H, 0x10, n, 0x00, 0, 0, 0)
    wds = struct.pack('>BBHHHH', 1, 0, x, y, BW, BH)
    return seg(t, 0x16, pcs) + seg(t, 0x17, wds) + seg(t, 0x80, b'')

data, n = b'', 0
for start, end in [(2, 5), (10, 13), (40, 44), (125, 130)]:
    data += show(start, n) + clear(end, n + 1); n += 2
open(sys.argv[1], 'wb').write(data)
