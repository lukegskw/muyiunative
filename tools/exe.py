#!/usr/bin/env python3
# The PS-X EXE reader (plan.md, section 2: the executable loader): the header and the code/data an executable loads into RAM, from a file or from the disc that boots
# it (tools/disc.py). psx-spx, CDROM File Formats, "FILENAME.EXE (General-Purpose Executable)": an 800h-byte header with "PS-X EXE", the initial PC (10h) and GP
# (14h), the destination in RAM (18h) and the size of what follows (1Ch), BSS (28h, 2Ch), the initial SP base and offset (30h, 34h), and the region's marker
# (from 4Ch); the code/data follow at 800h. The checks are runtime/exe.c's: the code/data and the entry must lie in main RAM (2 MB, through any segment)
import re, struct
from dataclasses import dataclass
from pathlib import Path

import disc

HEADER, RAM = 0x800, 2 << 20


@dataclass(frozen=True)
class Exe:
    pc: int; gp: int; addr: int; text: bytes; bss: int; bss_size: int; sp: int; marker: str

    @property
    def end(self): return self.addr + len(self.text)

    def word(self, a):   # the little-endian word the executable loads at address a, or None outside its code/data
        o = a - self.addr
        return struct.unpack_from('<I', self.text, o)[0] if 0 <= o <= len(self.text) - 4 and not o & 3 else None


def in_ram(a, n): p = a & 0x1fffffff; return p < RAM and n <= RAM - p


def parse(data, name='executable'):
    if len(data) < HEADER or data[:8] != b'PS-X EXE': raise SystemExit(f'{name}: not a PS-X EXE')
    pc, gp, addr, size, _, _, bss, bss_size, sp, sp_off = struct.unpack_from('<10I', data, 0x10)
    if size > len(data) - HEADER or not in_ram(addr, size) or not in_ram(pc, 4): raise SystemExit(f'{name}: the header\'s code/data or entry is not in main RAM')
    marker = re.sub(rb'[\x00-\x1f\x7f-\xff].*', b'', data[0x4c:HEADER], flags=re.S).decode('ascii')
    return Exe(pc, gp, addr, bytes(data[HEADER:HEADER + size]), bss, bss_size, (sp + sp_off) & 0xffffffff if sp else 0, marker)


def load(path):   # a PS-X EXE file, or the executable a disc image boots (.cue, or a raw .bin)
    path = Path(path)
    if path.suffix.lower() in ('.cue', '.bin'): name, data = disc.boot(disc.tracks(path)[0]); return parse(data, name)
    return parse(path.read_bytes(), path.name)
