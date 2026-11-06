"""Mac product inputs (plan.md, phases 5–6): a portable copy of shared and separate raw tracks keeps its indexes and bytes, including stored pregaps and colliding source
names. Build plans validate profiles and expand their routes before compilation. Inputs are synthetic PS-X EXEs and ISO9660 volumes; no game data or GPU is needed.
The packaged program is assembled here, without an SDK or third-party executable; Metal preferences and offline shaders do not make headless defaults require a GPU."""
import hashlib, plistlib, re, shutil, struct, subprocess, sys, tempfile, unittest
from contextlib import chdir
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import disc, mac_build
from asm import program
from test_disc import raw
from test_exe import make


def both(n, size): return n.to_bytes(size, 'little') + n.to_bytes(size, 'big')


def iso_record(name, sector, size, folder=False):
    name = name.encode('ascii'); n = 33 + len(name) + (len(name) % 2 == 0)
    return bytes([n, 0]) + both(sector, 4) + both(size, 4) + bytes([126, 10, 31, 0, 0, 0, 0, 2 if folder else 0, 0, 0]) + both(1, 2) + bytes([len(name)]) + name + bytes(n - 33 - len(name))


def create_iso(path, exe, serial='SLUS_000.01'):
    """Write a raw Mode 2 disc booting exe, of any length, and return its Path."""
    path = Path(path); cnf = f'BOOT = cdrom:\\{serial};1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'.encode('ascii')
    n = 22 + (len(exe) + 2047) // 2048; sectors = [b''] * max(n, 24)
    sectors[4] = bytes(0x20) + b'Sony Computer Entertainment Amer  ica '
    root = iso_record('\x00', 20, 2048, True)
    pvd = bytearray(2048); pvd[:7] = b'\x01CD001\x01'; pvd[8:40] = b'PLAYSTATION'.ljust(32, b' '); pvd[40:72] = b'MUYIUNATIVE TEST'.ljust(32, b' ')
    pvd[80:88] = both(len(sectors), 4); pvd[120:124] = both(1, 2); pvd[124:128] = both(1, 2); pvd[128:132] = both(2048, 2)
    pvd[132:140] = both(10, 4); struct.pack_into('<I', pvd, 140, 18); struct.pack_into('>I', pvd, 148, 19); pvd[156:156 + len(root)] = root
    sectors[16] = bytes(pvd); sectors[17] = b'\xffCD001\x01'; sectors[18] = b'\x01\x00' + struct.pack('<IH', 20, 1) + b'\x00\x00'
    sectors[19] = b'\x01\x00' + struct.pack('>IH', 20, 1) + b'\x00\x00'
    sectors[20] = root + iso_record('\x01', 20, 2048, True) + iso_record('SYSTEM.CNF;1', 21, len(cnf)) + iso_record(serial + ';1', 22, len(exe))
    sectors[21] = cnf
    for i in range((len(exe) + 2047) // 2048): sectors[22 + i] = exe[i * 2048:(i + 1) * 2048]
    path.write_bytes(b''.join(raw(s) for s in sectors)); return path


def product_exe(color=0x2000c0ff):
    # psx-spx, GPU Display Control Commands (GP1), Rendering Attributes (E1h-E6h), Fill Rectangle and Monochrome Polygon: 320x240, a blue background and a gold triangle.
    # Direct MMIO keeps this app fixture entirely our own code; ps1-tests executables contain SDK libraries and may never be packaged in an app (CONTRIBUTING).
    lines = ['mtc0 zero, $12', 'lui t0, 0x1f80', 'ori t0, t0, 0x1810']
    gp1 = [0, 0x08000001, 0x05000000, 0x06c60260, 0x07040010, 0x03000000]
    gp0 = [0xe1000400, 0xe3000000, 0xe403bd3f, 0xe5000000, 0xe6000000, 0x02281810, 0, 0x00f00140, color, 0x0032003c, 0x003c0104, 0x00c800a0]
    for offset, words in ((4, gp1), (0, gp0)):
        for word in words: lines += [f'lui t1, {word >> 16}', f'ori t1, t1, {word & 0xffff}', f'sw t1, {offset}(t0)']
    code, _ = program(0x80010000, '\n'.join(lines + ['wait:', 'beq zero, zero, wait', 'nop']))
    # The first fixture's 236-byte text made OpenBIOS execute unloaded RAM. psx-spx, CDROM File Formats, FILENAME.EXE: text size must be N*800h bytes.
    return make(code.ljust(2048, b'\x00'))


class MacBuild(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.source = self.tmp / 'source'; self.source.mkdir()
        self.exe = make(); self.data = create_iso(self.source / 'data.bin', self.exe)

    def tearDown(self): shutil.rmtree(self.tmp)

    def profile(self, text=''):
        p = self.tmp / 'game.toml'
        p.write_text('name = "Synthetic Game"\nserial = "SLUS-00001"\n[disc]\nboot_sha256 = "' + hashlib.sha256(self.exe).hexdigest() + '"\n' + text)
        return p

    def test_iso_builder_keeps_an_arbitrary_executable(self):
        payload = make(bytes(range(256)) * 70)
        image = create_iso(self.tmp / 'large.bin', payload)
        name, actual = disc.boot(disc.tracks(image)[0]); self.assertEqual((name, actual), ('SLUS_000.01', payload))
        self.assertEqual(disc.licence(disc.tracks(image)[0]), 'SCEA')

    def test_single_bin_is_portable(self):
        before = disc.identify(self.data); bundled = mac_build.bundle_disc(self.data, self.tmp / 'disc')
        self.assertEqual(bundled.name, 'disc.cue'); shutil.rmtree(self.source)
        self.assertEqual(disc.identify(bundled), before)
        self.assertNotIn(str(self.source), bundled.read_text())
        self.assertTrue(all(not t.file.is_symlink() and t.file.parent == bundled.parent for t in disc.tracks(bundled)))

    def test_separate_files_with_the_same_basename(self):
        a = self.source / 'one'; b = self.source / 'two'; a.mkdir(); b.mkdir()
        self.data.rename(a / 'track.bin'); (b / 'track.bin').write_bytes(bytes(i & 255 for i in range(160 * disc.SECTOR)))
        cue = self.source / 'game.cue'; cue.write_text('FILE "one/track.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n'
                                                    'FILE "two/track.bin" BINARY\n TRACK 02 AUDIO\n INDEX 00 00:00:00\n INDEX 01 00:02:00\n')
        expected = disc.identify(cue); bundled = mac_build.bundle_disc(cue, self.tmp / 'disc'); shutil.rmtree(self.source)
        ts = disc.tracks(bundled); self.assertNotEqual(ts[0].file, ts[1].file); self.assertEqual((ts[1].first, ts[1].start), (0, 150))
        self.assertEqual(disc.identify(bundled), expected)

    def test_shared_file_and_stored_pregap(self):
        data = self.data.read_bytes(); count = len(data) // disc.SECTOR
        self.data.write_bytes(data + bytes(160 * disc.SECTOR))
        stamp = lambda f: f'{f // 4500:02}:{f // 75 % 60:02}:{f % 75:02}'
        cue = self.source / 'game.cue'; cue.write_text(f'FILE "data.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n'
                                                    f' TRACK 02 AUDIO\n INDEX 00 {stamp(count)}\n INDEX 01 {stamp(count + 150)}\n')
        expected = disc.identify(cue); bundled = mac_build.bundle_disc(cue, self.tmp / 'disc'); shutil.rmtree(self.source)
        ts = disc.tracks(bundled); self.assertEqual(ts[0].file, ts[1].file); self.assertEqual((ts[1].first, ts[1].start), (count, count + 150))
        self.assertEqual(disc.identify(bundled), expected); self.assertEqual(len(list(bundled.parent.glob('*.bin'))), 1)

    def test_first_track_stored_leadin_keeps_its_origin(self):
        self.data.write_bytes(raw(b'') * 2 + self.data.read_bytes())
        cue = self.source / 'game.cue'; cue.write_text('FILE "data.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 00 00:00:00\n INDEX 01 00:00:02\n')
        expected = disc.identify(cue); bundled = mac_build.bundle_disc(cue, self.tmp / 'disc'); shutil.rmtree(self.source)
        self.assertEqual((disc.tracks(bundled)[0].first, disc.tracks(bundled)[0].start), (0, 2)); self.assertEqual(disc.identify(bundled), expected)

    def test_raw_mode1_uses_its_header(self):
        data = self.data.read_bytes()
        self.data.write_bytes(b''.join(raw(data[i + 24:i + 24 + 2048], mode=1) for i in range(0, len(data), disc.SECTOR)))
        original = self.data.read_bytes(); bundled = mac_build.bundle_disc(self.data, self.tmp / 'disc'); shutil.rmtree(self.source)
        ts = disc.tracks(bundled); self.assertEqual(ts[0].mode, 'MODE1/2352'); self.assertEqual(ts[0].file.read_bytes(), original)
        self.assertEqual(disc.boot(ts[0])[1], self.exe)

    def test_existing_destination_is_preserved(self):
        out = self.tmp / 'disc'; out.mkdir(); (out / 'keep').write_text('untouched')
        with self.assertRaises((OSError, SystemExit)): mac_build.bundle_disc(self.data, out)
        self.assertEqual((out / 'keep').read_text(), 'untouched'); self.assertEqual(list(out.iterdir()), [out / 'keep'])

    def test_build_refuses_an_unrelated_app_before_compilation(self):
        out = self.tmp / 'Other.app'; out.mkdir(); (out / 'keep').write_text('untouched')
        result = subprocess.run([sys.executable, ROOT / 'scripts/muyiunative', 'build', self.data, '--output', out], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0); self.assertIn('not this game app', result.stderr)
        self.assertNotIn('building the capture host', result.stdout); self.assertEqual((out / 'keep').read_text(), 'untouched')

    def test_build_refuses_scaled_software_before_compilation(self):
        out = self.tmp / 'Scaled.app'
        result = subprocess.run([sys.executable, ROOT / 'scripts/muyiunative', 'build', self.data, '--scale', '2', '--output', out], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0); self.assertIn('--scale 2 or 4 needs --renderer metal', result.stderr)
        self.assertNotIn('building the capture host', result.stdout); self.assertFalse(out.exists())

    def test_set_rejects_region_mismatch_and_inputs_inside_output(self):
        second = create_iso(self.source / 'second.bin', self.exe)
        data = second.read_bytes().replace(b'Amer  ica ', b'Euro pe   '); second.write_bytes(data)
        command = [sys.executable, ROOT / 'scripts/muyiunative', 'build', self.data, '--disc', second]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0); self.assertIn('same licence region', result.stderr)
        out = self.tmp / 'Same Game.app'; (out / 'Contents/Resources').mkdir(parents=True)
        identity = mac_build.build_plan(self.data)['save_id']
        (out / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': f'org.muyiunative.game.{identity.lower()}'}))
        inside = create_iso(out / 'Contents/Resources/second.bin', self.exe)
        result = subprocess.run([*command[:-1], inside, '--output', out], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0); self.assertIn('contains a source disc', result.stderr); self.assertTrue(inside.is_file())

    def test_unknown_game_plan_and_stable_save_identity(self):
        plan = mac_build.build_plan(self.data)
        self.assertEqual((plan['serial'], plan['boot_sha256'], plan['frames'], plan['inputs']), ('SLUS-00001', hashlib.sha256(self.exe).hexdigest(), 2400, []))
        self.assertIsNone(plan['profile']); self.assertTrue(plan['title']); self.assertIn(str(plan['boot_sha256']), str(plan['save_id']))
        other = self.tmp / 'renamed.bin'; shutil.copyfile(self.data, other)
        self.assertEqual(mac_build.build_plan(other)['save_id'], plan['save_id'])
        homebrew = create_iso(self.tmp / 'homebrew.bin', self.exe, serial='PSX.EXE')
        self.assertIsNone(mac_build.build_plan(homebrew)['serial']); self.assertEqual(mac_build.build_plan(homebrew)['boot_sha256'], plan['boot_sha256'])

    def test_profile_routes_and_overrides(self):
        profile = self.profile('[validation]\nframes = 12\ninputs = ["2:8", "4:0"]\n[first_level]\nframes = 30\ninputs = []\n'
                               'pulses = [{ start = 1, stop = 5, period = 2, on = 1, buttons = 8 }]\n')
        plan = mac_build.build_plan(self.data, profile)
        self.assertEqual((plan['title'], plan['route'], plan['frames'], plan['inputs']), ('Synthetic Game', 'first_level', 30, ['1:8', '2:0', '3:8', '4:0']))
        plan = mac_build.build_plan(self.data, profile, route='validation')
        self.assertEqual((plan['frames'], plan['inputs']), (12, ['2:8', '4:0']))
        plan = mac_build.build_plan(self.data, profile, route='validation', frames=6, inputs=['1:40', '3:0'])
        self.assertEqual((plan['frames'], plan['inputs']), (6, ['1:40', '3:0']))
        with self.assertRaises(SystemExit): mac_build.build_plan(self.data, profile, route='absent', frames=5)

    def test_wrong_executable_is_refused_before_build(self):
        profile = self.profile('[validation]\nframes = 12\n')
        profile.write_text(profile.read_text().replace(hashlib.sha256(self.exe).hexdigest(), '0' * 64))
        with self.assertRaises(SystemExit): mac_build.build_plan(self.data, profile)

    def test_relative_profile_survives_compiler_working_directory(self):
        profile = self.profile('[validation]\nframes = 12\n')
        with chdir(self.tmp): plan = mac_build.build_plan(self.data, Path('game.toml'))
        self.assertEqual(plan['profile'], profile.resolve())

    def test_malformed_profile_fields_are_refused(self):
        profile = self.profile('[validation]\nframes = 12\n'); text = profile.read_text()
        for malformed in (text.replace('name = "Synthetic Game"', 'name = 1'), text.replace('frames = 12', 'frames = true'),
                          'validation = "not a table"\n' + text.split('[validation]')[0]):
            with self.subTest(profile=malformed):
                profile.write_text(malformed)
                with self.assertRaises(SystemExit): mac_build.build_plan(self.data, profile)


@unittest.skipUnless(sys.platform == 'darwin' and shutil.which('codesign'), 'needs macOS and its codesign')
class MacProduct(unittest.TestCase):
    def test_four_disc_app_is_native_and_independent_of_all_originals(self):
        with tempfile.TemporaryDirectory(prefix='muyiunative-disc-set-test-') as directory:
            root = Path(directory); source = root / 'originals'; source.mkdir(); cues, hashes = [], []
            for i, color in enumerate((0x2000c0ff, 0x20ff0030, 0x2040ff20, 0x20c080ff), 1):
                folder = source / f'disc {i}'; folder.mkdir(); image = product_exe(color)
                create_iso(folder / 'track.bin', image, serial=f'SLUS_000.0{i}'); hashes.append(hashlib.sha256(image).hexdigest())
                cue = folder / 'disc.cue'; cue.write_text('FILE "track.bin" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n'); cues.append(cue)
            playlist = source / 'set.m3u'; playlist.write_text('#EXTM3U\n' + '\n'.join(str(p.relative_to(source)) for p in cues))
            app = root / 'Four Discs.app'
            def run(command):
                r = subprocess.run(list(map(str, command)), capture_output=True, text=True, timeout=300)
                self.assertEqual(r.returncode, 0, (r.stdout + r.stderr)[-6000:]); return r.stdout
            built = run([sys.executable, ROOT / 'scripts/muyiunative', 'build', playlist, '--frames', 900, '--output', app])
            self.assertEqual(built.count('the machines agree'), 4)
            metadata = plistlib.loads((app / 'Contents/Resources/game.plist').read_bytes())
            self.assertEqual(metadata['Version'], 2); self.assertEqual([d['BootSHA256'] for d in metadata['Discs']], hashes)
            self.assertEqual(metadata['SaveID'], mac_build.build_plan(cues[0])['save_id'])
            info = plistlib.loads((app / 'Contents/Info.plist').read_bytes()); executable = app / 'Contents/MacOS' / info['CFBundleExecutable']
            moved = root / 'Moved Four Discs.app'; app.rename(moved); executable = moved / executable.relative_to(app); shutil.rmtree(source)
            frames = []
            for number in range(1, 5):
                played = run([executable, '--headless', '--frames', 900, '--disc-number', number, '--lockstep'])
                self.assertIn('the machines agree', played)
                counts = re.search(r'host: (\d+) instructions native, (\d+) interpreted', played); self.assertIsNotNone(counts); assert counts is not None
                self.assertGreater(int(counts[1]), 0); self.assertEqual(int(counts[2]), 0, 'every disc route must run natively in the same executable')
                frames.append(mac_build.frame_hash(played))
            self.assertEqual(len(set(frames)), 4, 'the four different native programs draw four different results')
            bad = subprocess.run([executable, '--headless', '--disc-number', '5'], capture_output=True, text=True)
            self.assertEqual(bad.returncode, 2)
            run(['codesign', '--verify', '--strict', moved])

    def test_native_app_is_portable_and_cards_survive_restart(self):
        with tempfile.TemporaryDirectory(prefix='muyiunative-product-test-') as directory:
            tmp = Path(directory); executable = product_exe()
            source = create_iso(tmp / 'source disc.bin', executable); profile = tmp / 'game.toml'; digest = hashlib.sha256(executable).hexdigest()
            profile.write_text(f'name = "Synthetic Triangle"\n[disc]\nboot_sha256 = "{digest}"\n[validation]\nframes = 900\ninputs = []\n')
            app = tmp / 'Native Triangle.app'

            def run(command, timeout=300):
                result = subprocess.run([str(s) for s in command], cwd=tmp, capture_output=True, text=True, timeout=timeout)
                self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-5000:]); return result

            run([sys.executable, ROOT / 'scripts/muyiunative', 'build', source.name, '--platform', 'mac-app', '--profile', profile.name, '--output', app,
                 '--renderer', 'metal', '--scale', 2])
            run(['codesign', '--verify', '--strict', app])
            signature = run(['codesign', '--display', '--verbose=2', app]).stderr
            self.assertRegex(signature, r'flags=0x[0-9a-f]+\([^)]*runtime')
            manifest = plistlib.loads((app / 'Contents/Resources/game.plist').read_bytes()); info = plistlib.loads((app / 'Contents/Info.plist').read_bytes())
            self.assertEqual((manifest['Version'], manifest['Title'], manifest['Disc'], manifest['BootSHA256']), (1, 'Synthetic Triangle', 'disc/disc.cue', digest))
            self.assertEqual((manifest['Renderer'], manifest['RenderScale']), ('metal', 2))
            shaders = app / 'Contents/Resources/ps1gpu.metallib'; self.assertTrue(shaders.is_file()); self.assertGreater(shaders.stat().st_size, 0)
            self.assertIn(digest, manifest['SaveID']); exe = app / 'Contents/MacOS' / info['CFBundleExecutable']
            source.rename(tmp / 'original moved away.bin')
            default_cards = Path.home() / 'Library/Application Support/muyiunative/Saves' / manifest['SaveID']; existed = default_cards.exists()
            playback = run([exe, '--headless', '--frames', 900, '--lockstep'])
            self.assertIn('frame hash a2fa0b6d358e04a3', playback.stdout); self.assertIn('the machines agree', playback.stdout)
            self.assertIn('900 frames, 320x240,', playback.stdout); self.assertNotIn('Metal renderer', playback.stdout)
            self.assertNotRegex(playback.stdout + playback.stderr, r'(?m)^metal:')
            software = run([exe, '--headless', '--frames', 1, '--renderer', 'software'])
            self.assertIn('host: 1 frames,', software.stdout); self.assertNotIn('Metal renderer', software.stdout)
            self.assertNotRegex(software.stdout + software.stderr, r'(?m)^metal:')
            counts = re.search(r'host: (\d+) instructions native, (\d+) interpreted', playback.stdout); self.assertIsNotNone(counts)
            assert counts is not None
            native, interpreted = map(int, counts.groups()); self.assertGreater(native / (native + interpreted), .99)
            self.assertEqual(default_cards.exists(), existed, 'headless playback must not create Application Support cards')
            self.assertFalse(list(tmp.rglob('*.mcr')))
            saves = tmp / 'temporary saves'; run([exe, '--headless', '--frames', 1, '--save-dir', saves])
            cards = [saves / f'card{i}.mcr' for i in (1, 2)]; self.assertEqual([c.stat().st_size for c in cards], [128 * 1024] * 2)
            data = bytearray(cards[0].read_bytes()); data[-128:] = bytes(range(128)); cards[0].write_bytes(data)
            before = [c.read_bytes() for c in cards]; run([exe, '--headless', '--frames', 1, '--save-dir', saves])
            self.assertEqual([c.read_bytes() for c in cards], before)


if __name__ == '__main__': unittest.main()
