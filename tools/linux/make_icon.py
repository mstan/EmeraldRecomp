#!/usr/bin/env python3
"""Rasterise the Android launcher vector (single source of the app icon) to PNG.

Handles the subset used by android/app/src/main/res/mipmap-anydpi/ic_launcher.xml:
filled paths made of M/L/H/V/Z commands (absolute and relative). 4x4
supersampling; no third-party modules.

    make_icon.py ic_launcher.xml out.png [size]
"""
import re
import struct
import sys
import xml.etree.ElementTree as ET
import zlib

ANDROID = "{http://schemas.android.com/apk/res/android}"


def parse_path(d):
    tokens = re.findall(r"[MLHVZmlhvz]|-?\d*\.?\d+", d)
    polys, cur, x, y, cmd, i = [], [], 0.0, 0.0, None, 0
    while i < len(tokens):
        t = tokens[i]
        if re.fullmatch(r"[MLHVZmlhvz]", t):
            cmd = t
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur = []
            continue
        rel = cmd.islower()
        c = cmd.upper()
        if c in "ML":
            nx, ny = float(tokens[i]), float(tokens[i + 1])
            i += 2
            x, y = (x + nx, y + ny) if rel else (nx, ny)
            if c == "M" and cur:
                polys.append(cur)
                cur = []
            cur.append((x, y))
            cmd = "l" if rel else "L"  # implicit lineto after moveto
        elif c == "H":
            v = float(tokens[i]); i += 1
            x = x + v if rel else v
            cur.append((x, y))
        elif c == "V":
            v = float(tokens[i]); i += 1
            y = y + v if rel else v
            cur.append((x, y))
        else:
            raise ValueError(f"unsupported path command {cmd}")
    if cur:
        polys.append(cur)
    return polys


def inside(poly, px, py):
    hit = False
    n = len(poly)
    for k in range(n):
        x1, y1 = poly[k]
        x2, y2 = poly[(k + 1) % n]
        if (y1 > py) != (y2 > py) and px < (x2 - x1) * (py - y1) / (y2 - y1) + x1:
            hit = not hit
    return hit


def main():
    src, out = sys.argv[1], sys.argv[2]
    size = int(sys.argv[3]) if len(sys.argv) > 3 else 256
    root = ET.parse(src).getroot()
    vw = float(root.get(ANDROID + "viewportWidth"))
    vh = float(root.get(ANDROID + "viewportHeight"))
    layers = []
    for p in root.iter("path"):
        color = p.get(ANDROID + "fillColor").lstrip("#")
        rgb = tuple(int(color[j:j + 2], 16) for j in (0, 2, 4))
        layers.append((parse_path(p.get(ANDROID + "pathData")), rgb))
    ss = 4
    rows = []
    for yy in range(size):
        row = bytearray()
        for xx in range(size):
            acc = [0, 0, 0]
            for sy in range(ss):
                for sx in range(ss):
                    px = (xx + (sx + 0.5) / ss) * vw / size
                    py = (yy + (sy + 0.5) / ss) * vh / size
                    rgb = (0, 0, 0)
                    for polys, color in layers:
                        if any(inside(poly, px, py) for poly in polys):
                            rgb = color
                    for c in range(3):
                        acc[c] += rgb[c]
            row += bytes(v // (ss * ss) for v in acc) + b"\xff"
        rows.append(b"\x00" + bytes(row))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload)))
    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + chunk(b"IEND", b""))
    with open(out, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    main()
