"""Regression of the 2026-11-15 RTX 5050 logs; no real hardware calls."""
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from physical import Physical, PhysicalError

GOOD={'fresh':True,'gpu_c':51.3,'mem_c':74.,'power_w':48.01,'util_gpu_pct':3,'rpm':[1679,1705]}
PRE_PEAK={**GOOD,'gpu_c':51.5,'power_w':51.48,'util_gpu_pct':21,'rpm':[1512,1512]}
POST_PEAK={**GOOD,'power_w':51.56,'util_gpu_pct':18,'rpm':[1733,1739]}

class LowerTransition(unittest.TestCase):
    def setUp(self):
        self.p=Physical(Path('/fake/agent'),True,thermal_profile=True)
        self.p.token='a'*32;self.p.pct=50
        self.p._validate=Mock();self.p.heartbeat=Mock();self.p.watchdog_tick=Mock();self.p._poll_child=Mock()
        self.p._audit=Mock();self.p.stop=Mock()
        self.p.status=Mock(side_effect=lambda:{'active':True,'percent':self.p.pct})
        self.p._send=Mock(side_effect=self.native)
        self.sleep=patch('physical.time.sleep');self.sleep.start();self.addCleanup(self.sleep.stop)
    def native(self,line,expect):
        if line.startswith(('LOWER ','RAISE ')):return {'pct':int(line.split()[1]),'event':'manual'}
    def call(self,pct,values):
        return self.p.lower_percent(501,self.p.token,pct,probe=Mock(side_effect=values),inspect={'mode':'manual'})
    def writes(self):
        return [c.args[0] for c in self.p._send.call_args_list if c.args[0].startswith(('LOWER ','RAISE '))]
    def test_direct_descent_checks_target_once(self):
        self.p.pct=100
        self.assertEqual(self.call(30,[{**GOOD,'rpm':[3700,3700]}]+[GOOD]*3)['percent'],30)
        self.assertEqual(self.writes(),['LOWER 30']);self.p.stop.assert_not_called()
    def test_direct_descent_rejects_invalid_target_or_profile(self):
        self.p.pct=80
        for target in (25,31,80,85,101):
            with self.subTest(target=target), self.assertRaisesRegex(PhysicalError,'lower_invalid_target_or_profile'):
                self.call(target,[])
        self.p.thermal_profile=False
        with self.assertRaisesRegex(PhysicalError,'lower_invalid_target_or_profile'):
            self.call(30,[])
        self.p.load_profile=True
        with self.assertRaisesRegex(PhysicalError,'lower_not_permitted_in_load_profile'):
            self.call(30,[])
        self.assertEqual(self.writes(),[])
    def test_direct_descent_tach_failure_requests_auto(self):
        self.p.pct=100
        with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
            self.call(30,[{**GOOD,'rpm':[3700,3700]},{**GOOD,'rpm':[0,1600]}])
        self.assertEqual(self.writes(),['LOWER 30']);self.p.stop.assert_called_once_with('low_calibration_failed')
    def test_real_preflight_peak_retries_before_writing(self):
        self.p.pct=40
        self.assertEqual(self.call(35,[PRE_PEAK,GOOD,GOOD,GOOD,GOOD])['percent'],35)
        self.assertEqual(self.writes(),['LOWER 35']);self.p.stop.assert_not_called()
    def test_real_postwrite_peak_requires_three_fresh_good_samples(self):
        self.assertEqual(self.call(45,[GOOD,POST_PEAK,GOOD,GOOD,GOOD])['percent'],45)
        self.assertEqual(self.writes(),['LOWER 45']);self.p.stop.assert_not_called()
    def test_sustained_preflight_preserves_40_without_write(self):
        self.p.pct=40
        with self.assertRaisesRegex(PhysicalError,'lower_thermal_margin_insufficient_no_write'):
            self.call(35,[PRE_PEAK]*3)
        self.assertEqual(self.p.pct,40);self.assertEqual(self.writes(),[]);self.p.stop.assert_not_called()
    def test_sustained_postwrite_recovers_50(self):
        with self.assertRaisesRegex(PhysicalError,'lower_recovered_verified_higher'):
            self.call(45,[GOOD]+[POST_PEAK]*5+[GOOD]*3)
        self.assertEqual(self.writes(),['LOWER 45','RAISE 50']);self.assertEqual(self.p.pct,50);self.p.stop.assert_not_called()
    def test_recovery_from_35_raises_to_50(self):
        self.p.pct=40
        with self.assertRaisesRegex(PhysicalError,'lower_recovered_verified_higher'):
            self.call(35,[GOOD]+[POST_PEAK]*5+[GOOD]*3)
        self.assertEqual(self.p.pct,50);self.assertEqual(self.writes(),['LOWER 35','RAISE 50'])
    def test_larger_load_reverts_without_grace(self):
        with self.assertRaisesRegex(PhysicalError,'lower_recovered_verified_higher'):
            self.call(45,[GOOD,{**GOOD,'power_w':90,'util_gpu_pct':70}]+[GOOD]*3)
        self.assertEqual(self.writes(),['LOWER 45','RAISE 50'])
    def test_critical_postwrite_fault_does_not_retry_or_raise(self):
        for fault in ({'rpm':[0,1739]},{'fresh':False},{'gpu_c':58.0},{'mem_c':82.0},{'power_w':146},{'util_gpu_pct':None}):
            with self.subTest(fault=fault):
                self.p.pct=50;self.p._send.reset_mock();self.p.stop.reset_mock()
                with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
                    self.call(45,[GOOD,{**GOOD,**fault}])
                self.assertEqual(self.writes(),['LOWER 45']);self.p.stop.assert_called_once_with('low_calibration_failed')
    def test_failed_recovery_tach_requests_auto(self):
        with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
            self.call(45,[GOOD]+[POST_PEAK]*5+[{**GOOD,'rpm':[0,1600]}])
        self.assertEqual(self.writes(),['LOWER 45','RAISE 50']);self.p.stop.assert_called_once_with('low_calibration_failed')
    def test_recovery_rm_failure_requests_auto(self):
        def native(line,expect):
            if line=='RAISE 50':raise PhysicalError('guard_failure')
            return self.native(line,expect)
        self.p._send.side_effect=native
        with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
            self.call(45,[GOOD]+[POST_PEAK]*5)
        self.p.stop.assert_called();self.assertEqual(self.writes(),['LOWER 45','RAISE 50'])
    def test_idle_profile_has_no_grace(self):
        self.p.thermal_profile=False
        idle={**GOOD,'gpu_c':42.,'power_w':20}
        with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
            self.call(45,[idle,{**idle,'power_w':36}])
        self.assertEqual(self.writes(),['LOWER 45']);self.p.stop.assert_called_once()
    def test_native_no_write_refusal_checks_current_safety(self):
        self.p.pct=40
        self.p._send.side_effect=PhysicalError('lower_preflight_changed_no_write')
        with self.assertRaisesRegex(PhysicalError,'lower_thermal_margin_insufficient_no_write'):
            self.call(35,[GOOD,PRE_PEAK])
        self.assertEqual(self.p.pct,40);self.p.stop.assert_not_called()

    def test_critical_preflight_fails_closed_without_set(self):
        self.p.pct=40
        with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
            self.call(35,[{**GOOD,'rpm':[0,1600]}])
        self.assertEqual(self.writes(),[]);self.p.stop.assert_called_once_with('low_calibration_failed')

    def test_native_no_write_refusal_cannot_preserve_unsafe_state(self):
        self.p.pct=40;self.p._send.side_effect=PhysicalError('lower_preflight_changed_no_write')
        with self.assertRaisesRegex(PhysicalError,'low_calibration_failed_AUTO_requested'):
            self.call(35,[GOOD,{**GOOD,'fresh':False}])
        self.p.stop.assert_called_once_with('low_calibration_failed')

if __name__=='__main__':unittest.main()
