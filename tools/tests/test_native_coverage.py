"""Exact native coverage gates reject BIOS-only runs, missing entries, rounded percentages and inconsistent fallback evidence. All reports are synthetic."""
import copy, json, sys, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import native_coverage


def report(native=100, interpreted=0) -> dict:
    regions = {name: dict(native=0, interpreted=0) for name in ('program_ram', 'kernel_ram', 'bios_rom', 'other')}
    regions['program_ram'] = dict(native=native, interpreted=interpreted); regions['bios_rom']['native'] = 10**12
    fallbacks = [dict(pc='80010000', region='program_ram', reason='no_entry', hits=interpreted)] if interpreted else []
    return dict(version=1, entry='80010000', started=True, regions=regions, fallbacks=fallbacks)


class NativeCoverage(unittest.TestCase):
    def read(self, data):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'coverage.json'; path.write_text(json.dumps(data)); return native_coverage.read(path)

    def test_exact_program_coverage_excludes_bios(self):
        self.assertEqual(native_coverage.enforce(self.read(report()))['percent'], 100)
        with self.assertRaisesRegex(ValueError, 'program RAM'): native_coverage.enforce(self.read(report(1, 99)))
        with self.assertRaisesRegex(ValueError, 'interpreted'): native_coverage.enforce(self.read(report(10**12, 1)))
        self.assertGreater(native_coverage.enforce(self.read(report(10000, 1)), 99.99)['percent'], 99.99)

    def test_missing_entry_and_vacuous_routes_fail(self):
        absent = report(); absent['started'] = False
        with self.assertRaisesRegex(ValueError, 'never reached'): native_coverage.enforce(self.read(absent))
        with self.assertRaisesRegex(ValueError, 'no program'): native_coverage.enforce(self.read(report(0)))

    def test_invalid_limits(self):
        for value in (-1, 101, True, float('inf'), float('nan'), '100', None):
            with self.subTest(value=value), self.assertRaises(ValueError): native_coverage.minimum(value)

    def test_corrupt_or_inconsistent_evidence_fails(self):
        original = report(3, 1)
        cases = []
        for key, value in (('version', 2), ('entry', 'bad'), ('started', 1), ('regions', {}), ('fallbacks', [])):
            data = copy.deepcopy(original); data[key] = value; cases.append(data)
        for key, value in (('native', True), ('interpreted', -1)):
            data = copy.deepcopy(original); data['regions']['program_ram'][key] = value; cases.append(data)
        for key, value in (('reason', 'unknown'), ('pc', 'instruction bytes'), ('hits', 0), ('region', 'unknown')):
            data = copy.deepcopy(original); data['fallbacks'][0][key] = value; cases.append(data)
        for data in cases:
            with self.subTest(data=data), self.assertRaises(ValueError): self.read(data)


if __name__ == '__main__': unittest.main()
