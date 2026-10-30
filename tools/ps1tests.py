#!/usr/bin/env python3
# The VRAM side of the ps1-tests checks (tests/ps1-tests/README.md): the hash of a console's VRAM picture from the suite's release, the way
# runtime/tests/t_ps1tests.c hashes the runtime's VRAM, and a picture of where a dump of the runtime's VRAM differs from it. Pictures stay outside the repository;
# only hashes go into t_ps1tests.c (AGENTS.md). A pixel is its 15 colour bits (the pictures lack the mask bit). The hash is 64-bit FNV-1a over an area's pixels,
# row by row, one step per pixel (hash ^= pixel, then hash *= the FNV prime), leaving out the areas given with --skip.
# Usage: ps1tests.py hash <vram.png> [--area x,y,w,h] [--skip x,y,w,h]...
#        ps1tests.py diff <vram.png> <dump.vram> [--area x,y,w,h] [--skip x,y,w,h]... [--png <out.png>]
# A dump is what t_ps1tests writes with PS1TESTS_DUMP=<folder>: 1024x512 little-endian 16-bit pixels. diff prints the differing pixels and, with --png, writes
# the area three times, side by side: the console's, the runtime's, and the differing pixels in magenta.
import argparse, struct, sys, zlib
from pathlib import Path

W, H = 1024, 512


def png_pixels(path):   # a VRAM picture as W*H 15-bit pixels; 8-bit RGB/RGBA and palette pictures of 1 to 8 bits, the formats the release uses
    d = Path(path).read_bytes()
    if d[:8] != b'\x89PNG\r\n\x1a\n': raise SystemExit(f'{path}: not a PNG')
    i, idat, pal, hdr = 8, b'', b'', None
    while i + 8 <= len(d):
        n, kind = struct.unpack('>I4s', d[i:i + 8]); body = d[i + 8:i + 8 + n]; i += 12 + n
        if kind == b'IHDR': hdr = struct.unpack('>IIBBBBB', body)
        elif kind == b'PLTE': pal = body
        elif kind == b'IDAT': idat += body
    if not hdr: raise SystemExit(f'{path}: no IHDR')
    w, h, depth, ctype, _, _, interlace = hdr
    if (w, h) != (W, H): raise SystemExit(f'{path}: {w}x{h}, not a {W}x{H} VRAM picture')
    if interlace or ctype not in (2, 3, 6) or (ctype != 3 and depth != 8): raise SystemExit(f'{path}: colour type {ctype}, depth {depth}, interlace {interlace}: not read')
    bpp = {2: 3, 6: 4, 3: 1}[ctype]; stride = (w * depth + 7) // 8 if ctype == 3 else w * bpp
    raw = zlib.decompress(idat); prev = bytearray(stride); out = []
    for y in range(h):
        f = raw[y * (stride + 1)]; line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):   # PNG filters, with a left neighbour one pixel (at least one byte) back
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        for x in range(w):
            if ctype == 3:
                k = line[x * depth // 8] >> (8 - depth - x * depth % 8) & ((1 << depth) - 1); r, g, b = pal[k * 3:k * 3 + 3]
            else: r, g, b = line[x * bpp:x * bpp + 3]
            out.append(r >> 3 | (g >> 3) << 5 | (b >> 3) << 10)
    return out


def dump_pixels(path):
    d = Path(path).read_bytes()
    if len(d) != W * H * 2: raise SystemExit(f'{path}: {len(d)} bytes, not a {W}x{H} VRAM dump')
    return [v & 0x7fff for v in struct.unpack(f'<{W * H}H', d)]


def rect(s):
    try: x, y, w, h = (int(v) for v in s.split(','))
    except ValueError: raise argparse.ArgumentTypeError(f'not x,y,w,h: {s}') from None
    if w < 1 or h < 1 or x < 0 or y < 0 or x + w > W or y + h > H: raise argparse.ArgumentTypeError(f'not inside {W}x{H} VRAM: {s}')
    return x, y, w, h


def counted(a, x, y): return not any(sx <= x < sx + sw and sy <= y < sy + sh for sx, sy, sw, sh in a.skip)


def fnv(px, a):
    h, (ax, ay, aw, ah) = 0xcbf29ce484222325, a.area
    for y in range(ay, ay + ah):
        for x in range(ax, ax + aw):
            if counted(a, x, y): h = ((h ^ px[y * W + x]) * 0x100000001b3) & 0xffffffffffffffff
    return h


def write_png(path, w, h, rgb):
    def chunk(kind, body): return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    rows = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def diff(a):
    hw, ours = png_pixels(a.png_in), dump_pixels(a.dump)
    ax, ay, aw, ah = a.area
    bad = [(x, y) for y in range(ay, ay + ah) for x in range(ax, ax + aw) if counted(a, x, y) and hw[y * W + x] != ours[y * W + x]]
    print(f'{len(bad)} pixels differ; hashes: console {fnv(hw, a):016x}, runtime {fnv(ours, a):016x}')
    for x, y in bad[:8]: print(f'  ({x},{y}): console {hw[y * W + x]:04x}, runtime {ours[y * W + x]:04x}')
    if a.png:
        def rgb(c): return bytes(((c & 31) << 3, (c >> 5 & 31) << 3, (c >> 10 & 31) << 3))
        wrong = set(bad); out = bytearray()
        for y in range(ay, ay + ah):
            for panel in range(3):
                for x in range(ax, ax + aw):
                    out += rgb(hw[y * W + x]) if panel == 0 else rgb(ours[y * W + x]) if panel == 1 else b'\xff\0\xff' if (x, y) in wrong else b'\x20\x20\x20'
        write_png(a.png, aw * 3, ah, bytes(out))
    return 1 if bad else 0


def main(argv=None):
    p = argparse.ArgumentParser(description='Hash a console VRAM picture of ps1-tests, or compare a runtime VRAM dump with it')
    sub = p.add_subparsers(dest='cmd', required=True)
    for name in ('hash', 'diff'):
        s = sub.add_parser(name); s.add_argument('png_in', metavar='vram.png')
        if name == 'diff': s.add_argument('dump', metavar='dump.vram'); s.add_argument('--png', help='write the console | runtime | difference picture here')
        s.add_argument('--area', type=rect, default=(0, 0, W, H)); s.add_argument('--skip', type=rect, action='append', default=[])
    a = p.parse_args(argv)
    if a.cmd == 'hash': print(f'{fnv(png_pixels(a.png_in), a):016x}'); return 0
    return diff(a)


if __name__ == '__main__': sys.exit(main())
