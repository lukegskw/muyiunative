#!/usr/bin/env python3
# Pack a Mac app (plan.md, phases 0 and 5): the host as the bundle's executable, an Info.plist whose LSMinimumSystemVersion is the deployment target the host was linked for
# (LC_BUILD_VERSION), PkgInfo, and the signature: codesign --force --options runtime --sign <identity> ('-' = ad-hoc). The hardened runtime is on, as nothing is generated or
# mapped executable at run time (plan.md, core rule). Phase 0's app is the host alone, which opens an empty window; the game's code and data join it from phase 5 on.
# The app is written to <out.app>.partial, then moved into place, so a failed pack leaves the previous app as it was. An existing <out.app> is replaced only when it carries
# the same bundle ID: the script never deletes an app it did not pack.
# Usage: pack_app.py <out.app> --exe <host> [--name ps1native] [--bundle-id org.ps1native.<name>] [--version 0.0] [--sign -]
import argparse, plistlib, re, shutil, subprocess, sys
from pathlib import Path
from xml.parsers.expat import ExpatError


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')
    return r.stdout


def min_os(exe):   # the deployment target the host was linked for: Info.plist says the same
    m = re.search(r'cmd LC_BUILD_VERSION.*?minos (\S+)', run(['otool', '-l', exe]), re.S)
    if not m: raise SystemExit(f'no LC_BUILD_VERSION in {exe}: not a Mac executable')
    return m.group(1)


def bundle_id(a): return a.bundle_id or 'org.ps1native.' + (re.sub(r'[^a-z0-9]+', '-', a.name.lower()).strip('-') or 'app')


def info_plist(a, minos):
    return {'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleIdentifier': bundle_id(a), 'CFBundleVersion': a.version, 'CFBundleShortVersionString': a.version,
            'CFBundlePackageType': 'APPL', 'CFBundleExecutable': a.name, 'CFBundleInfoDictionaryVersion': '6.0', 'LSMinimumSystemVersion': minos,
            'LSArchitecturePriority': ['arm64'], 'NSHighResolutionCapable': True, 'LSApplicationCategoryType': 'public.app-category.games'}


def packed_id(app):   # the bundle ID of an existing app; None when it is not one
    try: return plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier')
    except (OSError, ValueError, ExpatError): return None


def pack(a):
    out, exe = Path(a.out).resolve(), Path(a.exe).resolve()
    if out.suffix != '.app': raise SystemExit(f'the output must end in .app: {out}')
    if not exe.is_file(): raise SystemExit(f'no host at {exe}')
    if not a.name.strip() or '/' in a.name: raise SystemExit(f'not a name for an app: {a.name!r}')
    if out.exists() and packed_id(out) != bundle_id(a): raise SystemExit(f'{out} exists and is not an app with bundle ID {bundle_id(a)}: remove it, or pack elsewhere')
    minos = min_os(exe)
    tmp = out.with_name(out.name + '.partial'); shutil.rmtree(tmp, ignore_errors=True)
    c = tmp / 'Contents'; (c / 'MacOS').mkdir(parents=True)
    shutil.copyfile(exe, c / 'MacOS' / a.name); (c / 'MacOS' / a.name).chmod(0o755)
    (c / 'Info.plist').write_bytes(plistlib.dumps(info_plist(a, minos)))
    (c / 'PkgInfo').write_text('APPL????')
    run(['codesign', '--force', '--options', 'runtime', '--sign', a.sign, tmp])
    if out.exists(): shutil.rmtree(out)
    tmp.rename(out)
    print(f'app: {out}', flush=True)


def parse(argv):
    ap = argparse.ArgumentParser(prog='pack_app.py', description='Pack and sign a Mac app of the host')
    ap.add_argument('out', help='the app (<name>.app)')
    ap.add_argument('--exe', required=True, help='the host (shell/mac/build/ps1native)')
    ap.add_argument('--name', default='ps1native', help="the app's name, also its executable's (default: ps1native)")
    ap.add_argument('--bundle-id', help='default: org.ps1native.<name>')
    ap.add_argument('--version', default='0.0')
    ap.add_argument('--sign', default='-', help='codesign identity (default: - = ad-hoc)')
    return ap.parse_args(argv)


if __name__ == '__main__':
    pack(parse(sys.argv[1:]))
