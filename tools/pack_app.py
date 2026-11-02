#!/usr/bin/env python3
# Pack a Mac app (plan.md, phases 0 and 5): the host as the bundle's executable, an Info.plist whose LSMinimumSystemVersion is the deployment target the host was linked for
# (LC_BUILD_VERSION), PkgInfo, and the signature: codesign --force --options runtime --sign <identity> ('-' = ad-hoc). The hardened runtime is on, as nothing is generated or
# mapped executable at run time (plan.md, core rule). Phase 1's app is the host and its resources (the bundled OpenBIOS, with its licenses); the game's code and data join
# it from phase 5 on. A resource, file or folder, is copied into Contents/Resources under its own name.
# The app is written to a unique temporary directory beside <out.app>, then moved into place, so a failed pack leaves the previous app as it was. An existing <out.app> is replaced only when it carries
# the same bundle ID: the script never deletes an app it did not pack.
# Usage: pack_app.py <out.app> --exe <host> [--resource <path>]... [--name muyiunative] [--bundle-id org.muyiunative.<name>] [--version 0.7.0] [--sign -]
import argparse, ctypes, os, plistlib, re, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
from xml.parsers.expat import ExpatError
from version import metadata


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')
    return r.stdout


def min_os(exe):   # the deployment target the host was linked for: Info.plist says the same
    m = re.search(r'cmd LC_BUILD_VERSION.*?minos (\S+)', run(['otool', '-l', exe]), re.S)
    if not m: raise SystemExit(f'no LC_BUILD_VERSION in {exe}: not a Mac executable')
    return m.group(1)


def bundle_id(a): return a.bundle_id or 'org.muyiunative.' + (re.sub(r'[^a-z0-9]+', '-', a.name.lower()).strip('-') or 'app')


def info_plist(a, minos):
    version, build, commit = metadata()
    return {'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleIdentifier': bundle_id(a), 'CFBundleVersion': build, 'CFBundleShortVersionString': a.version or version,
            'MuyiunativeCommit': commit,
            'CFBundlePackageType': 'APPL', 'CFBundleExecutable': a.name, 'CFBundleInfoDictionaryVersion': '6.0', 'LSMinimumSystemVersion': minos,
            'LSArchitecturePriority': ['arm64'], 'NSHighResolutionCapable': True, 'LSApplicationCategoryType': 'public.app-category.games'}


def packed_id(app):   # the bundle ID of an existing app; None when it is not one
    try: return plistlib.loads((app / 'Contents/Info.plist').read_bytes()).get('CFBundleIdentifier')
    except (OSError, ValueError, ExpatError): return None


def replace_directory(staged, out):
    # The former rmtree(out) then rename(staged) left a cancellation window with no app at out. Darwin renamex_np(RENAME_SWAP) commits both names atomically.
    if not out.exists(): staged.rename(out); return
    libc = ctypes.CDLL(None, use_errno=True); rename = libc.renamex_np
    rename.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint]; rename.restype = ctypes.c_int
    if rename(os.fsencode(staged), os.fsencode(out), 2):
        error = ctypes.get_errno(); raise OSError(error, os.strerror(error))


def pack(a):
    out, exe = Path(a.out).resolve(), Path(a.exe).resolve()
    if out.suffix != '.app': raise SystemExit(f'the output must end in .app: {out}')
    if not exe.is_file(): raise SystemExit(f'no host at {exe}')
    if not a.name.strip() or '/' in a.name: raise SystemExit(f'not a name for an app: {a.name!r}')
    res = [Path(r).resolve() for r in a.resource]
    for r in res:
        if not r.exists(): raise SystemExit(f'no resource at {r}')
    if len({r.name for r in res}) < len(res): raise SystemExit('two resources with one name: ' + ', '.join(str(r) for r in res))
    if any(out.is_relative_to(r) or r.is_relative_to(out) for r in res): raise SystemExit('the output app and its resources must not contain each other')
    if out.exists() and packed_id(out) != bundle_id(a): raise SystemExit(f'{out} exists and is not an app with bundle ID {bundle_id(a)}: remove it, or pack elsewhere')
    minos = min_os(exe)
    # The former fixed .partial path erased any preexisting folder at that name and let concurrent builds erase each other's staging directories.
    out.parent.mkdir(parents=True, exist_ok=True); tmp = Path(tempfile.mkdtemp(prefix=out.name + '.partial-', dir=out.parent))
    try:
        c = tmp / 'Contents'; (c / 'MacOS').mkdir(parents=True); (c / 'Resources').mkdir()
        shutil.copyfile(exe, c / 'MacOS' / a.name); (c / 'MacOS' / a.name).chmod(0o755)
        for r in res:
            if r.is_dir(): shutil.copytree(r, c / 'Resources' / r.name)
            else: shutil.copyfile(r, c / 'Resources' / r.name)
        (c / 'Info.plist').write_bytes(plistlib.dumps(info_plist(a, minos)))
        (c / 'PkgInfo').write_text('APPL????')
        run(['codesign', '--force', '--options', 'runtime', '--sign', a.sign, tmp])
        replace_directory(tmp, out)
    finally: shutil.rmtree(tmp, ignore_errors=True)
    print(f'app: {out}', flush=True)


def parse(argv):
    ap = argparse.ArgumentParser(prog='pack_app.py', description='Pack and sign a Mac app of the host')
    ap.add_argument('out', help='the app (<name>.app)')
    ap.add_argument('--exe', required=True, help='the host (shell/mac/build/muyiunative)')
    ap.add_argument('--resource', action='append', default=[], help='a file or folder for Contents/Resources (repeatable)')
    ap.add_argument('--name', default='muyiunative', help="the app's name, also its executable's (default: muyiunative)")
    ap.add_argument('--bundle-id', help='default: org.muyiunative.<name>')
    ap.add_argument('--version', help='release version (default: the version in tools/version.py)')
    ap.add_argument('--sign', default='-', help='codesign identity (default: - = ad-hoc)')
    return ap.parse_args(argv)


if __name__ == '__main__':
    def cancelled(signum, frame): raise SystemExit('pack: cancelled')
    signal.signal(signal.SIGTERM, cancelled)
    pack(parse(sys.argv[1:]))
