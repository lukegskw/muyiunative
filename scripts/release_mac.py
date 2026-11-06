#!/usr/bin/env python3
# Phase 7's maintainer release: build the launcher, sign with a local Developer ID, notarize, staple, then verify an extracted ZIP before making it available.
# Apple documents the required timestamp, hardened runtime and stapling at https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution
# Signing keys stay in the user's keychain. This command creates a local archive; it never publishes a GitHub release or uploads game apps.
import argparse, hashlib, json, os, plistlib, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from build_kit import manifest, source_files
from version import metadata
import sparkle
import update_feed


def run(*command):
    result = subprocess.run([str(c) for c in command], capture_output=True, text=True)
    if result.returncode: raise SystemExit(f'release: {Path(str(command[0])).name} failed: {(result.stderr or result.stdout).strip()[-2500:]}')
    return result.stdout


def identity(requested):
    for line in run('security', 'find-identity', '-v', '-p', 'codesigning').splitlines():
        match = re.search(r'([0-9A-F]{40})\s+"(Developer ID Application: [^"]+)"', line)
        if match and requested in (match[1], match[2]): return match[1]
    raise SystemExit('release: no valid local Developer ID Application identity matches --identity; create or import it with its private key in Xcode first')


def verify_app(app, expected):
    info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
    actual = info.get('CFBundleShortVersionString'), info.get('CFBundleVersion'), info.get('MuyiunativeCommit')
    if actual != expected or info.get('CFBundleIdentifier') != 'org.muyiunative.muyiunative' or info.get('CFBundleExecutable') != 'muyiunative':
        raise SystemExit('release: the bundle is not the expected version of the generic launcher')
    if any(info.get(key) != value for key, value in sparkle.settings().items()): raise SystemExit('release: missing or incorrect signed update configuration')
    resources = app / 'Contents/Resources'
    if {p.name for p in resources.iterdir()} != {'BuildKit', 'openbios', 'ps1gpu.metallib'}:
        raise SystemExit('release: unexpected launcher resources; game apps and disc data must never be distributed')
    kit = resources / 'BuildKit'; data = manifest(kit)
    expected_files = {p.relative_to(ROOT).as_posix() for p in source_files()} | {'gfx/metal/ps1gpu.metallib'}
    if set(data['Files']) != expected_files or (data['AppVersion'], data['BuildVersion'], data['Commit']) != expected:
        raise SystemExit('release: the build kit does not match the release sources and identity')
    bios_files = {p.relative_to(resources / 'openbios').as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in (resources / 'openbios').rglob('*') if p.is_file()}
    expected_bios = {name.removeprefix('bios/openbios/'): digest for name, digest in data['Files'].items() if name.startswith('bios/openbios/')}
    if bios_files != expected_bios or hashlib.sha256((resources / 'ps1gpu.metallib').read_bytes()).hexdigest() != data['Files']['gfx/metal/ps1gpu.metallib']:
        raise SystemExit('release: presentation resources differ from the source kit')
    if hashlib.sha256((resources / 'openbios/openbios.bin').read_bytes()).hexdigest() != 'caf2162eb498408f86e19f9b3a3c5e083e34c25a43baae33ffc3641afffa54c8':
        raise SystemExit('release: the bundled OpenBIOS is not the documented source-built image')
    framework = app / 'Contents/Frameworks/Sparkle.framework'
    if framework.is_symlink() or not framework.is_dir() or {p.name for p in framework.parent.iterdir()} != {'Sparkle.framework'}:
        raise SystemExit('release: missing or unexpected updater framework')
    macho = []
    for path in app.rglob('*'):
        if path.is_symlink():
            if not path.is_relative_to(framework) or not path.resolve().is_relative_to(framework.resolve()): raise SystemExit('release: unexpected symbolic link in the launcher')
            continue
        if path.is_file():
            with path.open('rb') as f: magic = f.read(4)
            if magic in (b'\xcf\xfa\xed\xfe', b'\xfe\xed\xfa\xcf', b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'): macho.append(path)
    executable = app / 'Contents/MacOS/muyiunative'
    if set(macho) != {executable, *(framework / name for name in sparkle.MACHO)} or run('lipo', '-archs', executable).strip() != 'arm64':
        raise SystemExit('release: expected exactly one arm64 host and the allowlisted Sparkle executables')
    run('codesign', '--verify', '--strict', '--deep', app)
    boot = run(executable, '--headless', '--frames', '60')
    if 'frame hash 49d1a17d49708cb1' not in boot: raise SystemExit('release: the launcher did not pass its bundled OpenBIOS boot')


def archive(app, output):
    run('ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', app, output)


def notarize(app, submission, certificate, profile):
    print('release: signing the launcher with Developer ID and a secure timestamp', flush=True)
    run('strip', '-S', '-x', app / 'Contents/MacOS/muyiunative')
    sparkle.sign(app / 'Contents/Frameworks/Sparkle.framework', certificate, lambda command: run(*command), timestamp=True)
    run('codesign', '--force', '--options', 'runtime', '--timestamp', '--sign', certificate, app)
    run('codesign', '--verify', '--strict', app)
    archive(app, submission)
    print('release: submitting the launcher to Apple for notarization', flush=True)
    result = json.loads(run('xcrun', 'notarytool', 'submit', submission, '--keychain-profile', profile, '--wait', '--output-format', 'json'))
    if result.get('status') != 'Accepted':
        raise SystemExit(f'release: notarization was not accepted (submission {result.get("id", "unknown")}, status {result.get("status", "unknown")}); retrieve its log with notarytool before retrying')
    print(f'release: notarization accepted, submission {result["id"]}', flush=True)
    run('xcrun', 'stapler', 'staple', app)
    run('xcrun', 'stapler', 'validate', app)
    run('spctl', '--assess', '--type', 'execute', '--verbose=2', app)
    return result['id']


def release(output, requested, profile):
    output = Path(output).expanduser().resolve()
    feed_output = output.with_suffix('.appcast.xml')
    if output.suffix != '.zip' or output.exists() or output.with_suffix('.zip.sha256').exists() or feed_output.exists():
        raise SystemExit('release: choose a new --output ZIP path; existing archives and checksums are never replaced')
    if run('git', '-C', ROOT, 'status', '--porcelain', '--untracked-files=normal').strip():
        raise SystemExit('release: commit the release sources first; a download must identify a clean source revision')
    expected = metadata(); certificate = identity(requested)
    # Fail before building when local notarization credentials are missing or invalid. No password or API key is accepted on this command line.
    run('xcrun', 'notarytool', 'history', '--keychain-profile', profile, '--output-format', 'json')
    mac = ROOT / 'shell/mac'; (mac / 'build').mkdir(exist_ok=True); output.parent.mkdir(parents=True, exist_ok=True)
    tools = sparkle.fetch(mac / 'build/sparkle') / 'bin'
    if run(tools / 'generate_keys', '--account', sparkle.ACCOUNT, '-p').strip() != sparkle.CONFIG['public_key']:
        raise SystemExit('release: the local Sparkle signing key does not match the bundled public key')
    with tempfile.TemporaryDirectory(prefix='release-', dir=mac / 'build') as directory:
        work = Path(directory); app = work / 'muyiunative.app'
        print(f'release: building muyiunative {expected[0]} (build {expected[1]}), commit {expected[2]}', flush=True)
        run('make', '-C', mac, f'B={work.relative_to(mac)}', 'app')
        verify_app(app, expected)
        notarize(app, work / 'submission.zip', certificate, profile)
        final = work / output.name; archive(app, final)
        extracted = work / 'extracted'; extracted.mkdir()
        run('ditto', '-x', '-k', final, extracted)
        print('release: verifying the extracted download, signature, ticket, kit and bundled boot', flush=True)
        unpacked = extracted / 'muyiunative.app'; verify_app(unpacked, expected)
        run('xcrun', 'stapler', 'validate', unpacked)
        run('spctl', '--assess', '--type', 'execute', '--verbose=2', unpacked)
        digest = hashlib.sha256(final.read_bytes()).hexdigest()
        signature = run(tools / 'sign_update', '--account', sparkle.ACCOUNT, '-p', final).strip()
        feed = work / feed_output.name
        update_feed.create(final, feed, expected[0], expected[1], plistlib.loads((app / 'Contents/Info.plist').read_bytes())['LSMinimumSystemVersion'], signature)
        run(tools / 'sign_update', '--account', sparkle.ACCOUNT, feed)
        run(tools / 'sign_update', '--account', sparkle.ACCOUNT, '--verify', feed)
        # Exclusive creation avoids replacing another run's output, including when two release commands finish concurrently.
        created = []
        try:
            with tempfile.TemporaryDirectory(prefix='.muyiunative-release-', dir=output.parent) as staged:
                checksum = Path(staged) / output.with_suffix('.zip.sha256').name; checksum.write_text(f'{digest}  {output.name}\n')
                for source, dest in [(final, output), (checksum, output.with_suffix('.zip.sha256')), (feed, feed_output)]:
                    copied = Path(staged) / dest.name
                    if source != copied: shutil.copyfile(source, copied)
                    os.link(copied, dest); created.append(dest)
        except BaseException:
            for path in created: path.unlink()
            raise
    print(f'release: ready {output}\nrelease: SHA-256 {digest}\nrelease: signed feed {feed_output}\nrelease: publish the launcher ZIP, checksum and signed appcast after review', flush=True)
    return output


def parse(argv=None):
    p = argparse.ArgumentParser(description='Build a Developer ID-signed, notarized launcher ZIP without publishing it')
    p.add_argument('--output', required=True, type=Path, help='new ZIP path selected by the maintainer')
    p.add_argument('--identity', required=True, help='local Developer ID Application identity name or SHA-1 fingerprint')
    p.add_argument('--notary-profile', required=True, help='existing notarytool keychain-profile name; never a password or API key')
    return p.parse_args(argv)


if __name__ == '__main__':
    try:
        args = parse(); release(args.output, args.identity, args.notary_profile)
    except KeyboardInterrupt: raise SystemExit('release: cancelled') from None
    except (OSError, ValueError) as error: raise SystemExit(f'release: {error}') from None
