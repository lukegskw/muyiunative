"""What t_native (runtime/tests) runs: a program and its exception handler, assembled here, run twice, interpreted and as the native code tools/codegen.py generates
for them; and words whose decoding the runtime's cpu_decode must agree on with tools/mips.py (plan.md, decision log: the tools' own decoder, tested against the
interpreter). Writes one C file for the test: python3 tools/tests/native_cases.py <out.c>"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import analyze, codegen, mips
from asm import program

BASE, VECTOR = 0x80010000, 0x80000080
# The exception handler: logs CAUSE and EPC at the pointer in 80020000h, clears the software interrupts, and returns: to EPC for an interrupt, past the instruction
# for an exception, and past the branch's delay slot when CAUSE.BD is set. Only k0 and k1 are used
HANDLER = '''
    lui k0, 0x8002
    lw k1, 0(k0)
    mfc0 k0, $13
    nop
    sw k0, 0(k1)
    mfc0 k0, $14
    nop
    sw k0, 4(k1)
    addiu k1, k1, 8
    lui k0, 0x8002
    sw k1, 0(k0)
    mfc0 k1, $13
    mfc0 k0, $14
    mtc0 zero, $13
    bltz k1, bd
    andi k1, k1, 0x7c
    beq k1, zero, done
    nop
    addiu k0, k0, 4
done:
    jr k0
    rfe
bd:
    j done
    addiu k0, k0, 8
'''
MAIN = '''
start:
    lui t0, 0x8002
    addiu t1, t0, 0x100
    sw t1, 0(t0)                ; the handler's log
    lui at, 0x4000              ; SR: COP2 on, interrupts off
    mtc0 at, $12
    lui s0, 0x1234              ; arithmetic and logic
    ori s0, s0, 0x5678
    addiu s1, zero, -5
    addu v0, s0, s1
    subu v1, s0, s1
    and a0, s0, s1
    or a1, s0, s1
    xor a2, s0, s1
    nor a3, s0, s1
    slt t2, s1, s0
    sltu t3, s1, s0
    slti t4, s1, -4
    sltiu t5, s1, 3
    andi t6, s0, 0xff0f
    xori t7, s0, 0xffff
    sll t8, s0, 7
    srl t9, s1, 3
    sra s2, s1, 1
    addiu s3, zero, 37
    sllv s4, s0, s3
    srlv s5, s1, s3
    srav s6, s1, s3
    add s7, s0, s0
    sub fp, s0, s1
    addi gp, s1, 100
    lui t0, 0x7fff              ; overflows: exceptions, no result; each in a function of its own, so that the handler comes back to its JR RA and
    ori t0, t0, 0xffff          ; the rest of this block still runs as native code
    lui t2, 0x8000
    jal f_add
    nop
    jal f_addi
    nop
    jal f_sub
    nop
    mult s0, s1                 ; MULT/DIV, and MFHI/MFLO waiting for them
    mflo v0
    mfhi v1
    multu s0, s1
    nop
    nop
    mflo a0
    mfhi a1
    div s0, s1
    mflo a2
    mfhi a3
    divu s0, s1
    mflo t2
    mfhi t3
    div s0, zero
    mflo t4
    mfhi t5
    lui t6, 0x8000
    addiu t7, zero, -1
    div t6, t7
    mflo t8
    mfhi t9
    divu s0, zero
    mflo t4
    mthi s0
    mtlo s1
    mfhi t5
    mflo t6
    lui s0, 0x8003              ; loads and stores, and the load delay slot
    lui t0, 0xdead
    ori t0, t0, 0xbeef
    sw t0, 0(s0)
    lui t1, 0x7766
    ori t1, t1, 0x5544
    sw t1, 4(s0)
    lw t2, 0(s0)
    addu t3, t2, zero
    addu t4, t2, zero
    lw t5, 4(s0)
    addiu t5, zero, 9
    lb t6, 3(s0)
    lbu t7, 3(s0)
    lh t8, 2(s0)
    lhu t9, 2(s0)
    lwr a0, 1(s0)
    lwl a0, 4(s0)
    nop
    lwl a1, 5(s0)
    lwr a1, 2(s0)
    nop
    swl t0, 9(s0)
    swr t1, 14(s0)
    sb t0, 16(s0)
    sh t1, 18(s0)
    lw v0, 8(s0)
    lw v1, 12(s0)
    lw a2, 16(s0)
    jal f_lw                    ; address errors
    nop
    jal f_sh
    nop
    jal f_lh
    nop
    addiu t0, zero, 0           ; each branch, taken and not
    addiu t1, zero, 1
    beq t0, t1, skip1
    addiu t2, zero, 11
    bne t0, t1, skip1
    addiu t3, zero, 12
    addiu t4, zero, 13
skip1:
    blez t0, skip2
    lw t5, 0(s0)                ; a load in a delay slot lands after the target's first instruction
skip2:
    addu t6, t5, zero
    bgtz t0, skip3
    nop
    bltz t1, skip3
    nop
    bgez t1, skip3
    nop
skip3:
    bltzal t1, sub1             ; not taken, and links
    nop
    bgezal t1, sub1
    nop
    jal sub1
    addiu a0, zero, 5
    lui t0, %hi(sub1)
    addiu t0, t0, %lo(sub1)
    jalr t0
    addiu a0, zero, 6
    lui t0, %hi(sub2)
    addiu t0, t0, %lo(sub2)
    jalr a1, t0
    nop
    addiu a0, zero, 2
    jal switch
    nop
    addiu a0, zero, 7
    jal switch
    nop
    syscall                     ; exceptions the handler returns from
    break 0x3
    jal reserved
    nop
    jal f_cop1
    nop
    beq zero, zero, after
    syscall                     ; in a delay slot: EPC at the branch, CAUSE.BD
after:
    mfc0 t0, $12
    nop
    mtc0 t0, $12
    .word 0x42000001            ; a COP0 TLB command: no effect
    lui t0, 0x0123              ; the GTE
    ori t0, t0, 0x4567
    mtc2 t0, $9
    mtc2 s1, $10
    mtc2 t1, $11
    cop2 0x0a00428              ; SQR
    mfc2 v0, $25
    mfc2 v1, $26
    cfc2 a0, $31
    ctc2 t0, $0
    cfc2 a1, $0
    lwc2 $9, 0(s0)
    cop2 0x0a00428
    swc2 $25, 20(s0)
    bc2f bcf
    nop
bcf:
    bc2t bcf
    nop
    lui at, 0x4000              ; a software interrupt, taken at the end of the block
    ori at, at, 0x0101
    mtc0 at, $12
    addiu at, zero, 0x100
    mtc0 at, $13
    addiu t0, zero, 1
    addiu t1, zero, 2
    beq zero, zero, irq
    addiu t2, zero, 3
irq:
    addiu t3, zero, 4
    lui at, 0x4001              ; SR.IsC: stores reach the cache, not memory
    mtc0 at, $12
    sw t0, 24(s0)
    lui at, 0x4000
    mtc0 at, $12
    addiu t0, zero, 100         ; a loop
loop:
    addiu t0, t0, -1
    bne t0, zero, loop
    addu t1, t1, t0
    lui t0, 0x1f80              ; the exit port
    ori t0, t0, 0x2082
    sh zero, 0(t0)
end:
    beq zero, zero, end
    nop
sub1:
    jr ra
    addu v0, a0, a0
sub2:
    jr a1
    nop
reserved:
    .word 0x5c000000            ; a reserved instruction; the handler comes back to the JR
    jr ra
    nop
f_add:
    add t1, t0, t0
    jr ra
    nop
f_addi:
    addi t1, t0, 1
    jr ra
    nop
f_sub:
    sub t1, t2, s0
    jr ra
    nop
f_lw:
    lw a3, 2(s0)
    jr ra
    nop
f_sh:
    sh a3, 1(s0)
    jr ra
    nop
f_lh:
    lh a3, 3(s0)
    jr ra
    nop
f_cop1:
    .word 0x44000000            ; COP1 while SR.CU1 is off: unusable
    jr ra
    nop
switch:
    sltiu v0, a0, 3
    beq v0, zero, other
    sll v0, a0, 2
    lui at, %hi(table)
    addu at, at, v0
    lw v0, %lo(table)(at)
    nop
    jr v0
    nop
case0:
    jr ra
    addiu v0, zero, 10
case1:
    jr ra
    addiu v0, zero, 11
case2:
    jr ra
    addiu v0, zero, 12
other:
    jr ra
    addiu v0, zero, -1
table:
    .word case0
    .word case1
    .word case2
'''


def decode_words():   # each primary opcode, SPECIAL function, BcondZ rt, COP0 and COP2 rs, with operand bits set
    words = [p << 26 | 0x00a51234 for p in range(64)] + [0x00a51000 | f for f in range(64)] + [0x04800003 | rt << 16 for rt in range(32)]
    words += [0x40000000 | rs << 21 | 0x001a6800 | f for rs in range(32) for f in (0x00, 0x01, 0x10)]
    words += [0x48000000 | rs << 21 | rt << 16 for rs in range(32) for rt in (0, 1, 2)]
    return words


def array(name, words): return f'const uint32_t {name}[] = {{' + ', '.join(f'0x{w:08x}u' for w in words) + f'}};\nconst unsigned {name}_n = {len(words)};'


def main():
    code, _ = program(BASE, MAIN)
    handler, _ = program(VECTOR, HANDLER)
    regions = [(BASE, code), (VECTOR, handler)]

    def word(a):
        for base, b in regions:
            if base <= a <= base + len(b) - 4 and not a & 3: return int.from_bytes(b[a - base:a - base + 4], 'little')
        return None
    prog = analyze.analyze(word, [BASE, VECTOR])
    words = decode_words()
    names = ['cop' if (n := mips.decode(w).name) in mips.COP else n for w in words]
    out = ['// Generated by tools/tests/native_cases.py for runtime/tests/t_native.c', '#include "native.h"', '',
           'const struct native_word { uint32_t word; int op; } native_words[] = {' + ', '.join(f'{{0x{w:08x}u, CPU_{n}}}' for w, n in zip(words, names)) + '};',
           f'const unsigned native_words_n = {len(words)};', f'const uint32_t native_base = 0x{BASE:08x}u, native_vector = 0x{VECTOR:08x}u;',
           array('native_main', [word(a) for a in range(BASE, BASE + len(code), 4)]), array('native_handler', [word(a) for a in range(VECTOR, VECTOR + len(handler), 4)]),
           *(f'// {n}' for n in prog.notes), '', codegen.generate(word, prog, 'test')]
    Path(sys.argv[1]).parent.mkdir(parents=True, exist_ok=True)
    Path(sys.argv[1]).write_text('\n'.join(out))


if __name__ == '__main__': main()
