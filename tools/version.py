#!/usr/bin/env python3
# Phase 7's app version, approved 2026-11-02: release number, commit count and short revision. The packer and developer host share this source.
import argparse, json, re, subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = '0.7.0'


def metadata(root=ROOT):
    manifest = Path(root) / 'build-kit.json'
    if manifest.is_file():
        data = json.loads(manifest.read_text())
        values = data.get('AppVersion'), data.get('BuildVersion'), data.get('Commit')
        if data.get('KitVersion') != 1 or not all(isinstance(v, str) for v in values) or not re.fullmatch(r'\d+\.\d+\.\d+', values[0] or '') or not re.fullmatch(r'[1-9]\d*', values[1] or '') or not re.fullmatch(r'[0-9a-f]{12,40}', values[2] or ''):
            raise SystemExit('version: invalid build-kit identity')
        return values
    def git(*args):
        r = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
        if r.returncode: raise SystemExit('version: cannot read Git history; build from a checkout with its complete history')
        return r.stdout.strip()
    if git('rev-parse', '--is-shallow-repository') == 'true': raise SystemExit('version: shallow history cannot supply the build number; run git fetch --unshallow')
    return VERSION, git('rev-list', '--count', 'HEAD'), git('rev-parse', '--short=12', 'HEAD')


def header(path):
    version, build, commit = metadata()
    text = '// Build metadata from tools/version.py; generated only on the Mac.\n' + ''.join(
        f'#define FRONT_{key} {json.dumps(value)}\n' for key, value in [('VERSION', version), ('BUILD', build), ('COMMIT', commit)])
    path = Path(path); path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists() or path.read_text() != text: path.write_text(text)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(); ap.add_argument('--header', required=True, type=Path); header(ap.parse_args().header)
