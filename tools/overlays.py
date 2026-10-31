# Captured code versions (plan.md, phase 4): runtime/capture.c records immutable blocks as they execute, before a later disc load or a store overwrites them.
# SHA-256 of the exact instruction bytes identifies each version; the address also matters to compilation (branches and PC-relative instructions).
# No overlay boundaries or disc filenames are guessed. Blocks absent from the final RAM image remain compilable, and all versions share the runtime's byte guard.
# Captures contain game code and stay in local build folders; a survey report contains only addresses, counts and hashes.
import hashlib, struct
from dataclasses import dataclass
from pathlib import Path

import analyze


@dataclass
class Block:
    pc: int; code: bytes; hits: int = 1

    @property
    def digest(self): return hashlib.sha256(self.code).hexdigest()

    def word(self, a):
        i = a - self.pc
        return int.from_bytes(self.code[i:i + 4], 'little') if not i % 4 and 0 <= i <= len(self.code) - 4 else None


def read(path):
    data = Path(path).read_bytes()
    if data[:8] != b'PS1BLK1\0': raise ValueError('capture has no PS1BLK1 header')
    out, at, seen = [], 8, set()
    while at < len(data):
        if len(data) - at < 16: raise ValueError('truncated capture record')
        pc, n, hits = struct.unpack_from('<IIQ', data, at); at += 16
        if pc % 4 or not 1 <= n <= 258 or pc + 4 * n > 1 << 32 or not hits: raise ValueError('invalid capture block')
        if len(data) - at < 4 * n: raise ValueError('truncated capture words')
        b = Block(pc, data[at:at + 4 * n], hits); at += 4 * n
        key = pc, b.digest
        if key in seen: raise ValueError('duplicate capture block')
        seen.add(key); out.append(b)
    return sorted(out, key=lambda b: (b.pc, b.digest))


@dataclass
class Function(analyze.Function):
    image: Block | None = None


def extend(program, blocks):
    # A block's own word reader keeps overlapping versions independent; static analysis of final RAM is still useful for code the route did not reach.
    for b in blocks:
        f = Function(b.pc, image=b)
        f.blocks[b.pc] = analyze.Block(b.pc, b.pc + len(b.code), 'stop')
        program.functions[b.pc, b.digest] = f   # codegen reads this captured image, independently of final RAM
    return program


def report(blocks):
    versions = {}
    for b in blocks: versions.setdefault(b.pc, set()).add(b.digest)
    return {'blocks': len(blocks), 'instructions': sum(len(b.code) // 4 * b.hits for b in blocks),
            'replaced_addresses': sum(len(v) > 1 for v in versions.values()),
            'code': [{'pc': f'{b.pc:08x}', 'sha256': b.digest, 'words': len(b.code) // 4, 'hits': b.hits} for b in blocks]}
