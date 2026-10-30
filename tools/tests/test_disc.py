"""tools/disc.py and scripts/disc_info.py read a disc built here: a CUE sheet with a Mode 2 data track holding the licence string, an ISO9660 volume with
SYSTEM.CNF and the executable it boots in a subdirectory, and an audio track with a pregap in a file of its own; then the same tracks in one file, and the data
track as a raw BIN alone. Checked: the serial, the licence's region, the executable's and the tracks' SHA-256, the TOML the script prints (without the disc's
path), and the sheets and files that are refused. No game files."""
import hashlib, shutil, struct, subprocess, sys, tempfile, tomllib, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import disc

SCRIPT = ROOT / 'scripts' / 'disc_info.py'
EXE = bytes(range(256)) * 11 + b'end'   # 2819 bytes: two sectors
CNF = b'BOOT = cdrom:\\GAME\\SLUS_000.01;1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n'


def raw(data, mode=2):   # a raw sector: sync, header, the Mode 2 subheader, 2048 bytes of user data, and the error codes (left zero)
    return b'\x00' + b'\xff' * 10 + b'\x00' + bytes([0, 2, 0, mode]) + (b'\x00' * 8 if mode == 2 else b'') + data.ljust(2048, b'\x00') + b'\x00' * (280 if mode == 2 else 288)


def record(name, lba, size, folder=False):   # an ISO9660 directory record
    n = name.encode(); r = struct.pack('<BBIIII7sBBBHHB', 0, 0, lba, 0, size, 0, b'', 2 if folder else 0, 0, 0, 1, 0, len(n)) + n
    r += b'\x00' * (len(r) & 1)
    return bytes([len(r)]) + r[1:]


def volume(licensed=True, boot=True):   # 24 sectors of a data track: the licence in sector 4, the volume descriptor in 16, directories in 18 and 19, files from 20
    s = [b''] * 24
    if licensed: s[4] = b'\x00' * 0x20 + b'Sony Computer Entertainment Amer  ica '
    s[16] = b'\x01CD001\x01' + b'\x00' * 149 + record('\x00', 18, 2048, True)
    s[17] = b'\xffCD001\x01'
    s[18] = record('\x00', 18, 2048, True) + record('\x01', 18, 2048, True) + record('GAME', 19, 2048, True) + (record('SYSTEM.CNF;1', 20, len(CNF)) if boot else b'')
    s[19] = record('\x00', 19, 2048, True) + record('\x01', 18, 2048, True) + record('SLUS_000.01;1', 21, len(EXE))
    s[20] = CNF; s[21] = EXE[:2048]; s[22] = EXE[2048:]
    return b''.join(raw(d) for d in s)


class Disc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp()); t = cls.tmp
        cls.data = volume(); cls.audio = bytes(i * 7 & 255 for i in range(160 * disc.SECTOR))   # a 150-sector pregap, then 10 sectors
        (t / 'one.bin').write_bytes(cls.data); (t / 'two.bin').write_bytes(cls.audio); (t / 'both.bin').write_bytes(cls.data + cls.audio)
        (t / 'disc.cue').write_text('FILE "one.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                                    'FILE "two.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n')
        (t / 'both.cue').write_text('REM one file\nFILE "both.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
                                    '  TRACK 02 AUDIO\n    INDEX 00 00:00:24\n    INDEX 01 00:02:24\n')

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp)

    def sha(self, b): return hashlib.sha256(b).hexdigest()

    def test_identify(self):
        d = disc.identify(self.tmp / 'disc.cue')
        self.assertEqual((d['serial'], d['licence'], d['boot'], d['boot_sha256']), ('SLUS-00001', 'SCEA', 'GAME\\SLUS_000.01', self.sha(EXE)))
        self.assertEqual(d['tracks'], [{'number': 1, 'mode': 'MODE2/2352', 'sectors': 24, 'sha256': self.sha(self.data)},
                                       {'number': 2, 'mode': 'AUDIO', 'sectors': 160, 'sha256': self.sha(self.audio)}])
        self.assertEqual(disc.identify(self.tmp / 'both.cue')['tracks'], d['tracks'], 'one file: each track from its first index to the next')
        self.assertEqual(disc.identify(self.tmp / 'one.bin')['tracks'], d['tracks'][:1], 'a raw BIN: one data track')

    def test_without_licence_or_cnf(self):
        (self.tmp / 'bare.bin').write_bytes(volume(licensed=False, boot=False))
        with self.assertRaisesRegex(SystemExit, 'PSX.EXE is not on the disc'): disc.identify(self.tmp / 'bare.bin')
        self.assertIsNone(disc.licence(disc.tracks(self.tmp / 'bare.bin')[0]))

    def test_refused(self):
        for name, text, why in [('m.cue', 'FILE "one.bin" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n', 'TRACK of MODE1/2352'),
                                ('w.cue', 'FILE "one.bin" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n', 'only BINARY'),
                                ('p.cue', 'FILE "one.bin" BINARY\n  TRACK 01 MODE2/2352\n    PREGAP 00:02:00\n    INDEX 01 00:00:00\n', 'PREGAP is not read'),
                                ('i.cue', 'FILE "one.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 00 00:00:00\n', 'no INDEX 01'),
                                ('a.cue', 'FILE "two.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n', 'first track is audio'),
                                ('x.cue', 'FILE "one.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:xx:00\n', 'not read')]:
            (self.tmp / name).write_text(text)
            with self.subTest(name), self.assertRaisesRegex(SystemExit, why): disc.tracks(self.tmp / name)
        (self.tmp / 'odd.bin').write_bytes(b'\x00' * 1000)
        with self.assertRaisesRegex(SystemExit, 'not whole 2352-byte sectors'): disc.tracks(self.tmp / 'odd.bin')

    def test_script(self):
        r = subprocess.run([sys.executable, SCRIPT, self.tmp / 'disc.cue'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        t = tomllib.loads(r.stdout)
        self.assertEqual((t['serial'], t['disc']['licence'], t['disc']['boot'], t['disc']['boot_sha256']), ('SLUS-00001', 'SCEA', 'GAME\\SLUS_000.01', self.sha(EXE)))
        self.assertEqual([x['sha256'] for x in t['disc']['tracks']], [self.sha(self.data), self.sha(self.audio)])
        self.assertNotIn(str(self.tmp), r.stdout + r.stderr, "the disc's path is not printed")
        r = subprocess.run([sys.executable, SCRIPT, self.tmp / 'none.cue'], capture_output=True, text=True)
        self.assertTrue(r.returncode and 'no such file' in r.stderr)


if __name__ == '__main__': unittest.main()
