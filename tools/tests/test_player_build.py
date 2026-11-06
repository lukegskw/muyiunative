"""Phase 7's exact GUI entry point: a signed launcher's exported kit builds a synthetic disc with CLT Python, without Git or Metal compilation. Failure and cancellation
clean their unique workspaces, including compiler grandchildren; existing apps and the source kit survive. No game files, retail BIOS, GUI or GPU are needed."""
import argparse, hashlib, json, os, plistlib, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import build_kit, player_build, version
from test_mac_build import create_iso, product_exe


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'needs Apple build/signing tools')
class PlayerBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='muyiunative-player-test-')); self.kit = self.tmp / 'kit with spaces'
        self.lib = self.tmp / 'ps1gpu.metallib'; self.lib.write_bytes(b'synthetic offline library')
        build_kit.export(self.kit, self.lib); self.cache = self.tmp / 'cache with spaces'; self.out = self.tmp / 'A Game.app'
        self.source = self.tmp / 'synthetic.bin'; self.source.write_bytes(b'')

    def tearDown(self): shutil.rmtree(self.tmp)

    def rewrite_worker(self, text):
        script = self.kit / 'scripts/muyiunative'; script.write_text(text)
        path = self.kit / 'build-kit.json'; data = json.loads(path.read_text()); data['Files']['scripts/muyiunative'] = hashlib.sha256(script.read_bytes()).hexdigest()
        path.write_text(json.dumps(data))

    def command(self, python=sys.executable):
        return [str(python), '-B', str(self.kit / 'tools/player_build.py'), str(self.source), '--output', str(self.out), '--work-root', str(self.cache)]

    def test_kit_allowlist_identity_and_integrity(self):
        data = build_kit.manifest(self.kit); self.assertEqual(version.metadata(self.kit), version.metadata())
        self.assertIn('tools/vendor/tomli/LICENSE', data['Files']); self.assertIn('bios/openbios/LICENSE', data['Files'])
        self.assertFalse(any('/tests/' in s or '/build/' in s or s.endswith(('.cue', '.metal', '.pyc')) for s in data['Files']))
        before = dict(data['Files']); build_kit.export(self.kit, self.lib); self.assertEqual(build_kit.manifest(self.kit)['Files'], before)
        extra = self.kit / 'unexpected.py'; extra.write_text('')
        with self.assertRaisesRegex(ValueError, 'unexpected files'): build_kit.manifest(self.kit)
        extra.unlink(); (self.kit / 'runtime/cpu.c').write_text('changed')
        with self.assertRaisesRegex(ValueError, 'changed build kit'): build_kit.manifest(self.kit)

    def test_manifest_refuses_traversal_and_missing_files(self):
        path = self.kit / 'build-kit.json'; data = json.loads(path.read_text()); data['Files']['../outside'] = '0' * 64; path.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, 'invalid build-kit path'): build_kit.manifest(self.kit)

    def test_failure_cleans_cache_and_preserves_existing_app(self):
        self.rewrite_worker('import sys\nprint("synthetic build failure", flush=True)\nsys.exit(7)\n')
        self.out.mkdir(); sentinel = self.out / 'keep'; sentinel.write_text('previous app')
        r = subprocess.run(self.command(), capture_output=True, text=True, timeout=20)
        self.assertEqual(r.returncode, 1, r.stdout + r.stderr); self.assertIn('synthetic build failure', r.stdout)
        self.assertEqual(list(self.cache.iterdir()), []); self.assertEqual(sentinel.read_text(), 'previous app'); build_kit.manifest(self.kit)

    def test_cancel_stops_a_compiler_grandchild_and_cleans_cache(self):
        child = 'import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print("ready",flush=True); time.sleep(120)'
        self.rewrite_worker('import subprocess,sys,time\np = subprocess.Popen([sys.executable,"-c",' + repr(child) + '],stdout=subprocess.PIPE,text=True)\np.stdout.readline()\nprint("child="+str(p.pid),flush=True)\ntime.sleep(120)\n')
        self.out.mkdir(); sentinel = self.out / 'keep'; sentinel.write_text('previous app')
        with subprocess.Popen(self.command(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True) as process:
            assert process.stdout is not None
            line = process.stdout.readline()
            while line and not line.startswith('child='): line = process.stdout.readline()
            self.assertTrue(line.startswith('child='), line); pid = int(line.split('=')[1])
            process.terminate(); output, _ = process.communicate(timeout=15)
            self.assertEqual(process.returncode, 130, output); self.assertIn('cancelled', output)
        status = subprocess.run(['ps', '-p', str(pid), '-o', 'stat='], capture_output=True, text=True).stdout.strip()
        self.assertTrue(not status or status.startswith('Z'), f'compiler child still running: {status}')
        self.assertEqual(list(self.cache.iterdir()), []); self.assertEqual(sentinel.read_text(), 'previous app')

    def test_output_cannot_be_placed_in_the_disposable_cache(self):
        args = argparse.Namespace(disc=self.source, output=self.cache / 'bad.app', work_root=self.cache, frames=None, profile=None)
        with self.assertRaisesRegex(ValueError, 'outside the kit'): player_build.build(args, self.kit)
        self.assertFalse(self.cache.exists())

    def test_disc_list_is_validated_and_forwarded_in_order(self):
        second = self.tmp / 'second.bin'; second.write_bytes(b'synthetic')
        self.rewrite_worker('import sys\nfrom pathlib import Path\nargs=sys.argv[1:]\nassert args[0]=="build"\nassert args[args.index("--disc")+1].endswith("second.bin")\nPath(args[args.index("--output")+1]).mkdir()\n')
        result = subprocess.run([*self.command(), '--disc', second], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr); self.assertTrue(self.out.is_dir()); self.assertEqual(list(self.cache.iterdir()), [])
        result = subprocess.run([*self.command(), '--disc', self.source], capture_output=True, text=True, timeout=20)
        self.assertNotEqual(result.returncode, 0); self.assertIn('duplicate paths', result.stderr)

    def test_exported_identity_needs_no_git(self):
        with patch('version.subprocess.run', side_effect=AssertionError('Git must not run inside a kit')):
            self.assertEqual(version.metadata(self.kit), ('0.9.1', json.loads((self.kit / 'build-kit.json').read_text())['BuildVersion'], json.loads((self.kit / 'build-kit.json').read_text())['Commit']))

    def test_clt_build_from_exported_kit_and_independent_app(self):
        lib = ROOT / 'shell/mac/build/gen/ps1gpu.metallib'
        subprocess.run(['make', '-C', ROOT / 'shell/mac', 'build/gen/ps1gpu.metallib'], check=True, capture_output=True)
        build_kit.export(self.kit, lib)
        image = product_exe(); create_iso(self.source, image)
        profile = self.tmp / 'profile.toml'; profile.write_text('name = "Kit Synthetic Triangle"\n[disc]\nboot_sha256 = "' + hashlib.sha256(image).hexdigest() + '"\n[validation]\nframes = 900\n')
        clt = Path('/Library/Developer/CommandLineTools'); python = clt / 'usr/bin/python3'
        env = dict(os.environ, PATH='/usr/bin:/bin:/usr/sbin:/sbin', PYTHONDONTWRITEBYTECODE='1', PYTHONNOUSERSITE='1')
        if python.is_file(): env['DEVELOPER_DIR'] = str(clt)
        else: python = Path(subprocess.check_output(['xcrun', '--find', 'python3'], text=True).strip())
        before = build_kit.manifest(self.kit)
        result = subprocess.run([*self.command(str(python)), '--profile', profile], cwd=self.tmp, env=env, capture_output=True, text=True, timeout=300)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-6000:]); self.assertIn('the machines agree', result.stdout)
        self.assertNotIn('xcrun -sdk macosx metal', result.stdout); self.assertIn('cp "../../gfx/metal/ps1gpu.metallib"', result.stdout)
        self.assertEqual(list(self.cache.iterdir()), []); self.assertEqual(build_kit.manifest(self.kit), before)
        info = plistlib.loads((self.out / 'Contents/Info.plist').read_bytes())
        self.assertEqual((info['CFBundleShortVersionString'], info['CFBundleVersion'], info['MuyiunativeCommit']), version.metadata(self.kit))
        self.assertEqual((self.out / 'Contents/Resources/ps1gpu.metallib').read_bytes(), lib.read_bytes(), 'player apps reuse the exact launcher shader library')
        self.source.rename(self.tmp / 'original moved away.bin')
        played = subprocess.run([self.out / 'Contents/MacOS/Kit Synthetic Triangle', '--headless', '--frames', '900', '--lockstep'], cwd=self.tmp, env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(played.returncode, 0, played.stderr); self.assertIn('frame hash a2fa0b6d358e04a3', played.stdout); self.assertIn('the machines agree', played.stdout)
        subprocess.run(['codesign', '--verify', '--strict', self.out], check=True, capture_output=True)
