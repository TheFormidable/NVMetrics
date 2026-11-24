import unittest
from benchmark import frame_report


class GamingTests(unittest.TestCase):
    def test_uniform_frames(self):
        r = frame_report('frametime_ms\n' + '10\n'*100)
        self.assertEqual(r['fps'], 100)
        self.assertEqual(r['one_percent_low'], 100)
        self.assertEqual(r['p99_ms'], 10)

    def test_stutter_and_duration_weighted_fps(self):
        r = frame_report('frametime_ms\n' + '10\n'*99 + '100\n')
        self.assertAlmostEqual(r['fps'], 100 / 1.09)
        self.assertEqual(r['one_percent_low'], 10)
        self.assertEqual(r['p99_ms'], 10)
        self.assertEqual(r['frames'], 100)

    def test_reject_no_real_frames_or_invalid_numbers(self):
        for s in ('fps\n60\n', 'frametime_ms\n', 'frametime_ms\n0\n',
                  'frametime_ms\nnan\n', 'frametime_ms\ninf\n',
                  'frametime_ms\n-1\n', 'frametime_ms\n10,20\n'):
            with self.assertRaises(ValueError):
                frame_report(s)
