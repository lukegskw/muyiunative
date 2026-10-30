"""tools/exe.py reads a PS-X EXE built here: its header's PC, GP, destination, BSS and stack, the region's marker, and the words of its code; refused when it is
not one, or its code or entry would not be in main RAM; and read from a disc that boots it (built as in test_disc.py). No game files."""
import shutil, struct, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import exe
from test_disc import volume

MARKER = b'Sony Computer Entertainment Inc. for North America area'


def make(code=b'\x08\x00\xe0\x03' + b'\x00' * 4, pc=0x80010000, addr=0x80010000, size=None, sp=0x801fff00, offset=0x100, marker=MARKER):   # a PS-X EXE
    h = b'PS-X EXE' + b'\x00' * 8 + struct.pack('<10I', pc, 0x8001f000, addr, len(code) if size is None else size, 0, 0, 0x80020000, 0x1000, sp, offset)
    return (h.ljust(0x4c, b'\x00') + marker).ljust(0x800, b'\x00') + code


class Exe(unittest.TestCase):
    def test_header(self):
        x = exe.parse(make())
        self.assertEqual((x.pc, x.gp, x.addr, x.end, x.bss, x.bss_size, x.sp), (0x80010000, 0x8001f000, 0x80010000, 0x80010008, 0x80020000, 0x1000, 0x80200000))
        self.assertEqual(x.marker, MARKER.decode())
        self.assertEqual([x.word(a) for a in (0x80010000, 0x80010004, 0x80010008, 0x8000fffc, 0x80010002)], [0x03e00008, 0, None, None, None])
        self.assertEqual(exe.parse(make(sp=0, marker=b'')).sp, 0, 'SP base 0: the stack is left as it is')
        self.assertEqual(exe.parse(make(addr=0xa0010000, pc=0x00010000)).addr, 0xa0010000, 'KSEG1 and KUSEG are main RAM too')

    def test_refused(self):
        for data, why in [(b'PS-X EXF' + make()[8:], 'not a PS-X EXE'), (make()[:0x700], 'not a PS-X EXE'), (make(size=0x1000), 'not in main RAM'),
                          (make(addr=0x801ffffc), 'not in main RAM'), (make(pc=0x1fc00000), 'not in main RAM'), (make(addr=0x1f800000), 'not in main RAM')]:
            with self.subTest(why), self.assertRaisesRegex(SystemExit, why): exe.parse(data, 'x.exe')

    def test_load(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            (tmp / 'a.exe').write_bytes(make(pc=0x80010004)); (tmp / 'disc.bin').write_bytes(volume(exe=make(pc=0x80010004)))
            self.assertEqual(exe.load(tmp / 'a.exe').pc, 0x80010004)
            self.assertEqual(exe.load(tmp / 'disc.bin').pc, 0x80010004, 'the executable SYSTEM.CNF boots')
        finally: shutil.rmtree(tmp)


if __name__ == '__main__': unittest.main()
