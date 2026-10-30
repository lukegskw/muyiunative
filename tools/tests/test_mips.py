"""tools/mips.py decodes each encoding as runtime/interp.c runs it: psx-spx's opcode tables (CPU Opcode Encoding), each encoding they leave empty a reserved
instruction; BcondZ for every rt; COP0 and COP2 by rs. Checked too: the registers each instruction reads and writes, the writes that land one instruction late,
where branches and jumps go (J from its delay slot's 256 MB region), and the assembly it prints, read back by the tests' assembler. No game files."""
import sys, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import mips
from asm import asm

# psx-spx, CPU Opcode Encoding: the primary opcodes (bits 31-26), and SPECIAL's function field (bits 5-0), eight to a group; '-' is N/A
PRIMARY = '''special bcondz j jal beq bne blez bgtz  addi addiu slti sltiu andi ori xori lui  cop0 cop1 cop2 cop3 - - - -  - - - - - - - -
             lb lh lwl lw lbu lhu lwr -  sb sh swl sw - - swr -  lwc0 lwc1 lwc2 lwc3 - - - -  swc0 swc1 swc2 swc3 - - - -'''.split()
SPECIAL = '''sll - srl sra sllv - srlv srav  jr jalr - - syscall break - -  mfhi mthi mflo mtlo - - - -  mult multu div divu - - - -
             add addu sub subu and or xor nor  - - slt sltu - - - -  - - - - - - - -  - - - - - - - -'''.split()
PC = 0x80010000


class Decode(unittest.TestCase):
    def test_opcode_tables(self):
        for p, name in enumerate(PRIMARY):
            if name not in ('special', 'bcondz', 'cop0', 'cop2'):
                with self.subTest(opcode=hex(p)): self.assertEqual(mips.decode(p << 26 | 0x00a51234).name, 'ri' if name == '-' else name)
        for f, name in enumerate(SPECIAL):
            with self.subTest(funct=hex(f)): self.assertEqual(mips.decode(0x00a51000 | f).name, 'ri' if name == '-' else name)

    def test_bcondz(self):   # rt 00h/01h/10h/11h, and the rest as the interpreter takes them: bit 0 GEZ, link only when bits 4-1 are 1000b
        for rt in range(32):
            want = {0: 'bltz', 1: 'bgez', 0x10: 'bltzal', 0x11: 'bgezal'}.get(rt, 'bgez' if rt & 1 else 'bltz')
            with self.subTest(rt=hex(rt)): self.assertEqual(mips.decode(0x04800000 | rt << 16 | 3).name, want)

    def test_coprocessors(self):
        for word, name in [(0x401a6800, 'mfc0'), (0x409a6000, 'mtc0'), (0x42000010, 'rfe'), (0x42000001, 'cop0'), (0x42000008, 'cop0'), (0x40400000, 'ri'),
                           (0x40c00000, 'ri'), (0x41000000, 'ri'), (0x4a180001, 'cop2'), (0x48020000, 'mfc2'), (0x48420000, 'cfc2'), (0x48820000, 'mtc2'),
                           (0x48c20000, 'ctc2'), (0x49000000, 'bc2f'), (0x49010000, 'bc2t'), (0x49020000, 'ri'), (0x48200000, 'ri'), (0x44000000, 'cop1'),
                           (0x4c000000, 'cop3'), (0xc8000000, 'lwc2'), (0xe8000000, 'swc2'), (0xc4000000, 'lwc1'), (0xec000000, 'swc3')]:
            with self.subTest(word=hex(word)): self.assertEqual(mips.decode(word).name, name)

    def test_registers(self):
        for text, reads, writes, delayed in [('addu v0, a0, a1', {4, 5}, {2}, False), ('addu zero, a0, zero', {4}, set(), False), ('sll v0, a0, 2', {4}, {2}, False),
                                             ('srav t0, t1, t2', {9, 10}, {8}, False), ('lw v0, 0(a0)', {4}, {2}, True), ('lwl v0, 3(a0)', {4, 2}, {2}, True),
                                             ('lbu t0, -1(sp)', {29}, {8}, True), ('sw ra, 16(sp)', {29, 31}, set(), False), ('lui at, 0x8001', set(), {1}, False),
                                             ('ori at, at, 0x10', {1}, {1}, False), ('mult a0, a1', {4, 5}, set(), False), ('mflo v0', set(), {2}, False),
                                             ('mthi a3', {7}, set(), False), ('jal 80010000', set(), {31}, False), ('bltzal a0, +8', {4}, {31}, False),
                                             ('beq a0, a1, +8', {4, 5}, set(), False), ('jr ra', {31}, set(), False), ('jalr t0', {8}, {31}, False),
                                             ('jalr a0, t0', {8}, {4}, False), ('mfc0 k0, $13', set(), {26}, True), ('mtc0 k0, $12', {26}, set(), False),
                                             ('mfc2 v0, $25', set(), {2}, True), ('cfc2 v0, $31', set(), {2}, True), ('ctc2 t0, $31', {8}, set(), False),
                                             ('lwc2 $12, 0(a0)', {4}, set(), False), ('swc2 $25, 4(sp)', {29}, set(), False), ('syscall', set(), set(), False)]:
            with self.subTest(text):
                op = mips.decode(asm(text, PC))
                self.assertEqual((set(op.reads), set(op.writes), op.name in mips.DELAYED), (reads, writes, delayed))

    def test_targets(self):
        self.assertEqual(mips.decode(asm('beq zero, zero, -4')).target(PC), PC, 'from the delay slot: -4 is the branch itself')
        self.assertEqual(mips.decode(asm('bgez a0, +0x100')).target(PC), PC + 0x104)
        self.assertEqual(mips.decode(asm('j 0000040')).target(0x8ffffffc), 0x90000040, 'J keeps the top bits of its delay slot\'s address')
        self.assertEqual(mips.decode(asm('jal 0000040')).target(PC), 0x80000040)
        self.assertIsNone(mips.decode(asm('jr ra')).target(PC))
        self.assertTrue(mips.decode(asm('bc2f +8')).delay_slot and mips.decode(asm('jalr t0')).delay_slot and not mips.decode(asm('syscall')).delay_slot)

    def test_text(self):   # what the assembler reads back, and a few encodings any MIPS assembler gives
        for text in ['nop', 'sll v0, a0, 2', 'srl t0, t0, 31', 'sra v1, v1, 16', 'srav t0, t1, t2', 'addu v0, a0, a1', 'nor t0, zero, t1', 'slt at, s0, s1',
                     'sltu v0, zero, a0', 'mult a0, a1', 'divu t0, t1', 'mfhi v0', 'mtlo a3', 'jr ra', 'jalr t9', 'jalr a0, t9', 'syscall', 'break 0x7',
                     'beq a0, a1, 80010100', 'bne zero, v0, 8000fff0', 'blez a0, 80010008', 'bgtz s0, 80010004', 'bltz a0, 80010000',
                     'bgezal zero, 80020000', 'bc2t 80010010', 'j 80012340', 'jal 8001fffc', 'addi t0, t0, 1', 'addiu sp, sp, -32', 'slti v0, a0, -1',
                     'sltiu v0, a0, 7', 'andi v0, v0, 0xff', 'ori at, at, 0x8000', 'xori a0, a0, 0x1', 'lui at, 0x8001', 'lb v0, -1(a0)', 'lh v0, 2(a0)',
                     'lhu v1, 2(sp)', 'lwl t0, 3(a0)', 'lw gp, -32768(gp)', 'lwr t0, 0(a0)', 'sb zero, 0(v0)', 'sh t0, 32767(t1)', 'swl t0, 3(a1)',
                     'sw ra, 16(sp)', 'swr t0, 0(a1)', 'lwc2 $12, 0(a0)', 'swc2 $25, 4(sp)', 'mfc0 k0, $13', 'mtc0 zero, $12', 'rfe', 'mfc2 v0, $25',
                     'cfc2 v0, $31', 'mtc2 t0, $0', 'ctc2 t0, $31', 'cop2 0x0180001', 'cop0 0x0000001', '.word 0x5c000000', '.word 0x44000000']:
            with self.subTest(text): self.assertEqual(mips.decode(asm(text, PC)).text(PC), text)
        for text, word in [('addiu sp, sp, -32', 0x27bdffe0), ('jr ra', 0x03e00008), ('sw ra, 16(sp)', 0xafbf0010), ('lui at, 0x8001', 0x3c018001),
                           ('jal 80010000', 0x0c004000), ('syscall', 0x0000000c), ('mfc0 k0, $13', 0x401a6800), ('rfe', 0x42000010)]:
            with self.subTest(text): self.assertEqual(asm(text, PC), word)
        self.assertEqual(mips.decode(asm('bne a0, zero, -8')).text(), 'bne a0, zero, -8', 'without an address: the offset')


if __name__ == '__main__': unittest.main()
