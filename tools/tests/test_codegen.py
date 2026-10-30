"""tools/codegen.py turns the blocks tools/analyze.py finds into C: a function per guest function entered at any of its blocks, one NATIVE_OP per instruction, the
exits back to the dispatcher, a block whose delay slot holds a branch cut before it, and the guest code each function was made from, as merged runs. Every
instruction name tools/mips.py gives has its function in runtime/cpu_ops.h. A BIOS ROM is read from its reset vector, with a capture's RAM (tools/capture.c)
through its mirrors and the addresses it lists. The command line writes the C for an executable. t_native and t_lockstep (runtime/tests) run what it generates
against the interpreter. No game files."""
import re, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import analyze, codegen, mips
from asm import program
from test_exe import make

B = 0x80010000
SRC = '''
start:
    addiu a0, zero, 3
loop:
    addiu a0, a0, -1
    bne a0, zero, loop
    nop
    jal leaf
    .word 0x44000000            ; COP1, in the delay slot
    beq a0, zero, out
    j out                       ; a jump in a delay slot
out:
    jr ra
    nop
leaf:
    jr ra
    nop
'''


class Codegen(unittest.TestCase):
    def setUp(self):
        code, self.labels = program(B, SRC)
        self.word = lambda a: int.from_bytes(code[a - B:a - B + 4], 'little') if B <= a <= B + len(code) - 4 else None
        self.prog = analyze.analyze(self.word, [B])
        self.c = codegen.generate(self.word, self.prog, 'demo')

    def test_functions_and_blocks(self):
        c = self.c
        self.assertIn('static void f80010000(ps1 *m) {', c)
        self.assertIn('static void f80010028(ps1 *m) {', c)
        for b in (0x80010000, 0x80010004, 0x80010010, 0x80010018, 0x80010020):
            self.assertIn(f'    case 0x{b:08x}u: goto b{b:08x};', c)
        self.assertIn('    NATIVE_OP(addiu, 0x80010004u, 0x2484ffffu);   // addiu a0, a0, -1', c)
        self.assertIn('    NATIVE_OP(cop, 0x80010014u, 0x44000000u);   // .word 0x44000000', c, 'COP1 is the runtime\'s cop')
        self.assertIn('    goto b80010004;', c, 'the first block falls into the loop')
        self.assertIn('    if (NATIVE_GO) switch (m->cpu.pc) { case 0x80010004u: goto b80010004; case 0x80010010u: goto b80010010; }', c)
        self.assertIn('    {0x80010000u, 0},\n    {0x80010004u, 0},', c, 'the entries, sorted, with their function')
        self.assertIn('    {0x80010028u, 1},', c)

    def test_delay_slot_branch_is_cut(self):
        body = self.c.split('b80010018:')[1].split('\nb')[0]
        self.assertIn('NATIVE_OP(beq, 0x80010018u', body)
        self.assertNotIn('NATIVE_OP(j,', body, 'the jump in the delay slot is left to the interpreter')

    def test_runs(self):
        self.assertEqual(codegen.runs(self.prog.functions[B]), [[0x80010000, 0x80010028]])
        self.assertIn('static const uint32_t r80010028[][2] = {{0x80010028u, 0x80010030u}};', self.c)

    def test_names_match_the_runtime(self):
        listed = re.search(r'#define CPU_OPS\(X\)(.*?)\nenum', (ROOT / 'runtime' / 'cpu_ops.h').read_text(), re.S)
        self.assertIsNotNone(listed, 'runtime/cpu_ops.h lists its instructions in CPU_OPS')
        names = {x[2:-1] for x in (listed.group(1) if listed else '').replace('\\', ' ').split() if x.startswith('X(')}
        decoded = {'cop' if n in mips.COP else n for n in set(mips.SPECIAL.values()) | set(mips.PRIMARY.values()) | mips.BRANCHES |
                   {'mfc0', 'mtc0', 'rfe', 'cop0', 'cop2', 'mfc2', 'cfc2', 'mtc2', 'ctc2', 'ri'}}
        self.assertEqual(decoded, names)

    def test_bios_and_capture(self):   # a ROM whose reset code jumps into RAM through a register, as OpenBIOS enters its kernel, and a capture of that RAM
        tmp = Path(tempfile.mkdtemp())
        try:
            rom = bytearray(codegen.ROM); rom[:12] = program(0xbfc00000, 'lui t0, 0x8001\njr t0\nnop')[0]
            ram = bytearray(codegen.RAM); ram[0x10000:0x10008] = program(0x80010000, 'jr ra\nnop')[0]
            (tmp / 'rom.bin').write_bytes(rom); (tmp / 'cap.ram').write_bytes(ram); (tmp / 'cap.txt').write_text('80010000\n90000000\n')
            word, entries = codegen.load(tmp / 'rom.bin', bios=True, capture=tmp / 'cap')
            self.assertEqual(entries, [0xbfc00000, 0x80010000], 'the reset vector, and the captured addresses that hold code')
            self.assertEqual([word(a) for a in (0x80010000, 0xa0010000, 0x00010000, 0x80210000, 0x9fc00000)], [0x03e00008, 0x03e00008, 0x03e00008, 0x03e00008, word(0xbfc00000)],
                             'RAM through KSEG0, KSEG1, KUSEG and its mirror at 2 MB; the ROM through KSEG0')
            prog = analyze.analyze(word, entries)
            self.assertEqual(sorted(prog.functions), [0x80010000, 0xbfc00000])
            self.assertEqual(prog.functions[0xbfc00000].blocks[0xbfc00000].call, 0x80010000, 'the jump into RAM: a tail call to a constant')
            (tmp / 'short.ram').write_bytes(b'\x00' * 16); (tmp / 'short.txt').write_text('')
            with self.assertRaisesRegex(SystemExit, 'not 2 MB of RAM'): codegen.load(tmp / 'rom.bin', bios=True, capture=tmp / 'short')
            with self.assertRaisesRegex(SystemExit, 'not a 512 KB BIOS'): codegen.load(tmp / 'cap.ram', bios=True)
        finally: shutil.rmtree(tmp)

    def test_command_line(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            (tmp / 'a.exe').write_bytes(make(program(B, SRC)[0]))
            r = subprocess.run([sys.executable, ROOT / 'tools' / 'codegen.py', tmp / 'a.exe', '--name', 'demo', '-o', tmp / 'gen' / 'demo.c'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.strip(), f'codegen: 2 functions, 6 blocks into {tmp / "gen" / "demo.c"}; 1 notes')
            self.assertIn('const ps1_native_set native_demo = {"demo", fns, 2, entries, 6};', (tmp / 'gen' / 'demo.c').read_text())
            r = subprocess.run([sys.executable, ROOT / 'tools' / 'codegen.py', tmp / 'a.exe', '--name', '9x', '-o', tmp / 'x.c'], capture_output=True, text=True)
            self.assertIn('not a C name', r.stderr)
        finally: shutil.rmtree(tmp)


if __name__ == '__main__': unittest.main()
