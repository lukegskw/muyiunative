#!/usr/bin/env python3
# Phase 7's source kit, packed inside the signed launcher. An allowlist excludes tests, build products, captures and all external game data.
# The manifest carries the launcher's build identity outside Git; SHA-256 checks detect an incomplete or changed kit before copying it to the build cache.
import argparse, hashlib, json, shutil, tempfile
from pathlib import Path

from pack_app import replace_directory
from version import metadata

ROOT = Path(__file__).resolve().parents[1]


def source_files(root=ROOT):
    files = [root / 'LICENSE', root / 'scripts/muyiunative']
    for folder, patterns in [('runtime', ['*.c', '*.h']), ('shell/mac', ['*.c', '*.m', '*.h', 'Makefile']),
                             ('gfx/metal', ['*.m', '*.h']), ('tools', ['*.py', 'capture.c']),
                             ('tools/vendor', ['**/*']), ('profiles', ['*/game.toml']), ('bios/openbios', ['*'])]:
        for pattern in patterns:
            files += [p for p in (root / folder).glob(pattern) if p.is_file() and not p.is_symlink() and '__pycache__' not in p.parts and p.suffix != '.pyc']
    return sorted(set(files))


def manifest(root):
    root = Path(root); data = json.loads((root / 'build-kit.json').read_text())
    if data.get('KitVersion') != 1 or not isinstance(data.get('Files'), dict): raise ValueError('invalid build-kit manifest')
    metadata(root)
    for name, digest in data['Files'].items():
        path = Path(name)
        if path.is_absolute() or '..' in path.parts or not isinstance(digest, str): raise ValueError('invalid build-kit path')
        target = root / path
        if not target.is_file() or target.is_symlink() or not target.resolve().is_relative_to(root.resolve()) or hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise ValueError(f'incomplete or changed build kit: {name}')
    required = {'scripts/muyiunative', 'tools/player_build.py', 'gfx/metal/ps1gpu.metallib', 'bios/openbios/openbios.bin', 'shell/mac/Makefile'}
    if not required <= set(data['Files']): raise ValueError('the build kit is missing required files')
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file() and p != root / 'build-kit.json'}
    if actual != set(data['Files']): raise ValueError('the build kit contains unexpected files')
    return data


def export(out, metallib, root=ROOT):
    out, metallib, root = Path(out).resolve(), Path(metallib).resolve(), Path(root).resolve()
    if not metallib.is_file(): raise ValueError('the launcher needs a precompiled metallib')
    if out.exists(): manifest(out)
    out.parent.mkdir(parents=True, exist_ok=True); staged = Path(tempfile.mkdtemp(prefix='build-kit.partial-', dir=out.parent))
    try:
        for source in source_files(root):
            dest = staged / source.relative_to(root); dest.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(source, dest)
        shutil.copyfile(metallib, staged / 'gfx/metal/ps1gpu.metallib')
        version, build, commit = metadata(root)
        files = {p.relative_to(staged).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(staged.rglob('*')) if p.is_file()}
        (staged / 'build-kit.json').write_text(json.dumps({'KitVersion': 1, 'AppVersion': version, 'BuildVersion': build, 'Commit': commit, 'Files': files}, indent=2) + '\n')
        manifest(staged); replace_directory(staged, out)
    finally: shutil.rmtree(staged, ignore_errors=True)
    print(f'kit: {len(files)} source/resource files, build {build}, commit {commit}', flush=True)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('out', type=Path); ap.add_argument('--metallib', type=Path, required=True); args = ap.parse_args()
    try: export(args.out, args.metallib)
    except (OSError, ValueError) as e: raise SystemExit(f'kit: {e}') from e
