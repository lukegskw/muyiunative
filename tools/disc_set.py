# Ordered local disc sets for the product build. M3U entries are UTF-8 local CUE/BIN paths relative to the playlist; comments and a BOM are accepted. No recursion,
# remote downloads or filename-based guesses about game families. The first disc retains the existing app/card identity when a single-disc app becomes a set.
from pathlib import Path

MAX_DISCS = 16


def sources(path, extra=()):
    path = Path(path).resolve()
    if path.suffix.lower() in ('.m3u', '.m3u8'):
        if extra: raise ValueError('choose a playlist or an explicit list of discs, not both')
        paths = [path.parent / line.strip() for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip() and not line.lstrip().startswith('#')]
    else: paths = [path, *extra]
    if not 1 <= len(paths) <= MAX_DISCS: raise ValueError(f'choose between 1 and {MAX_DISCS} discs')
    resolved = [Path(p).resolve() for p in paths]
    if len(set(resolved)) != len(resolved): raise ValueError('the disc set contains duplicate paths')
    for p in resolved:
        if p.suffix.lower() not in ('.cue', '.bin') or not p.is_file(): raise ValueError(f'{p.name}: choose an existing local CUE or BIN disc')
    return resolved
