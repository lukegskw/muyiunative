"""Phase-4 capture parsing and content-keyed code generation: transient versions survive final RAM, duplicate addresses compile independently, and malformed records
fail before generating native code. No game files: all inputs are assembled here; runtime/tests/t_overlays.c executes the generated versions against the interpreter."""
import hashlib, shutil, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import analyze, codegen, overlays
from asm import program
from test_exe import make

B = 0x80010000


def capture(blocks):
    return b'PS1BLK1\0' + b''.join(struct.pack('<IIQ', b.pc, len(b.code) // 4, b.hits) + b.code for b in blocks)


class Overlays(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.path = self.tmp / 'cap.blocks'
        self.blocks = [overlays.Block(B, program(B, f'addiu v0, zero, {n}\njr ra\nnop')[0], hits) for n, hits in ((11, 5), (22, 7))]
        self.path.write_bytes(capture(self.blocks))

    def tearDown(self): shutil.rmtree(self.tmp)

    def test_versions(self):
        blocks = overlays.read(self.path)
        self.assertEqual({b.digest for b in blocks}, {hashlib.sha256(b.code).hexdigest() for b in self.blocks})
        r = overlays.report(blocks)
        self.assertEqual((r['blocks'], r['replaced_addresses'], r['instructions']), (2, 1, 36))
        p = overlays.extend(analyze.Program({}, []), blocks)
        text = codegen.generate(lambda _: 0, p, 'test')   # final RAM has lost both versions
        for b in blocks:
            self.assertIn(f'f{B:08x}_{b.digest}', text)
            self.assertIn(f'0x{b.word(B):08x}u', text)
        self.assertEqual(text.count('    {0x80010000u,'), 2, 'the same address has two independently guarded entries')

    def test_malformed(self):
        valid = capture(self.blocks)
        for data in (b'', b'WRONG123', valid[:-1], valid + b'x', capture([self.blocks[0], self.blocks[0]]),
                     b'PS1BLK1\0' + struct.pack('<IIQ', B, 0, 1), b'PS1BLK1\0' + struct.pack('<IIQ', B + 1, 3, 1),
                     b'PS1BLK1\0' + struct.pack('<IIQ', B, 259, 1), b'PS1BLK1\0' + struct.pack('<IIQ', B, 3, 0)):
            with self.subTest(data=data[:24]):
                self.path.write_bytes(data)
                with self.assertRaises(ValueError): overlays.read(self.path)

    def test_cli_preserves_transient_code(self):
        (self.tmp / 'a.exe').write_bytes(make(self.blocks[1].code))
        ram = bytearray(codegen.RAM); ram[B & 0x1fffff:(B & 0x1fffff) + 12] = self.blocks[1].code
        (self.tmp / 'cap.ram').write_bytes(ram); (self.tmp / 'cap.txt').write_text(f'{B:08x}\n')
        r = subprocess.run([sys.executable, ROOT / 'tools' / 'codegen.py', self.tmp / 'a.exe', '--capture', self.tmp / 'cap', '--name', 'game',
                            '-o', self.tmp / 'game.c', '--max-ops', '4'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        text = ''.join(p.read_text() for p in self.tmp.glob('game*.c'))
        for b in self.blocks: self.assertIn(b.digest, text)


if __name__ == '__main__': unittest.main()
