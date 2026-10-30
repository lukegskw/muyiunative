"""scripts/native_game.py on discs built here: it finds a game's profile by the serial in the boot executable's name, refuses a disc whose boot executable is not
the one the profile describes, and a profile without a route. The rest (capture, generation, the host's build, the route in lockstep) takes a host build of a
minute and a disc that boots, so it is checked by hand: docs/development.md, phase 3. No game files."""
import shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from test_disc import volume

SCRIPT = ROOT / 'scripts' / 'native_game.py'


class NativeGame(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); (self.tmp / 'disc.bin').write_bytes(volume())

    def tearDown(self): shutil.rmtree(self.tmp)

    def run_script(self, *args): return subprocess.run([sys.executable, SCRIPT, self.tmp / 'disc.bin', *args], capture_output=True, text=True)

    def test_refused(self):
        r = self.run_script()
        self.assertTrue(r.returncode and 'no profile profiles/SLUS-00001/game.toml' in r.stderr, r.stderr)
        (self.tmp / 'other.toml').write_text('[disc]\nboot_sha256 = "00"\n[validation]\nframes = 10\n')
        r = self.run_script('--profile', self.tmp / 'other.toml')
        self.assertTrue(r.returncode and 'not the one other.toml describes' in r.stderr, r.stderr)
        (self.tmp / 'none.toml').write_text('name = "x"\n')
        r = self.run_script('--profile', self.tmp / 'none.toml')
        self.assertTrue(r.returncode and 'no route' in r.stderr, r.stderr)


if __name__ == '__main__': unittest.main()
