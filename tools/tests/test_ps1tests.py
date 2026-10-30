"""tools/ps1tests.py reads a VRAM picture as the release of ps1-tests stores it (RGB, RGBA and 4- and 8-bit palette PNGs, every row filter) into 15-bit pixels,
hashes an area of it with the areas to leave out the way runtime/tests/t_ps1tests.c hashes the runtime's VRAM, and compares a VRAM dump with it, writing a picture
of the difference. Also: every program and log in tests/ps1-tests is one t_ps1tests.c runs, and the other way round. The pictures are made here; no game files."""
import re, shutil, struct, subprocess, sys, tempfile, unittest, zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / 'tools' / 'ps1tests.py'
W, H = 1024, 512


def png(path, rows, ctype, depth=8, palette=b''):   # rows: each row's raw bytes; the filter cycles through all five types, row by row
    bpp = {2: 3, 6: 4, 3: 1}[ctype]; out = b''; prev = bytes(len(rows[0]))
    for y, row in enumerate(rows):
        f = y % 5; enc = bytearray()
        for x, v in enumerate(row):
            a = row[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
            pred = (0, a, b, (a + b) // 2, a if pa <= pb and pa <= pc else b if pb <= pc else c)[f]
            enc.append((v - pred) & 255)
        out += bytes([f]) + enc; prev = row
    def chunk(kind, body): return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    body = chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, depth, ctype, 0, 0, 0)) + (chunk(b'PLTE', palette) if palette else b'')
    Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + body + chunk(b'IDAT', zlib.compress(out)) + chunk(b'IEND', b''))


def pixel(x, y): return (x * 7 + y * 13) % 32 | (y % 32) << 5 | (x % 32) << 10


def fnv(pixels):
    h = 0xcbf29ce484222325
    for p in pixels: h = ((h ^ p) * 0x100000001b3) & 0xffffffffffffffff
    return h


class Ps1Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.tmp = Path(tempfile.mkdtemp())

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def tool(self, *args): return subprocess.run([sys.executable, TOOL, *map(str, args)], capture_output=True, text=True)

    def test_formats_and_hash(self):
        rgb = [bytes(v for x in range(W) for v in ((pixel(x, y) & 31) << 3, (pixel(x, y) >> 5 & 31) << 3, (pixel(x, y) >> 10) << 3)) for y in range(H)]
        rgba = [bytes(v for x in range(W) for v in (*row[x * 3:x * 3 + 3], 255)) for row in rgb]
        png(self.tmp / 'rgb.png', rgb, 2); png(self.tmp / 'rgba.png', rgba, 6)
        whole = fnv(pixel(x, y) for y in range(H) for x in range(W))
        for name in ('rgb.png', 'rgba.png'):
            r = self.tool('hash', self.tmp / name); self.assertEqual(r.returncode, 0, r.stderr); self.assertEqual(r.stdout.strip(), f'{whole:016x}', name)
        part = fnv(pixel(x, y) for y in range(10, 30) for x in range(5, 45) if not (20 <= x < 25 and 12 <= y < 14))
        r = self.tool('hash', self.tmp / 'rgb.png', '--area', '5,10,40,20', '--skip', '20,12,5,2'); self.assertEqual(r.stdout.strip(), f'{part:016x}')
        palette = bytes(v for i in range(16) for v in (i * 8, 0, 248 - i * 8))
        nibbles = [bytes((x * 2 + y) % 16 << 4 | (x * 2 + 1 + y) % 16 for x in range(W // 2)) for y in range(H)]
        png(self.tmp / 'pal4.png', nibbles, 3, 4, palette)
        r = self.tool('hash', self.tmp / 'pal4.png'); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), f'{fnv((x + y) % 16 | (31 - (x + y) % 16) << 10 for y in range(H) for x in range(W)):016x}', '4-bit palette')

    def test_diff_and_refusals(self):
        rows = [bytes(v for x in range(W) for v in (8 * (x % 32), 0, 0)) for y in range(H)]
        png(self.tmp / 'console.png', rows, 2)
        vram = bytearray(struct.pack(f'<{W * H}H', *(0x8000 | x % 32 for y in range(H) for x in range(W))))   # the mask bit does not count
        struct.pack_into('<H', vram, (3 * W + 4) * 2, 0x7c00)
        (self.tmp / 'dump.vram').write_bytes(vram)
        r = self.tool('diff', self.tmp / 'console.png', self.tmp / 'dump.vram', '--png', self.tmp / 'diff.png')
        self.assertEqual(r.returncode, 1); self.assertIn('1 pixels differ', r.stdout); self.assertIn('(4,3): console 0004, runtime 7c00', r.stdout)
        self.assertEqual((self.tmp / 'diff.png').read_bytes()[16:24], struct.pack('>II', 3 * W, H))
        r = self.tool('diff', self.tmp / 'console.png', self.tmp / 'dump.vram', '--skip', '4,3,1,1'); self.assertEqual(r.returncode, 0, r.stdout)
        self.assertNotEqual(self.tool('hash', self.tmp / 'dump.vram').returncode, 0, 'a dump is not a PNG')
        self.assertNotEqual(self.tool('hash', self.tmp / 'console.png', '--area', '1000,0,100,10').returncode, 0, 'an area outside VRAM')

    def test_programs_match_the_folder(self):
        listed = set(re.findall(r'"([\w/.-]+\.(?:exe|log))"', (ROOT / 'runtime/tests/t_ps1tests.c').read_text()))
        folder = ROOT / 'tests/ps1-tests'
        present = {str(p.relative_to(folder)) for p in folder.rglob('*') if p.suffix in ('.exe', '.log')}
        self.assertEqual(listed, present)


if __name__ == '__main__': unittest.main()
