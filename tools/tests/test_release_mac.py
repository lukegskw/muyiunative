"""Maintainer release safety: notarization must be accepted and stapled before the final ZIP, with an extracted-copy check and no replacement of existing downloads.
Apple services/signing commands are simulated; fixtures use synthetic host bytes and the permitted source-built OpenBIOS. No credentials, retail files or GPU."""
import hashlib, json, plistlib, shutil, sys, tempfile, unittest, zipfile
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))
from scripts import release_mac as release
import sparkle


class Release(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup); self.root = Path(self.tmp.name)
        self.expected = ('0.7.0', '63', 'abcdef123456'); self.status = 'Accepted'; self.calls = []; self.dirty = False
        self.seed = self.root / 'seed.app'; contents = self.seed / 'Contents'; resources = contents / 'Resources'; kit = resources / 'BuildKit'
        files = {'scripts/muyiunative': b'#!/bin/sh\n', 'tools/player_build.py': b'# synthetic tool\n', 'shell/mac/Makefile': b'# synthetic make\n',
                 'gfx/metal/ps1gpu.metallib': b'synthetic shader', 'bios/openbios/openbios.bin': (REPO / 'bios/openbios/openbios.bin').read_bytes()}
        for name, data in files.items():
            path = kit / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(data)
        (kit / 'build-kit.json').write_text(json.dumps({'KitVersion': 1, 'AppVersion': self.expected[0], 'BuildVersion': self.expected[1], 'Commit': self.expected[2],
                                                     'Files': {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}}))
        shutil.copytree(kit / 'bios/openbios', resources / 'openbios'); shutil.copyfile(kit / 'gfx/metal/ps1gpu.metallib', resources / 'ps1gpu.metallib')
        (contents / 'Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'org.muyiunative.muyiunative', 'CFBundleExecutable': 'muyiunative',
            'CFBundleShortVersionString': self.expected[0], 'CFBundleVersion': self.expected[1], 'MuyiunativeCommit': self.expected[2], 'LSMinimumSystemVersion': '14.0', **sparkle.settings()}))
        (contents / 'MacOS').mkdir(); (contents / 'MacOS/muyiunative').write_bytes(b'\xcf\xfa\xed\xfe' + b'synthetic host')
        framework = contents / 'Frameworks/Sparkle.framework'
        for name in sparkle.MACHO:
            path = framework / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'\xcf\xfa\xed\xfe' + b'synthetic updater')
        (self.root / 'shell/mac').mkdir(parents=True)
        mock = patch.object(sparkle, 'fetch', lambda out: self.root / 'sparkle'); mock.start(); self.addCleanup(mock.stop)
        for name, value in [('ROOT', self.root), ('metadata', lambda: self.expected), ('source_files', lambda: [self.root / n for n in files if n != 'gfx/metal/ps1gpu.metallib']), ('run', self.run_command)]:
            mock = patch.object(release, name, value); mock.start(); self.addCleanup(mock.stop)

    def run_command(self, *command):
        c = tuple(map(str, command)); self.calls.append(c)
        if c[0].endswith('/generate_keys'): return sparkle.CONFIG['public_key'] + '\n'
        if c[0].endswith('/sign_update'): return 'A' * 86 + '==' if '-p' in c else ''
        if c[0] == 'git': return ' M changed.py' if self.dirty else ''
        if c[0] == 'security': return '  1) ' + 'A' * 40 + ' "Developer ID Application: Example (TEAM)"\n'
        if c[0] == 'make':
            folder = self.root / 'shell/mac' / next(v[2:] for v in c if v.startswith('B=')); shutil.copytree(self.seed, folder / 'muyiunative.app'); return ''
        if c[:3] == ('xcrun', 'notarytool', 'submit'): return json.dumps({'id': 'synthetic-submission', 'status': self.status})
        if c[:3] == ('xcrun', 'stapler', 'staple'): (Path(c[3]) / 'Contents/ticket').write_text('synthetic notarization ticket')
        if c[:3] == ('xcrun', 'stapler', 'validate'): self.assertTrue((Path(c[3]) / 'Contents/ticket').is_file())
        if c[0] == 'spctl': self.assertTrue((Path(c[-1]) / 'Contents/ticket').is_file())
        if c[0] == 'lipo': return 'arm64\n'
        if c[0].endswith('/MacOS/muyiunative'): return 'host: 60 frames, 640x480, frame hash 49d1a17d49708cb1\n'
        if c[0] == 'ditto':
            source, dest = Path(c[-2]), Path(c[-1])
            if '-c' in c:
                with zipfile.ZipFile(dest, 'w') as z:
                    for p in source.rglob('*'):
                        if p.is_file(): z.write(p, p.relative_to(source.parent))
            else:
                with zipfile.ZipFile(source) as z: z.extractall(dest)
        return ''

    def test_accepted_release_contains_stapled_app_and_checked_checksum(self):
        out = self.root / 'downloads/preview.zip'; release.release(out, 'A' * 40, 'local-profile')
        with zipfile.ZipFile(out) as z: self.assertEqual(z.read('muyiunative.app/Contents/ticket'), b'synthetic notarization ticket')
        self.assertEqual(out.with_suffix('.zip.sha256').read_text(), f'{hashlib.sha256(out.read_bytes()).hexdigest()}  preview.zip\n')
        self.assertTrue(out.with_suffix('.appcast.xml').is_file())
        self.assertTrue(any(c[0].endswith('/sign_update') and '--verify' in c for c in self.calls))
        signing = next(c for c in self.calls if c[:2] == ('codesign', '--force'))
        self.assertIn('--timestamp', signing); self.assertIn('runtime', signing)
        boots = [c for c in self.calls if c[0].endswith('/MacOS/muyiunative')]
        self.assertEqual(len(boots), 2); self.assertIn('/extracted/', boots[1][0])
        self.assertEqual(list((self.root / 'shell/mac/build').iterdir()), [])

    def test_rejected_notarization_never_creates_download(self):
        self.status = 'Invalid'; out = self.root / 'downloads/preview.zip'
        with self.assertRaisesRegex(SystemExit, 'not accepted'): release.release(out, 'A' * 40, 'local-profile')
        self.assertFalse(out.exists()); self.assertFalse(out.with_suffix('.zip.sha256').exists())
        self.assertFalse(any(c[:3] == ('xcrun', 'stapler', 'staple') for c in self.calls))
        self.assertEqual(list((self.root / 'shell/mac/build').iterdir()), [])

    def test_existing_download_and_dirty_sources_are_refused(self):
        out = self.root / 'preview.zip'; out.write_bytes(b'keep')
        with self.assertRaisesRegex(SystemExit, 'never replaced'): release.release(out, 'A' * 40, 'local-profile')
        self.assertEqual(out.read_bytes(), b'keep')
        self.dirty = True
        with self.assertRaisesRegex(SystemExit, 'clean source revision'): release.release(self.root / 'other.zip', 'A' * 40, 'local-profile')

    def test_release_requires_developer_id_and_refuses_game_resources(self):
        with self.assertRaisesRegex(SystemExit, 'Developer ID Application'): release.identity('-')
        self.assertEqual(release.identity('Developer ID Application: Example (TEAM)'), 'A' * 40)
        (self.seed / 'Contents/Resources/game.plist').write_text('synthetic metadata')
        with self.assertRaisesRegex(SystemExit, 'unexpected launcher resources'): release.verify_app(self.seed, self.expected)

    def test_changed_resources_or_an_unexpected_native_binary_are_refused(self):
        shader = self.seed / 'Contents/Resources/ps1gpu.metallib'; original = shader.read_bytes(); shader.write_bytes(b'changed')
        with self.assertRaisesRegex(SystemExit, 'resources differ'): release.verify_app(self.seed, self.expected)
        shader.write_bytes(original); (self.seed / 'Contents/extra').write_bytes(b'\xcf\xfa\xed\xfeunexpected executable')
        with self.assertRaisesRegex(SystemExit, 'exactly one'): release.verify_app(self.seed, self.expected)

    def test_framework_links_must_stay_inside_the_framework(self):
        framework = self.seed / 'Contents/Frameworks/Sparkle.framework'
        (framework / 'Sparkle').symlink_to('Versions/B/Sparkle'); release.verify_app(self.seed, self.expected)
        (framework / 'outside').symlink_to(self.root)
        with self.assertRaisesRegex(SystemExit, 'symbolic link'): release.verify_app(self.seed, self.expected)


if __name__ == '__main__': unittest.main()
