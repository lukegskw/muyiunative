"""tools/pack_app.py turns a host into an app that macOS takes: the bundle's layout, an Info.plist with the host's deployment target, a signature with the hardened runtime
that codesign verifies, resources (a file and a folder) inside the signed bundle, a second pack over the first, and its refusals: an output that is not an .app, a missing
resource, and an existing app with another bundle ID, which stays as it was.
Needs Xcode's command-line tools (clang, otool, codesign); no game files."""
import argparse, os, plistlib, re, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch

PACK = Path(__file__).resolve().parents[1] / 'pack_app.py'
sys.path.insert(0, str(PACK.parent))
import pack_app, version


class Version(unittest.TestCase):
    def test_report_fields_exist_in_the_issue_form(self):
        form = (PACK.parents[1] / '.github/ISSUE_TEMPLATE/bug_report.yml').read_text()
        ids = re.findall(r'^    id: (\S+)$', form, re.M)
        self.assertEqual(len(ids), len(set(ids)), 'issue form IDs must be unique')
        self.assertTrue({'app-version', 'commit', 'device', 'game', 'renderer'} <= set(ids))

    def test_shared_version_and_full_history(self):
        release, build, commit = version.metadata()
        self.assertEqual(release, '0.9.0'); self.assertRegex(build, r'^[1-9][0-9]*$'); self.assertRegex(commit, r'^[0-9a-f]{12,40}$')
        self.assertEqual(build, subprocess.check_output(['git', '-C', version.ROOT, 'rev-list', '--count', 'HEAD'], text=True).strip())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'generated/version.h'; version.header(path); original = path.stat().st_mtime_ns
            version.header(path); self.assertEqual(path.stat().st_mtime_ns, original, 'unchanged metadata must not rebuild every host')
            self.assertIn(f'#define FRONT_COMMIT "{commit}"', path.read_text())

    def test_shallow_and_missing_history_fail_clearly(self):
        with patch('version.subprocess.run', return_value=subprocess.CompletedProcess([], 0, 'true\n', '')):
            with self.assertRaisesRegex(SystemExit, 'shallow history'): version.metadata()
        with patch('version.subprocess.run', return_value=subprocess.CompletedProcess([], 128, '', 'not a repository')):
            with self.assertRaisesRegex(SystemExit, 'cannot read Git history'): version.metadata()


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'needs macOS and its codesign')
class PackApp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'host.c'; src.write_text('int main(void) { return 0; }\n')
        cls.exe = cls.tmp / 'host'
        subprocess.run(['clang', '-arch', 'arm64', '-mmacosx-version-min=14.0', '-o', cls.exe, src], check=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def pack(self, out, *args):
        return subprocess.run([sys.executable, PACK, out, '--exe', self.exe, *args], capture_output=True, text=True)

    def test_layout_info_and_signature(self):
        app = self.tmp / 'Empty Game.app'
        r = self.pack(app, '--name', 'Empty Game'); self.assertEqual(r.returncode, 0, r.stderr)
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
        self.assertEqual((info['CFBundleExecutable'], info['CFBundleIdentifier'], info['LSMinimumSystemVersion'], info['CFBundlePackageType']),
                         ('Empty Game', 'org.muyiunative.empty-game', '14.0', 'APPL'))
        release, build, commit = version.metadata()
        self.assertEqual((info['CFBundleShortVersionString'], info['CFBundleVersion'], info['MuyiunativeCommit']), (release, build, commit))
        self.assertTrue(os.access(app / 'Contents/MacOS/Empty Game', os.X_OK))
        self.assertEqual((app / 'Contents/PkgInfo').read_text(), 'APPL????')
        v = subprocess.run(['codesign', '--verify', '--strict', '--verbose=2', app], capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stderr)
        shown = subprocess.run(['codesign', '--display', '--verbose=2', app], capture_output=True, text=True).stderr
        self.assertRegex(shown, r'flags=0x[0-9a-f]+\([^)]*runtime', 'signed without the hardened runtime')
        self.assertFalse(app.with_name(app.name + '.partial').exists())
        r = self.pack(app, '--name', 'Empty Game'); self.assertEqual(r.returncode, 0, f'a second pack over its own app: {r.stderr}')

    def test_version_override_keeps_the_build_identity(self):
        app = self.tmp / 'Version.app'; r = self.pack(app, '--version', '1.2.3'); self.assertEqual(r.returncode, 0, r.stderr)
        info = plistlib.loads((app / 'Contents/Info.plist').read_bytes()); _, build, commit = version.metadata()
        self.assertEqual((info['CFBundleShortVersionString'], info['CFBundleVersion'], info['MuyiunativeCommit']), ('1.2.3', build, commit))

    def test_cancelled_pack_preserves_the_previous_app(self):
        app = self.tmp / 'Cancel.app'; r = self.pack(app, '--name', 'Cancel'); self.assertEqual(r.returncode, 0, r.stderr)
        old = (app / 'Contents/Info.plist').read_bytes()
        args = argparse.Namespace(out=app, exe=self.exe, name='Cancel', bundle_id=None, resource=[], version=None, sign='-')
        with patch('pack_app.shutil.copyfile', side_effect=SystemExit('pack: cancelled')):
            with self.assertRaisesRegex(SystemExit, 'cancelled'): pack_app.pack(args)
        self.assertEqual((app / 'Contents/Info.plist').read_bytes(), old); self.assertFalse(list(self.tmp.glob('Cancel.app.partial-*')))
        subprocess.run(['codesign', '--verify', '--strict', app], check=True, capture_output=True)

    def test_directory_commit_swaps_both_names_atomically(self):
        staged, out = self.tmp / 'new-directory', self.tmp / 'old-directory'; staged.mkdir(); out.mkdir()
        (staged / 'new').write_text('new'); (out / 'old').write_text('old')
        pack_app.replace_directory(staged, out)
        self.assertEqual((out / 'new').read_text(), 'new'); self.assertEqual((staged / 'old').read_text(), 'old')

    def test_refuses_an_output_that_is_not_an_app(self):
        r = self.pack(self.tmp / 'Empty')
        self.assertNotEqual(r.returncode, 0); self.assertIn('must end in .app', r.stderr)

    def test_resources_inside_the_signature(self):
        data = self.tmp / 'data'; data.mkdir(); (data / 'rom.dat').write_bytes(b'\x00\x01'); (data / 'LICENSE').write_text('MIT\n')
        note = self.tmp / 'note.txt'; note.write_text('hello\n')
        app = self.tmp / 'Res.app'
        r = self.pack(app, '--name', 'Res', '--resource', data, '--resource', note); self.assertEqual(r.returncode, 0, r.stderr)
        res = app / 'Contents/Resources'
        self.assertEqual(((res / 'data/rom.dat').read_bytes(), (res / 'data/LICENSE').read_text(), (res / 'note.txt').read_text()), (b'\x00\x01', 'MIT\n', 'hello\n'))
        v = subprocess.run(['codesign', '--verify', '--strict', '--verbose=2', app], capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stderr)
        r = self.pack(self.tmp / 'Missing.app', '--name', 'Missing', '--resource', self.tmp / 'nothing')
        self.assertNotEqual(r.returncode, 0); self.assertIn('no resource', r.stderr)
        self.assertFalse((self.tmp / 'Missing.app').exists())

    def test_never_replaces_another_app(self):
        other = self.tmp / 'Other.app'; (other / 'Contents').mkdir(parents=True)
        (other / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': 'com.example.other'}))
        r = self.pack(other, '--name', 'Other')
        self.assertNotEqual(r.returncode, 0); self.assertIn('bundle ID', r.stderr)
        self.assertEqual(plistlib.loads((other / 'Contents/Info.plist').read_bytes()), {'CFBundleIdentifier': 'com.example.other'})

    def test_preserves_an_existing_partial_folder(self):
        app = self.tmp / 'Staging.app'; partial = app.with_name(app.name + '.partial'); partial.mkdir(); sentinel = partial / 'keep.txt'; sentinel.write_text('untouched')
        r = self.pack(app, '--name', 'Staging'); self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(sentinel.read_text(), 'untouched'); self.assertFalse(list(self.tmp.glob(app.name + '.partial-*')))

    def test_refuses_resources_that_overlap_the_output(self):
        resource = self.tmp / 'parent'; resource.mkdir(); note = resource / 'keep.txt'; note.write_text('untouched')
        r = self.pack(resource / 'Nested.app', '--resource', resource)
        self.assertNotEqual(r.returncode, 0); self.assertIn('must not contain each other', r.stderr); self.assertEqual(note.read_text(), 'untouched')
        app = self.tmp / 'Existing.app'; r = self.pack(app, '--name', 'Existing'); self.assertEqual(r.returncode, 0, r.stderr)
        r = self.pack(app, '--name', 'Existing', '--resource', app / 'Contents/Resources')
        self.assertNotEqual(r.returncode, 0); self.assertIn('must not contain each other', r.stderr)
        self.assertTrue((app / 'Contents/MacOS/Existing').is_file())


if __name__ == '__main__':
    unittest.main()
