"""tools/analyze.py finds the functions and blocks of programs assembled here: calls and returns, a loop, code running on into another function, a tail call,
a BIOS call through a register set from a constant, a compiled switch with its bounds check, and data the descent never decodes, though it looks like a call.
Then what only the profile knows (a function reached through a pointer, a jump table, a data range), what is left to run time with a note, exceptions and their
resume points, branches that are always or never taken, and the command line on an executable. No game files."""
import shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import analyze
from asm import program
from test_exe import make

B = 0x80010000
MAIN = '''
start:
    addiu sp, sp, -24
    sw ra, 16(sp)
    jal pre
    nop
    jal func
    nop
    addiu a0, zero, 3       ; 18: where func returns, then into the loop
loop:
    addiu a0, a0, -1
    bne a0, zero, loop
    nop
    jal switch
    addiu a0, zero, 2       ; the argument, in the delay slot
    jal bios_puts
    nop
    lw ra, 16(sp)
    jr ra
    addiu sp, sp, 24
    .word 0xffffffff        ; 44: data after the return
    .word 0x0c000000        ; 48: data that reads as a JAL
pre:
    addiu v0, zero, 1       ; runs on into func
func:
    jr ra
    nop
bios_puts:                  ; the BIOS's A0h table, function 3Fh
    addiu t2, zero, 0xa0
    jr t2
    addiu t1, zero, 0x3f
switch:
    sltiu v0, a0, 3
    beq v0, zero, default
    sll v0, a0, 2
    lui at, %hi(table)
    addu at, at, v0
    lw v0, %lo(table)(at)
    nop
    jr v0
    nop
case0:
    j default
    addiu v0, zero, 10
case1:
    j func                  ; a tail call
    addiu v0, zero, 11
case2:
    addiu v0, zero, 12
default:
    jr ra
    nop
table:
    .word case0
    .word case1
    .word case2
'''
POINTERS = '''
start:
    lw t0, 0(a0)
    nop
    jalr t0                 ; a call through a pointer: its target is left to run time
    nop
    lw t1, 4(a0)
    nop
    jr t1                   ; and a jump
    nop
handler:                    ; 20: code only the profile names
    syscall
    lw v0, 0(a1)
    nop
    jr v0                   ; 2c: through a table only the profile names
    nop
one:
    jr ra
    nop
two:                        ; 3c
    break
    .word 0x5c000000        ; 40: a reserved instruction, where the handler of BREAK would come back
tab:
    .word one
    .word two
'''
UNBOUNDED = '''
start:
    lui v1, %hi(tab)        ; the table's address from LUI and ADDIU, as newer compilers build it
    addiu v1, v1, %lo(tab)
next:
    sll a0, a0, 2           ; no bound on the index
    addu v1, v1, a0
    lw v1, 0(v1)
    nop
    jr v1
    nop
tab:
    .word start
    .word next
'''
TAKEN = '''
start:
    bgezal zero, sub        ; always taken: a call
    nop
    bltzal zero, data       ; never taken, though it links
    nop
    beq zero, zero, end     ; always taken
    nop
    .word 0xffffffff
end:
    beq a0, zero, end2
    j end2                  ; a jump in a delay slot
    nop
end2:
    jr ra
    nop
sub:
    jr ra
    nop
data:
    .word 0x5c000000
    addiu v0, zero, 1       ; and code that runs out of the image
'''


def run(src, entries=('start',), hints=None):
    code, labels = program(B, src)
    word = lambda a: int.from_bytes(code[a - B:a - B + 4], 'little') if B <= a <= B + len(code) - 4 else None
    p = analyze.analyze(word, [labels[e] for e in entries], hints)
    rel = lambda a: a - B if a is not None and a >= B else a
    return {rel(f): [(rel(b.start), rel(b.end), b.exit, tuple(map(rel, b.succs)), rel(b.call)) for b in fn.blocks.values()] for f, fn in p.functions.items()}, p.notes, labels


class Analyze(unittest.TestCase):
    def test_main(self):
        fns, notes, _ = run(MAIN)
        self.assertEqual(fns, {
            0x00: [(0x00, 0x10, 'call', (0x10,), 0x4c), (0x10, 0x18, 'call', (0x18,), 0x50), (0x18, 0x1c, 'fall', (0x1c,), None),
                   (0x1c, 0x28, 'branch', (0x1c, 0x28), None), (0x28, 0x30, 'call', (0x30,), 0x64), (0x30, 0x38, 'call', (0x38,), 0x58),
                   (0x38, 0x44, 'return', (), None)],
            0x4c: [(0x4c, 0x50, 'tail', (), 0x50)],
            0x50: [(0x50, 0x58, 'return', (), None)],
            0x58: [(0x58, 0x64, 'tail', (), 0xa0)],
            0x64: [(0x64, 0x70, 'branch', (0x9c, 0x70), None), (0x70, 0x88, 'switch', (0x88, 0x90, 0x98), None), (0x88, 0x90, 'jump', (0x9c,), None),
                   (0x90, 0x98, 'tail', (), 0x50), (0x98, 0x9c, 'fall', (0x9c,), None), (0x9c, 0xa4, 'return', (), None)]})
        self.assertEqual(notes, [])

    def test_left_to_run_time(self):
        fns, notes, _ = run(POINTERS)
        self.assertEqual(fns, {0x00: [(0x00, 0x10, 'call', (0x10,), None), (0x10, 0x20, 'indirect', (), None)]})
        self.assertEqual(notes, ['80010018: jr t1 goes where run time says'])
        fns, notes, _ = run(UNBOUNDED)
        self.assertEqual(fns, {0x00: [(0x00, 0x20, 'indirect', (), None)]})
        self.assertEqual(notes, ['80010018: jr v1 goes through a table at 80010020 of unknown length, which the profile can give'])
        fns, notes, _ = run(UNBOUNDED, hints={'jump_tables': [{'jr': B + 0x18, 'table': B + 0x20, 'count': 2}]})
        self.assertEqual((fns, notes), ({0x00: [(0x00, 0x08, 'fall', (0x08,), None), (0x08, 0x20, 'switch', (0x00, 0x08), None)]}, []))

    def test_hints(self):
        hints = {'functions': [B + 0x20], 'jump_tables': [{'jr': B + 0x2c, 'table': B + 0x44, 'count': 2}]}
        fns, notes, _ = run(POINTERS, hints=hints)
        self.assertEqual(fns[0x20], [(0x20, 0x24, 'exception', (0x24,), None), (0x24, 0x34, 'switch', (0x34, 0x3c), None), (0x34, 0x3c, 'return', (), None),
                                     (0x3c, 0x40, 'exception', (0x40,), None), (0x40, 0x44, 'exception', (), None)])
        fns, notes, _ = run(POINTERS, hints=hints | {'data': [[B + 0x34, B + 0x3c]]})
        self.assertEqual(fns[0x20][1], (0x24, 0x34, 'indirect', (), None))
        self.assertIn('8001002c: the jump table at 80010044 holds addresses outside the code', notes)

    def test_taken_and_notes(self):
        fns, notes, _ = run(TAKEN)
        self.assertEqual(fns, {0x00: [(0x00, 0x08, 'call', (0x08,), 0x30), (0x08, 0x10, 'branch', (0x10,), None), (0x10, 0x18, 'jump', (0x1c,), None),
                                      (0x1c, 0x24, 'branch', (0x28, 0x24), None), (0x24, 0x28, 'fall', (0x28,), None), (0x28, 0x30, 'return', (), None)],
                               0x30: [(0x30, 0x38, 'return', (), None)]})
        self.assertEqual(notes, ['80010020: a branch or jump in a delay slot is left to the interpreter'])
        fns, notes, _ = run(TAKEN, entries=('start', 'data'))
        self.assertEqual(fns[0x38], [(0x38, 0x3c, 'exception', (), None)])
        fns, notes, _ = run(TAKEN.replace('    .word 0x5c000000\n', ''), entries=('start', 'data'))
        self.assertEqual(fns[0x38], [(0x38, 0x3c, 'stop', (), None)])
        self.assertIn('8001003c: no code here (data, or outside the image)', notes)

    def test_profile_and_command_line(self):
        tmp = Path(tempfile.mkdtemp())
        try:
            (tmp / 'game.toml').write_text('name = "x"\n[analysis]\nfunctions = [0x80010020]\njump_tables = [{ jr = 0x8001002c, table = 0x80010044, count = 2 }]\n'
                                           'data = [[0x80010100, 0x80010200]]\n')
            self.assertEqual(analyze.hints_from_profile(tmp / 'game.toml'), {'functions': [0x80010020], 'jump_tables': [{'jr': 0x8001002c, 'table': 0x80010044,
                                                                             'count': 2}], 'data': [[0x80010100, 0x80010200]]})
            (tmp / 'main.exe').write_bytes(make(program(B, MAIN)[0]))
            r = subprocess.run([sys.executable, ROOT / 'tools' / 'analyze.py', tmp / 'main.exe', '--list'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertEqual(r.stdout.splitlines()[:3], ['analyze: 5 functions, 16 blocks, 39 instructions (88% of 176 bytes)',
                                                         'analyze: 0 jumps and 0 calls through registers left to run time, 1 jump tables, 0 notes',
                                                         '80010000     7 blocks      68 bytes'])
        finally: shutil.rmtree(tmp)


if __name__ == '__main__': unittest.main()
