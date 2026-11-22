import sys,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'service'))
from physical import Physical
AUTO={'mode':'auto'}
GOOD={'gpu_c':44.0,'mem_c':62.0,'power_w':11.0,'util_gpu_pct':0,'fresh':True}
class PreflightReasons(unittest.TestCase):
 def test_good(self):
  self.assertTrue(Physical.idle_ok(GOOD,AUTO))
 def test_power_and_util_rejected(self):
  for metric,bad,want in [('power_w',56.0,'preflight_power_above_35W'),
       ('util_gpu_pct',98,'preflight_utilization_above_20pct_or_unavailable')]:
   s={**GOOD,metric:bad}
   self.assertFalse(Physical.idle_ok(s,AUTO))
   self.assertEqual(Physical.preflight_rejection(s,AUTO),want)
 def test_unknown_util_rejected_before_manual(self):
  s={**GOOD,'util_gpu_pct':None}
  self.assertFalse(Physical.idle_ok(s,AUTO))
  self.assertEqual(Physical.preflight_rejection(s,AUTO),'preflight_utilization_above_20pct_or_unavailable')
 def test_no_auto_rejected(self):
  self.assertEqual(Physical.preflight_rejection(GOOD,{'mode':'manual'}),'preflight_requires_AUTO')
 def test_stale_rejected(self):
  self.assertEqual(Physical.preflight_rejection({**GOOD,'fresh':False},AUTO),'preflight_telemetry_not_fresh')
 def test_thermal_entry_at_49_to_59_degrees_is_allowed_when_other_sensors_safe(self):
  # 49 C is NOT a fixed cut-off. A fresh thermal-profile sample has
  # a strict 60 C entry limit; the other independent limits still apply.
  for temp in (49.0,50.0,54.0,55.0,58.0,59.9):
   with self.subTest(gpu=temp):
    sample={**GOOD,'gpu_c':temp,'mem_c':70.0,'power_w':42.0,'util_gpu_pct':3}
    self.assertTrue(Physical.idle_ok(sample,AUTO,thermal=True,continuous=True))
  too_hot={**GOOD,'gpu_c':60.0}
  self.assertFalse(Physical.idle_ok(too_hot,AUTO,thermal=True,continuous=True))
  self.assertEqual(Physical.preflight_rejection(too_hot,AUTO,thermal=True,continuous=True),
                   'preflight_gpu_temperature_not_below_60C')
 def test_49_degrees_with_stale_telemetry_is_still_refused(self):
  sample={**GOOD,'gpu_c':49.,'fresh':False}
  self.assertFalse(Physical.idle_ok(sample,AUTO,thermal=True,continuous=True))
  self.assertEqual(Physical.preflight_rejection(sample,AUTO,thermal=True,continuous=True),
                   'preflight_telemetry_not_fresh')
 def test_too_hot_rejected(self):
  self.assertEqual(Physical.preflight_rejection({**GOOD,'gpu_c':56},AUTO),'preflight_gpu_temperature_not_below_55C')
if __name__=='__main__':unittest.main()
