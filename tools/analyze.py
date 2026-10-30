#!/usr/bin/env python3
# Function discovery (plan.md, section 2: the analyzer; phase 3): which words of an executable are code, cut into functions and basic blocks for the code generator.
# Recursive descent from the entry points and the profile's hints, not a sweep, so data is not taken for code. A call (JAL, BGEZAL r0) starts a function; a branch
# or a jump inside the function adds blocks; JR RA returns; a jump through another register goes where a constant put in it points (a BIOS call: ADDIU t2, r0,
# A0h), or through a table when the compiler's pattern is found (an SLTIU bound, the index shifted by SLL 2 and added to the table's address, made of constants,
# then LW) or the profile names the table; otherwise the target is left to run time. A block ends after the delay slot of its branch or jump (psx-spx, CPU Jump
# Opcodes: the next instruction runs first), at SYSCALL, BREAK or a reserved instruction, or where another block starts. The return address of a call and the
# address after SYSCALL/BREAK start blocks too: native code is entered there again after a callee or an exception handler ran elsewhere (the interpreter, the BIOS).
# Usage: python3 tools/analyze.py <file.exe|disc.cue> [--profile profiles/<serial>/game.toml] [--list]
import argparse, sys, tomllib
from dataclasses import dataclass, field

import exe, mips

EXITS = ('fall', 'branch', 'jump', 'call', 'tail', 'return', 'switch', 'indirect', 'exception', 'stop')
# A compiler puts a NOP (the zero word) in a delay slot or after a load, rarely more than a few in a row: a longer run is zero-filled memory, code that is gone (a
# capture's RAM where a program loaded later cleared what ran there) or never was, and decoding it would make a function of it
ZEROS = 64


@dataclass
class Block:   # [start, end): its instructions, a final branch or jump's delay slot included. exit, one of EXITS: how control leaves; succs: the blocks it goes to
    start: int; end: int; exit: str; succs: tuple = (); call: int | None = None   # call: a call's or tail jump's target, None when it is not known


@dataclass
class Function:
    start: int; blocks: dict = field(default_factory=dict)


@dataclass
class Program:
    functions: dict; notes: list   # notes: what was left unresolved, each beginning with its address


def hints_from_profile(path):   # the [analysis] table of a game's profile: functions = [...], jump_tables = [{jr, table, count}], data = [[start, end], ...]
    with open(path, 'rb') as f: return tomllib.load(f).get('analysis', {})


def analyze(word, entries, hints=None):   # word(address) -> the 32-bit word there, or None outside the image
    hints = hints or {}
    data = [(lo, hi) for lo, hi in hints.get('data', [])]
    tables = {t['jr']: (t['table'], t['count']) for t in hints.get('jump_tables', [])}
    notes = []

    def code(a): return None if any(lo <= a < hi for lo, hi in data) else word(a)

    def note(text):
        if text not in notes: notes.append(text)

    def explore(f, starts):   # the code reachable from f inside its function: each word decoded (delay slots too), the words scanned from a block's start,
        insns, scanned, leaders, ends, calls, work = {}, set(), {f}, {}, set(), [f]   # the block leaders, how each run of code ends, and the calls made
        while work:
            a, zeros = work.pop(), 0
            while a not in scanned:
                w = code(a)
                if w is None: note(f'{a:08x}: no code here (data, or outside the image)'); break
                zeros = zeros + 1 if w == 0 else 0
                if zeros > ZEROS: note(f'{a - 4 * ZEROS:08x}: {ZEROS} zero words in a row: memory, not code'); break
                op = insns[a] = mips.decode(w); scanned.add(a)
                if op.delay_slot:
                    d = code(a + 4)
                    if d is None: note(f'{a:08x}: {op.text(a)} has no delay slot in the image'); ends[a] = ('stop', (), None); break
                    if mips.decode(d).delay_slot: note(f'{a + 4:08x}: a branch or jump in a delay slot is left to the interpreter')
                    insns[a + 4] = mips.decode(d)
                    end = ends[a] = transfer(f, a, op, insns, starts)
                    if end[2] is not None and end[0] == 'call': calls.add(end[2])
                    if end[0] == 'tail' and end[2] is not None and word(end[2]) is not None: calls.add(end[2])
                    for s in end[1]:
                        if s not in leaders: leaders.add(s); work.append(s)
                    break
                if op.name in ('syscall', 'break', 'ri'):
                    ends[a] = ('exception', () if op.name == 'ri' else (a + 4,), None)
                    if op.name != 'ri' and a + 4 not in leaders: leaders.add(a + 4); work.append(a + 4)
                    break
                a += 4
                if a in starts and a != f: break   # runs into another function
        return insns, scanned, leaders, ends, {c for c in calls if word(c) is not None}

    def transfer(f, a, op, insns, starts):   # (exit, successors, call target) of the branch or jump at a
        n, t = op.name, op.target(a)
        always = n in ('j', 'jal') or (n == 'beq' and op.rs == op.rt) or (n in ('blez', 'bgez', 'bgezal') and op.rs == 0)
        if (n == 'bne' and op.rs == op.rt) or (n in ('bltz', 'bgtz', 'bltzal') and op.rs == 0): return 'branch', (a + 8,), None   # never taken
        if n in ('jal', 'bltzal', 'bgezal'): return 'call', (a + 8,), t   # taken or not, BLTZAL/BGEZAL link; a call goes on at a + 8 when it returns
        if n in mips.BRANCHES and not always: return 'branch', (t, a + 8), None
        if n in mips.BRANCHES or n == 'j': return ('tail', (), t) if t in starts and t != f else ('jump', (t,), None)
        if n == 'jr' and op.rs == 31: return 'return', (), None
        if n == 'jalr': return 'call', (a + 8,), constant(a, op.rs, insns)
        tab = tables.get(a)   # the profile's table first, then a constant, then the compiler's pattern
        if not tab:
            k = constant(a, op.rs, insns)
            if k is not None: return 'tail', (), k
            tab = table(a, op.rs, insns)
        if tab:
            at, count = tab
            if count is None: note(f'{a:08x}: {op.text(a)} goes through a table at {at:08x} of unknown length, which the profile can give'); return 'indirect', (), None
            targets = tuple(word(at + 4 * i) for i in range(count))
            if all(x is not None and code(x) is not None for x in targets): return 'switch', tuple(dict.fromkeys(targets)), None
            note(f'{a:08x}: the jump table at {at:08x} holds addresses outside the code')
        note(f'{a:08x}: {op.text(a)} goes where run time says')
        return 'indirect', (), None

    def last_write(a, r, insns, depth=24):   # (address, op) of the last instruction before a writing register r, on the straight path back; None past a jump
        for b in range(a - 4, a - 4 * depth - 4, -4):
            op = insns.get(b)
            if op is None or op.name in mips.JUMPS or op.name in ('syscall', 'break', 'ri'): return None
            if r in op.writes: return b, op
        return None

    def constant(a, r, insns):   # the address a register holds when a jump through it at a is reached, if the code just before set it from constants
        d = last_write(a, r, insns)
        if d is None: return None
        b, op = d
        if op.name in ('addiu', 'ori') and op.rs == 0: return op.simm & 0xffffffff if op.name == 'addiu' else op.imm
        if op.name == 'lui': return op.imm << 16
        if op.name in ('addiu', 'ori'):
            hi = last_write(b, op.rs, insns)
            if hi and hi[1].name == 'lui': return (hi[1].imm << 16) + op.simm & 0xffffffff if op.name == 'addiu' else hi[1].imm << 16 | op.imm
        return None

    def table(a, r, insns):   # (address, count or None) of a compiled switch: SLTIU c, i, count ... SLL x, i, 2; the table's address y from constants; ADDU z, x, y;
        d = last_write(a, r, insns)   # LW r, lo(z); JR r
        if d is None or d[1].name != 'lw': return None
        add = last_write(d[0], d[1].rs, insns)
        if add is None or add[1].name != 'addu': return None
        for p, q in ((add[1].rs, add[1].rt), (add[1].rt, add[1].rs)):
            base, sh = constant(add[0], p, insns), last_write(add[0], q, insns)
            if base is not None and sh and sh[1].name == 'sll' and sh[1].sa == 2:
                bound = next((op for b in range(d[0] - 4, d[0] - 4 * 32, -4) if (op := insns.get(b)) and op.name == 'sltiu' and op.rs == sh[1].rt), None)
                return (base + d[1].simm) & 0xffffffff, bound.imm if bound else None
        return None

    starts = set(entries) | set(hints.get('functions', []))   # pass 1: every function start, by following calls until no new one turns up
    work = list(starts)
    while work:
        found = explore(work.pop(), starts)[4] - starts
        starts |= found; work += found
    functions = {}   # pass 2: each function's blocks, now that every start is known
    for f in sorted(starts):
        if code(f) is None: note(f'{f:08x}: a function start with no code'); continue
        insns, scanned, leaders, ends, _ = explore(f, starts)
        fn = functions[f] = Function(f)
        for s in sorted(leaders & scanned):
            a = s
            while True:
                if a in ends:
                    how, succs, call = ends[a]
                    fn.blocks[s] = Block(s, a + 8 if insns[a].delay_slot and how != 'stop' else a + 4, how, tuple(x for x in succs if x in scanned), call)
                    break
                b = a + 4
                if b in leaders: fn.blocks[s] = Block(s, b, 'fall', (b,)); break
                if b not in scanned: fn.blocks[s] = Block(s, b, 'tail', (), b) if b in starts else Block(s, b, 'stop'); break
                a = b
    return Program(functions, sorted(notes))


def main():
    p = argparse.ArgumentParser(description='Find the functions and blocks of a PS-X EXE, or of the executable a disc boots')
    p.add_argument('path'); p.add_argument('--profile', help="a game's profile, for its [analysis] hints"); p.add_argument('--list', action='store_true')
    a = p.parse_args()
    x = exe.load(a.path)
    prog = analyze(x.word, [x.pc], hints_from_profile(a.profile) if a.profile else None)
    blocks = [b for f in prog.functions.values() for b in f.blocks.values()]
    covered = {i for b in blocks for i in range(b.start, b.end, 4)}
    print(f'analyze: {len(prog.functions)} functions, {len(blocks)} blocks, {len(covered)} instructions ({4 * len(covered) * 100 // max(len(x.text), 1)}% of '
          f'{len(x.text)} bytes)')
    print(f'analyze: {sum(b.exit == "indirect" for b in blocks)} jumps and {sum(b.exit == "call" and b.call is None for b in blocks)} calls through registers'
          f' left to run time, {sum(b.exit == "switch" for b in blocks)} jump tables, {len(prog.notes)} notes')
    if a.list:
        for f in prog.functions.values(): print(f'{f.start:08x} {len(f.blocks):5d} blocks {sum(b.end - b.start for b in f.blocks.values()):7d} bytes')
        for n in prog.notes: print(f'note: {n}')
    return 0


if __name__ == '__main__': sys.exit(main())
