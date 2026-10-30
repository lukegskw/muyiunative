#!/usr/bin/env python3
# The R3000A's MIPS I instructions for the tools (plan.md, section 2: the analyzer and the code generator; the decision log: the tools' own decoder): what a 32-bit
# word is, which registers it reads and writes, and where a branch or jump goes. psx-spx, CPU Specifications (CPU Opcode Encoding and the opcode chapters). Words
# are decoded as runtime/interp.c runs them, the reference (AGENTS.md): BcondZ by bits 0 and 4 of rt, RFE by its function field with bit 25 set, and every encoding
# the interpreter takes as a reserved instruction is 'ri'. What COP1, COP3 and LWC/SWC 0, 1 and 3 do depends on SR when they run ('cop'), as does whether COP2 is
# usable. A load's register and those of MFC0, MFC2 and CFC2 are written one instruction late (the load delay slot)
from dataclasses import dataclass

REGS = ('zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
        's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra')
SPECIAL = {0x00: 'sll', 0x02: 'srl', 0x03: 'sra', 0x04: 'sllv', 0x06: 'srlv', 0x07: 'srav', 0x08: 'jr', 0x09: 'jalr', 0x0c: 'syscall', 0x0d: 'break',
           0x10: 'mfhi', 0x11: 'mthi', 0x12: 'mflo', 0x13: 'mtlo', 0x18: 'mult', 0x19: 'multu', 0x1a: 'div', 0x1b: 'divu', 0x20: 'add', 0x21: 'addu',
           0x22: 'sub', 0x23: 'subu', 0x24: 'and', 0x25: 'or', 0x26: 'xor', 0x27: 'nor', 0x2a: 'slt', 0x2b: 'sltu'}
PRIMARY = {0x02: 'j', 0x03: 'jal', 0x04: 'beq', 0x05: 'bne', 0x06: 'blez', 0x07: 'bgtz', 0x08: 'addi', 0x09: 'addiu', 0x0a: 'slti', 0x0b: 'sltiu',
           0x0c: 'andi', 0x0d: 'ori', 0x0e: 'xori', 0x0f: 'lui', 0x11: 'cop1', 0x13: 'cop3', 0x20: 'lb', 0x21: 'lh', 0x22: 'lwl', 0x23: 'lw', 0x24: 'lbu',
           0x25: 'lhu', 0x26: 'lwr', 0x28: 'sb', 0x29: 'sh', 0x2a: 'swl', 0x2b: 'sw', 0x2e: 'swr', 0x30: 'lwc0', 0x31: 'lwc1', 0x32: 'lwc2', 0x33: 'lwc3',
           0x38: 'swc0', 0x39: 'swc1', 0x3a: 'swc2', 0x3b: 'swc3'}
LOADS = {'lb', 'lh', 'lwl', 'lw', 'lbu', 'lhu', 'lwr'}
STORES = {'sb', 'sh', 'swl', 'sw', 'swr'}
BRANCHES = {'beq', 'bne', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal', 'bc2f', 'bc2t'}
JUMPS = {'j', 'jal', 'jr', 'jalr'}
DELAYED = LOADS | {'mfc0', 'mfc2', 'cfc2'}
COP = {'cop1', 'cop3', 'lwc0', 'lwc1', 'lwc3', 'swc0', 'swc1', 'swc3'}
SHIFTS = {'sll', 'srl', 'sra'}
SHIFTV = {'sllv', 'srlv', 'srav'}
RALU = {'add', 'addu', 'sub', 'subu', 'and', 'or', 'xor', 'nor', 'slt', 'sltu'}
IALU = {'addi', 'addiu', 'slti', 'sltiu', 'andi', 'ori', 'xori'}
MULDIV = {'mult', 'multu', 'div', 'divu'}


@dataclass(frozen=True)
class Op:
    word: int; name: str

    rs = property(lambda self: self.word >> 21 & 31)
    rt = property(lambda self: self.word >> 16 & 31)
    rd = property(lambda self: self.word >> 11 & 31)
    sa = property(lambda self: self.word >> 6 & 31)
    imm = property(lambda self: self.word & 0xffff)
    simm = property(lambda self: (self.word & 0xffff) - ((self.word & 0x8000) << 1))

    @property
    def delay_slot(self): return self.name in BRANCHES or self.name in JUMPS   # the next instruction runs before control moves

    def target(self, pc):   # where a branch or J/JAL at pc goes (from the delay slot's address, as the interpreter counts); None for JR/JALR and the rest
        if self.name in BRANCHES: return (pc + 4 + (self.simm << 2)) & 0xffffffff
        if self.name in ('j', 'jal'): return ((pc + 4) & 0xf0000000) | (self.word & 0x3ffffff) << 2
        return None

    @property
    def reads(self):   # the general registers it reads (r0 left out: it is always 0)
        n, s, t = self.name, self.rs, self.rt
        if n in SHIFTS or n in ('mtc0', 'mtc2', 'ctc2'): r = {t}
        elif n in SHIFTV or n in RALU or n in MULDIV or n in STORES or n in ('beq', 'bne'): r = {s, t}
        elif n in ('jr', 'jalr', 'mthi', 'mtlo', 'blez', 'bgtz', 'bltz', 'bgez', 'bltzal', 'bgezal', 'lwc2', 'swc2') or n in IALU: r = {s}
        elif n in ('lwl', 'lwr'): r = {s, t}   # the unaligned loads merge into rt
        elif n in LOADS: r = {s}
        else: r = set()
        return frozenset(r - {0})

    @property
    def writes(self):   # the general registers it writes (r0 left out); those of DELAYED land after the next instruction
        n = self.name
        if n in SHIFTS or n in SHIFTV or n in RALU or n in ('mfhi', 'mflo', 'jalr'): r = {self.rd}
        elif n in IALU or n in LOADS or n in ('lui', 'mfc0', 'mfc2', 'cfc2'): r = {self.rt}
        elif n in ('jal', 'bltzal', 'bgezal'): r = {31}   # the link is written whether or not the branch is taken
        else: r = set()
        return frozenset(r - {0})

    def text(self, pc=None):   # assembly, with a branch or jump's target when pc is given
        n, w, s, t, d = self.name, self.word, REGS[self.rs], REGS[self.rt], REGS[self.rd]
        to = lambda: f'{self.target(pc):08x}' if pc is not None else (f'{self.simm << 2:+d}' if n in BRANCHES else f'{(w & 0x3ffffff) << 2:07x}')
        if w == 0: return 'nop'   # SLL r0, r0, 0
        if n in SHIFTS: return f'{n} {d}, {t}, {self.sa}'
        if n in SHIFTV: return f'{n} {d}, {t}, {s}'
        if n in RALU: return f'{n} {d}, {s}, {t}'
        if n in MULDIV: return f'{n} {s}, {t}'
        if n in ('mfhi', 'mflo'): return f'{n} {d}'
        if n in ('mthi', 'mtlo', 'jr'): return f'{n} {s}'
        if n == 'jalr': return f'jalr {s}' if self.rd == 31 else f'jalr {d}, {s}'
        if n in ('syscall', 'break'): return n if not w >> 6 & 0xfffff else f'{n} 0x{w >> 6 & 0xfffff:x}'
        if n in ('beq', 'bne'): return f'{n} {s}, {t}, {to()}'
        if n in BRANCHES - {'bc2f', 'bc2t'}: return f'{n} {s}, {to()}'
        if n in ('bc2f', 'bc2t', 'j', 'jal'): return f'{n} {to()}'
        if n in ('andi', 'ori', 'xori'): return f'{n} {t}, {s}, 0x{self.imm:x}'
        if n in IALU: return f'{n} {t}, {s}, {self.simm}'
        if n == 'lui': return f'lui {t}, 0x{self.imm:x}'
        if n in LOADS or n in STORES: return f'{n} {t}, {self.simm}({s})'
        if n in ('lwc2', 'swc2'): return f'{n} ${self.rt}, {self.simm}({s})'
        if n in ('mfc0', 'mtc0', 'mfc2', 'cfc2', 'mtc2', 'ctc2'): return f'{n} {t}, ${self.rd}'
        if n == 'rfe': return 'rfe'
        if n in ('cop0', 'cop2'): return f'{n} 0x{w & 0x1ffffff:07x}'
        return f'.word 0x{w:08x}'   # a reserved instruction, or COP1/COP3 and their loads and stores


def decode(word):
    word &= 0xffffffff
    p = word >> 26
    if p == 0x00: return Op(word, SPECIAL.get(word & 0x3f, 'ri'))
    if p == 0x01: rt = word >> 16 & 31; return Op(word, ('bgez' if rt & 1 else 'bltz') + ('al' if (rt & 0x1e) == 0x10 else ''))
    if p == 0x10:   # COP0: MFC0/MTC0 by rs, RFE; other commands with bit 25 set do nothing; the rest is reserved
        rs = word >> 21 & 31
        if rs in (0, 4): return Op(word, 'mfc0' if rs == 0 else 'mtc0')
        return Op(word, ('rfe' if (word & 0x3f) == 0x10 else 'cop0') if word >> 25 & 1 else 'ri')
    if p == 0x12:   # COP2, the GTE: a command with bit 25 set, else the register moves and BC2F/BC2T (rt 0 or 1)
        if word >> 25 & 1: return Op(word, 'cop2')
        rs, rt = word >> 21 & 31, word >> 16 & 31
        if rs == 8: return Op(word, ('bc2f', 'bc2t')[rt] if rt <= 1 else 'ri')
        return Op(word, {0: 'mfc2', 2: 'cfc2', 4: 'mtc2', 6: 'ctc2'}.get(rs, 'ri'))
    return Op(word, PRIMARY.get(p, 'ri'))
