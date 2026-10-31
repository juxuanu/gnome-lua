#!/usr/bin/env python3
# Compares the pixels of two PNG files (8 bits, RGB or RGBA; the alpha is
# ignored): prints the size of each and how many pixels differ, and where,
# and exits 1 when they differ or the sizes do not match. A rectangle given
# after the files is left out of the comparison. No dependency beyond Python.
#
# Usage: compare-png.py A.png B.png [X Y WIDTH HEIGHT]
#
# SPDX-License-Identifier: GPL-2.0-or-later

import struct
import sys
import zlib


def load(path):
    data = open(path, 'rb').read()
    pos = 8
    idat = b''
    width = height = channels = 0
    while pos < len(data):
        length, = struct.unpack('>I', data[pos:pos + 4])
        kind = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if kind == b'IHDR':
            width, height, depth, color = struct.unpack('>IIBB', chunk[:10])
            if depth != 8 or color not in (2, 6):
                raise SystemExit(f'{path}: unsupported PNG (depth {depth}, color type {color})')
            channels = 4 if color == 6 else 3
        elif kind == b'IDAT':
            idat += chunk
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = width * channels
    previous = bytearray(stride)
    rows = []
    for y in range(height):
        start = y * (stride + 1)
        kind = raw[start]
        line = bytearray(raw[start + 1:start + 1 + stride])
        for i in range(stride):
            a = line[i - channels] if i >= channels else 0
            b = previous[i]
            c = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([bytes(line[i:i + 3]) for i in range(0, stride, channels)])
        previous = line
    return width, height, rows


def main():
    (wa, ha, a), (wb, hb, b) = load(sys.argv[1]), load(sys.argv[2])
    ix, iy, iw, ih = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 0, 0, 0)
    if (wa, ha) != (wb, hb):
        print(f'sizes differ: {wa}x{ha} and {wb}x{hb}')
        return 1
    points = [(x, y) for y in range(ha) for x in range(wa)
              if a[y][x] != b[y][x] and not (ix <= x < ix + iw and iy <= y < iy + ih)]
    colors = len({pixel for row in a for pixel in row})
    where = ''
    if points:
        xs = [x for x, _ in points]
        ys = [y for _, y in points]
        where = f', within {min(xs)},{min(ys)} to {max(xs)},{max(ys)}'
    print(f'{wa}x{ha}, {colors} colors, {len(points)} pixels differ{where}')
    return 1 if points else 0


if __name__ == '__main__':
    sys.exit(main())
