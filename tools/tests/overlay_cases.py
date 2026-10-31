"""Synthetic code versions for t_overlays: two programs sharing an address, an instruction rewritten through a RAM mirror, and synchronous DMA into running code.
All words are assembled here; no disc or BIOS is needed. The native set uses the same content-keyed blocks as a game's capture (plan.md, phase 4)."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import analyze, codegen, overlays
from asm import program

CASES = [(0x80010000, f'addiu v0, zero, {n}\njr ra\nnop') for n in (11, 22)] + [
    (0x80011000, '''lui t0, 0xa021
ori t0, t0, 0x1000
lui t1, 0x2402
ori t1, t1, 44
sw t1, 20(t0)
addiu v0, zero, 11
jr ra
nop'''),
    (0x80012000, 'sw t1, 0(t0)\naddiu v0, zero, 11\njr ra\nnop')]


def main(out):
    blocks = [overlays.Block(pc, program(pc, source)[0]) for pc, source in CASES]
    p = overlays.extend(analyze.Program({}, []), blocks)
    text = codegen.generate(lambda _: None, p, 'overlays')
    for i, b in enumerate(blocks):
        words = ', '.join(f'0x{b.word(a):08x}u' for a in range(b.pc, b.pc + len(b.code), 4))
        text += f'\nconst uint32_t overlay_case_{i}[] = {{{words}}};\n'
    Path(out).write_text(text)


if __name__ == '__main__': main(sys.argv[1])
