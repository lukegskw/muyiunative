"""A MIPS I assembler for the tool tests: the syntax tools/mips.py prints (Op.text), with labels ('name:'), %hi(label) and %lo(label), and ';' comments. Programs
are written in the tests, so no game code is needed."""
import re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import mips

FUNCT = {v: k for k, v in mips.SPECIAL.items()}
OPCODE = {v: k for k, v in mips.PRIMARY.items()}
REG = {n: i for i, n in enumerate(mips.REGS)}


def reg(x): return int(x[1:]) if x.startswith('$') else REG[x]


def asm(line, pc=None):   # one instruction; a branch's target is an address (hex) when pc is given, else a byte offset (+n, -n)
    n, _, rest = line.strip().partition(' ')
    a = [x.strip() for x in rest.split(',')] if rest else []
    R = lambda f, rs=0, rt=0, rd=0, sa=0: rs << 21 | rt << 16 | rd << 11 | sa << 6 | f
    I = lambda o, rs, rt, imm: o << 26 | rs << 21 | rt << 16 | imm & 0xffff

    def off(x):
        if x[0] in '+-': return int(x, 0) >> 2
        if pc is None: raise ValueError(f'asm: {line}: a target address needs the instruction\'s')
        return (int(x, 16) - pc - 4) >> 2

    def mem(x):
        m = re.fullmatch(r'(-?\w+)\((\$?\w+)\)', x)
        if not m: raise ValueError(f'asm: {line}: not offset(register)')
        return int(m[1], 0), reg(m[2])
    if n == '.word': return int(a[0], 0)
    if n == 'nop': return 0
    if n in mips.SHIFTS: return R(FUNCT[n], 0, reg(a[1]), reg(a[0]), int(a[2], 0))
    if n in mips.SHIFTV: return R(FUNCT[n], reg(a[2]), reg(a[1]), reg(a[0]))
    if n in mips.RALU: return R(FUNCT[n], reg(a[1]), reg(a[2]), reg(a[0]))
    if n in mips.MULDIV: return R(FUNCT[n], reg(a[0]), reg(a[1]))
    if n in ('mfhi', 'mflo'): return R(FUNCT[n], rd=reg(a[0]))
    if n in ('mthi', 'mtlo', 'jr'): return R(FUNCT[n], reg(a[0]))
    if n == 'jalr': return R(FUNCT[n], reg(a[-1]), 0, reg(a[0]) if len(a) == 2 else 31)
    if n in ('syscall', 'break'): return R(FUNCT[n]) | (int(a[0], 0) if a else 0) << 6
    if n in ('beq', 'bne'): return I(OPCODE[n], reg(a[0]), reg(a[1]), off(a[2]))
    if n in ('blez', 'bgtz'): return I(OPCODE[n], reg(a[0]), 0, off(a[1]))
    if n in ('bltz', 'bgez', 'bltzal', 'bgezal'): return I(1, reg(a[0]), {'bltz': 0, 'bgez': 1, 'bltzal': 16, 'bgezal': 17}[n], off(a[1]))
    if n in ('bc2f', 'bc2t'): return I(0x12, 8, n == 'bc2t', off(a[0]))
    if n in ('j', 'jal'): return OPCODE[n] << 26 | int(a[0], 16) >> 2 & 0x3ffffff
    if n in mips.IALU: return I(OPCODE[n], reg(a[1]), reg(a[0]), int(a[2], 0))
    if n == 'lui': return I(OPCODE[n], 0, reg(a[0]), int(a[1], 0))
    if n in mips.LOADS or n in mips.STORES or n in ('lwc2', 'swc2'): o, b = mem(a[1]); return I(OPCODE[n], b, reg(a[0]), o)
    if n in ('mfc0', 'mtc0', 'mfc2', 'cfc2', 'mtc2', 'ctc2'):
        return (0x10 if n[-1] == '0' else 0x12) << 26 | {'mf': 0, 'cf': 2, 'mt': 4, 'ct': 6}[n[:2]] << 21 | reg(a[0]) << 16 | reg(a[1]) << 11
    if n == 'rfe': return 0x42000010
    if n in ('cop0', 'cop2'): return (0x10 if n == 'cop0' else 0x12) << 26 | 1 << 25 | int(a[0], 0)
    raise ValueError(f'asm: {line}')


def program(base, src):   # a program's bytes at base, and its labels' addresses
    lines, labels, pc = [], {}, base
    for s in (x.split(';')[0].strip() for x in src.splitlines()):
        if s.endswith(':'): labels[s[:-1]] = pc
        elif s: lines.append((pc, s)); pc += 4
    hi = lambda m: str((labels[m[1]] + 0x8000) >> 16 & 0xffff)
    lo = lambda m: str((labels[m[1]] & 0xffff) - (labels[m[1]] & 0x8000) * 2)
    out = b''
    for pc, s in lines:
        n, _, rest = s.partition(' ')
        rest = re.sub(r'%lo\((\w+)\)', lo, re.sub(r'%hi\((\w+)\)', hi, rest))
        rest = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: f'0x{labels[m[0]]:08x}' if m[0] in labels else m[0], rest)
        out += struct.pack('<I', asm(f'{n} {rest}', pc))
    return out, labels
