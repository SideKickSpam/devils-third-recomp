#!/usr/bin/env python3
"""Draws dt-port's application icon (our own art: block letters "dt" on a dark tile with an amber frame, the setup
tool's colours) and writes it as a Windows .ico with 256, 64, 48, 32 and 16 pixel images (PNG-compressed).

    python3 -I src/tools/make_icon.py src/runtime/assets/dt-port.ico
"""
import struct
import sys
import zlib

BG = (20, 20, 22)
FRAME = (232, 163, 58)
LETTER_D = (230, 224, 214)
LETTER_T = (232, 163, 58)


def rounded_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def shape(x, y):
    """Colour at (x, y) in a 1.0 x 1.0 square, or None outside the tile."""
    if not rounded_rect(x, y, 0.02, 0.02, 0.98, 0.98, 0.18):
        return None
    if not rounded_rect(x, y, 0.07, 0.07, 0.93, 0.93, 0.14):
        return FRAME
    # "d": a bowl (ring) and a stem on its right.
    bx, by, ro, ri = 0.36, 0.60, 0.17, 0.085
    d2 = (x - bx) ** 2 + (y - by) ** 2
    if ri * ri <= d2 <= ro * ro and x <= bx + 0.09:
        return LETTER_D
    if bx + 0.085 <= x <= bx + 0.17 and 0.22 <= y <= by + ro:
        return LETTER_D
    # "t": a stem and a cross bar, the stem's foot turning right.
    tx = 0.66
    if tx - 0.04 <= x <= tx + 0.045 and 0.27 <= y <= 0.72:
        return LETTER_T
    if tx - 0.11 <= x <= tx + 0.12 and 0.40 <= y <= 0.47:
        return LETTER_T
    if tx - 0.04 <= x <= tx + 0.15 and 0.70 <= y <= 0.77 and rounded_rect(x, y, tx - 0.04, 0.62, tx + 0.15, 0.77, 0.07):
        return LETTER_T
    return BG


def render(size, ss=4):
    rows = bytearray()
    for py in range(size):
        rows.append(0)
        for px in range(size):
            acc = [0, 0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    c = shape((px + (sx + 0.5) / ss) / size, (py + (sy + 0.5) / ss) / size)
                    if c:
                        acc[0] += c[0]
                        acc[1] += c[1]
                        acc[2] += c[2]
                        acc[3] += 255
            n = ss * ss
            a = acc[3] // n
            if a:
                cov = acc[3] / 255
                rows += bytes((int(acc[0] / cov), int(acc[1] / cov), int(acc[2] / cov), a))
            else:
                rows += b"\0\0\0\0"
    return rows


def png(size, rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rgba), 9)) + chunk(b"IEND", b""))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "dt-port.ico"
    sizes = [256, 64, 48, 32, 16]
    images = [png(s, render(s)) for s in sizes]
    header = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    entries = b""
    for s, data in zip(sizes, images):
        entries += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(out, "wb") as f:
        f.write(header + entries + b"".join(images))
    if len(sys.argv) > 2:  # a preview PNG of the largest size
        with open(sys.argv[2], "wb") as f:
            f.write(images[0])


if __name__ == "__main__":
    main()
