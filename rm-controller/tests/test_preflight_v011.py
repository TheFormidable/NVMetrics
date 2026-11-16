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
 def test_too_hot_rejected(self):
  self.assertEqual(Physical.preflight_rejection({**GOOD,'gpu_c':56},AUTO),'preflight_gpu_temperature_not_below_55C')
if __name__=='__main__':unittest.main()
