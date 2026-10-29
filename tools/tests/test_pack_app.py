"""tools/pack_app.py turns a host into an app that macOS takes: the bundle's layout, an Info.plist with the host's deployment target, a signature with the hardened runtime
that codesign verifies, resources (a file and a folder) inside the signed bundle, a second pack over the first, and its refusals: an output that is not an .app, a missing
resource, and an existing app with another bundle ID, which stays as it was.
Needs Xcode's command-line tools (clang, otool, codesign); no game files."""
import os, plistlib, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

PACK = Path(__file__).resolve().parents[1] / 'pack_app.py'


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
                         ('Empty Game', 'org.ps1native.empty-game', '14.0', 'APPL'))
        self.assertTrue(os.access(app / 'Contents/MacOS/Empty Game', os.X_OK))
        self.assertEqual((app / 'Contents/PkgInfo').read_text(), 'APPL????')
        v = subprocess.run(['codesign', '--verify', '--strict', '--verbose=2', app], capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stderr)
        shown = subprocess.run(['codesign', '--display', '--verbose=2', app], capture_output=True, text=True).stderr
        self.assertRegex(shown, r'flags=0x[0-9a-f]+\([^)]*runtime', 'signed without the hardened runtime')
        self.assertFalse(app.with_name(app.name + '.partial').exists())
        r = self.pack(app, '--name', 'Empty Game'); self.assertEqual(r.returncode, 0, f'a second pack over its own app: {r.stderr}')

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


if __name__ == '__main__':
    unittest.main()
