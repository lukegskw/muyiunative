#!/usr/bin/env python3
# Launcher-only Sparkle dependency. Verify the pinned official archive before extraction; game builds never fetch or embed it. Private update keys stay in Keychain.
import argparse, hashlib, json, shutil, tarfile, tempfile, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIG = json.loads((ROOT / 'tools/vendor/sparkle/config.json').read_text())
FEED = 'https://raw.githubusercontent.com/lukegskw/muyiunative/updates/appcast.xml'
ACCOUNT = 'org.muyiunative.updates'
FRAMEWORK_CODE = ['Versions/B/Autoupdate', 'Versions/B/Updater.app', '.']
MACHO = ['Versions/B/Autoupdate', 'Versions/B/Updater.app/Contents/MacOS/Updater', 'Versions/B/Sparkle']


def settings():
    return {'SUFeedURL': FEED, 'SUPublicEDKey': CONFIG['public_key'], 'SUEnableAutomaticChecks': True,
            'SUAutomaticallyUpdate': False, 'SUAllowsAutomaticUpdates': False, 'SUEnableSystemProfiling': False,
            'SUVerifyUpdateBeforeExtraction': True, 'SURequireSignedFeed': True}


def fetch(out):
    out = Path(out).resolve(); stamp = out / '.archive-sha256'
    if stamp.is_file() and stamp.read_text().strip() == CONFIG['sha256'] and (out / 'Sparkle.framework').is_dir(): return out
    out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sparkle-', dir=out.parent) as directory:
        temp = Path(directory); archive = temp / 'download.tar.xz'
        with urllib.request.urlopen(CONFIG['url'], timeout=60) as response, archive.open('wb') as dest:
            shutil.copyfileobj(response, dest)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != CONFIG['sha256']: raise ValueError('Sparkle archive checksum mismatch')
        extracted = temp / 'extracted'; extracted.mkdir()
        # Source app builds require Python >=3.12; player kits only import settings/signing, never this fetch path.
        with tarfile.open(archive) as source: source.extractall(extracted, filter='data')
        if not (extracted / 'Sparkle.framework/Versions/B/Sparkle').is_file(): raise ValueError('incomplete Sparkle distribution')
        (extracted / '.archive-sha256').write_text(CONFIG['sha256'] + '\n')
        if out.exists(): shutil.rmtree(out)
        extracted.rename(out)
    return out


def sign(framework, identity, run, timestamp=False):
    for name in FRAMEWORK_CODE:
        command = ['codesign', '--force', '--options', 'runtime', '--sign', identity]
        if timestamp: command += ['--timestamp']
        run([*command, Path(framework) / name])


def embed(source, contents, identity, run):
    target = Path(contents) / 'Frameworks/Sparkle.framework'; target.parent.mkdir()
    shutil.copytree(source, target, symlinks=True)
    # Sparkle's sandboxing guide explicitly permits dropping XPC services in an unsandboxed app. Its installer/relauncher remain, signed from the inside out.
    shutil.rmtree(target / 'Versions/B/XPCServices'); (target / 'XPCServices').unlink()
    sign(target, identity, run)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description='Fetch the checksum-pinned Sparkle framework and maintainer tools')
    p.add_argument('out', type=Path)
    try: print(fetch(p.parse_args().out))
    except (OSError, ValueError) as error: raise SystemExit(f'sparkle: {error}') from error
