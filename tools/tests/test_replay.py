"""Input replays combine held base buttons with independent repeated presses, including overlapping masks and route truncation; t_replay checks the host parser (plan.md, phase 4). No game data."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import replay


class Replay(unittest.TestCase):
    def test_overlapping_presses(self):
        pulses = [{'start': 10, 'stop': 30, 'period': 10, 'on': 5, 'buttons': 0x8000},
                  {'start': 12, 'stop': 20, 'period': 10, 'on': 6, 'buttons': 0xc000}]
        self.assertEqual(replay.events(['0:20', '16:0'], pulses), ['0:20', '10:8020', '12:c020', '16:c000', '18:0', '20:8000', '25:0'])
        self.assertEqual(replay.events(['0:20', '16:0'], pulses, 13), ['0:20', '10:8020', '12:c020', '13:20'])
        self.assertEqual(replay.events(['5:8', '5:0', '9:4000', '15:0'], frames=10), ['9:4000'])

    def test_refused(self):
        for inputs in (['1:8', '0:0'], ['-1:8'], ['0:10000'], ['0:'], ['0:8:0']):
            with self.subTest(inputs=inputs), self.assertRaises(ValueError): replay.events(inputs)
        for patch in ({'period': 0}, {'on': 0}, {'on': 12}, {'stop': 5}, {'buttons': 0}, {'buttons': '8000'}):
            p: dict[str, object] = {'start': 5, 'stop': 100, 'period': 12, 'on': 6, 'buttons': 0x8000}; p.update(patch)
            with self.subTest(patch=patch), self.assertRaises(ValueError): replay.events([], [p])


if __name__ == '__main__': unittest.main()
