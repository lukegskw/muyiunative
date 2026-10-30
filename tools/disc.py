#!/usr/bin/env python3
# The disc reader (plan.md, section 2, Python tools): a CUE sheet's tracks, or a raw BIN as one data track, and the ISO9660 files of the first track; enough, for now,
# to find what identifies a disc for its profile (scripts/disc_info.py): the executable SYSTEM.CNF boots and the serial in its name, the region of the licence string,
# and SHA-256 hashes of the executable and of each track. psx-spx, CDROM File Formats (CUE/BIN, SYSTEM.CNF), CDROM ISO Volume Descriptors and File/Directory
# Descriptors; the licence string is read as runtime/cdrom.c reads it. CUE sheets are read as runtime/disc.c reads them: BINARY files of MODE1/2352, MODE2/2352
# and AUDIO tracks with INDEX 00/01, no PREGAP or POSTGAP. A disc is only read; nothing is written or copied.
import hashlib, re, struct
from dataclasses import dataclass
from pathlib import Path

SECTOR = 2352


@dataclass
class Track:   # first, start, end: sectors into its file: its first index (00, or 01 without one), INDEX 01, and where the next track or the file begins
    number: int; mode: str; file: Path; first: int = -1; start: int = -1; end: int = 0


def file_sectors(path):
    size = path.stat().st_size
    if not size or size % SECTOR: raise SystemExit(f'{path.name}: {size} bytes, not whole {SECTOR}-byte sectors')
    return size // SECTOR


def tracks(path):   # a CUE sheet's tracks, or a raw BIN as one MODE2/2352 track
    path = Path(path)
    if path.suffix.lower() != '.cue': return [Track(1, 'MODE2/2352', path, 0, 0, file_sectors(path))]
    out, file = [], None
    for n, line in enumerate(path.read_text(encoding='latin-1').splitlines(), 1):
        w = line.split()
        if not w: continue
        k = w[0].upper()
        try:
            if k == 'FILE':
                m = re.fullmatch(r'\s*FILE\s+(?:"([^"]+)"|(\S+))\s+(\S+)\s*', line, re.I)
                if not m or m.group(3).upper() != 'BINARY': raise SystemExit(f'{path.name}:{n}: only BINARY files are read')
                file = path.parent / (m.group(1) or m.group(2))
            elif k == 'TRACK':
                if file is None or len(w) != 3 or w[2].upper() not in ('MODE1/2352', 'MODE2/2352', 'AUDIO'):
                    raise SystemExit(f'{path.name}:{n}: a TRACK of MODE1/2352, MODE2/2352 or AUDIO, after its FILE')
                out.append(Track(int(w[1]), w[2].upper(), file))
            elif k == 'INDEX' and out and len(w) == 3:
                mm, ss, ff = (int(v) for v in w[2].split(':')); at = (mm * 60 + ss) * 75 + ff; t = out[-1]
                if int(w[1]) == 0: t.first = at
                elif int(w[1]) == 1: t.start = at; t.first = at if t.first < 0 else t.first
            elif k in ('PREGAP', 'POSTGAP', 'INDEX'): raise SystemExit(f'{path.name}:{n}: {k} is not read')
        except ValueError: raise SystemExit(f'{path.name}:{n}: not read: {line.strip()}') from None
    if not out: raise SystemExit(f'{path.name}: no tracks')
    for i, t in enumerate(out):
        nxt = out[i + 1] if i + 1 < len(out) else None
        t.end = nxt.first if nxt and nxt.file == t.file else file_sectors(t.file)
        if not 0 <= t.first <= t.start < t.end <= file_sectors(t.file): raise SystemExit(f'{path.name}: track {t.number} has no INDEX 01 inside its file')
    if out[0].mode == 'AUDIO': raise SystemExit(f'{path.name}: the first track is audio, not data')
    return out


def sector(track, lba):   # the 2048 bytes of user data of sector lba of a data track (LBA 0 = its INDEX 01), after a Mode 1 or Mode 2 header
    if track.start + lba >= track.end: raise SystemExit(f'{track.file.name}: sector {lba} is past the end of track {track.number}')
    with track.file.open('rb') as f: f.seek((track.start + lba) * SECTOR); s = f.read(SECTOR)
    return s[24 if s[15] == 2 else 16:][:2048]


def read(track, lba, size): return b''.join(sector(track, lba + k) for k in range((size + 2047) // 2048))[:size]


def records(track, lba, size):   # a directory's records: (name, lba, size, is a directory); names keep their ";1"
    for k in range((size + 2047) // 2048):
        d, i = sector(track, lba + k), 0
        while i + 33 <= 2048 and d[i]:
            yield d[i + 33:i + 33 + d[i + 32]].decode('latin-1'), *struct.unpack_from('<I', d, i + 2), *struct.unpack_from('<I', d, i + 10), bool(d[i + 25] & 2)
            i += d[i]


def find(track, path):   # (lba, size) of a file by its path, as SYSTEM.CNF writes it (DIR\NAME.EXT;1, any case, the version optional), or None
    pvd = sector(track, 16)
    if pvd[:6] != b'\x01CD001': raise SystemExit(f'{track.file.name}: no ISO9660 volume descriptor at sector 16')
    lba, size = struct.unpack_from('<I', pvd, 156 + 2)[0], struct.unpack_from('<I', pvd, 156 + 10)[0]
    parts = [re.sub(r';\d*$', '', p).upper() for p in re.split(r'[\\/]', path) if p]
    for i, want in enumerate(parts):
        hit = next(((a, s, d) for name, a, s, d in records(track, lba, size) if re.sub(r';\d*$', '', name).upper() == want), None)
        if not hit or hit[2] != (i < len(parts) - 1): return None
        lba, size = hit[0], hit[1]
    return lba, size


def licence(track):   # the region of the licence string in sector 4, as the drive answers GetID with it: SCEA, SCEE or SCEI; None without one
    t = sector(track, 4)[0x20:0x40]
    return {b'Amer': 'SCEA', b'Euro': 'SCEE', b'Inc.': 'SCEI'}.get(t[28:32]) if t.startswith(b'Sony Computer Entertainment ') else None


def sha256(track):   # the track's raw sectors from its first index on: a whole file when each track has one, as dumps usually do
    h = hashlib.sha256()
    with track.file.open('rb') as f:
        f.seek(track.first * SECTOR); left = (track.end - track.first) * SECTOR
        while left:
            b = f.read(min(left, 1 << 20))
            if not b: raise SystemExit(f'{track.file.name}: ends inside track {track.number}')
            h.update(b); left -= len(b)
    return h.hexdigest()


def identify(path):   # what scripts/disc_info.py prints
    ts = tracks(path); data = ts[0]
    cnf = find(data, 'SYSTEM.CNF')
    if cnf:   # BOOT = cdrom:\NAME.EXT;1, or cdrom:NAME.EXT;1
        m = re.search(rb'BOOT\s*=\s*cdrom:\\?([^\s;]+)', read(data, *cnf), re.I)
        if not m: raise SystemExit('SYSTEM.CNF names no BOOT executable')
        boot = m.group(1).decode('latin-1')
    else: boot = 'PSX.EXE'   # what the BIOS boots without SYSTEM.CNF
    exe = find(data, boot)
    if not exe: raise SystemExit(f'the boot executable {boot} is not on the disc')
    m = re.fullmatch(r'([A-Z]{4})[_-](\d{3})\.(\d{2})', re.split(r'[\\/]', boot)[-1], re.I)
    return {'serial': f'{m.group(1).upper()}-{m.group(2)}{m.group(3)}' if m else None, 'licence': licence(data), 'boot': boot,
            'boot_sha256': hashlib.sha256(read(data, *exe)).hexdigest(),
            'tracks': [{'number': t.number, 'mode': t.mode, 'sectors': t.end - t.first, 'sha256': sha256(t)} for t in ts]}
