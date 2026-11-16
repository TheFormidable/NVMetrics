import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch
from physical import Physical
from server import IPCServer


class ThermalSupervisorTests(unittest.TestCase):
    def setUp(self):
        self.p=Physical(Path('/fake/agent'),True,thermal_profile=True)
        self.p._audit=Mock()
        self.s=IPCServer('/fake/socket',501,Mock(),self.p)
        self.p.pct=100
        self.sample={'gpu_c':42.62,'mem_c':62.,'power_w':18.45,'util_gpu_pct':21,'fresh':True,'rpm':[3650,3651]}

    def test_real_trip_sample_no_longer_aborts(self):
        self.assertIsNone(self.s.health_reason(self.sample))
        reply=self.s.dispatch(b'{"v":1,"op":"ping"}\n',501)
        self.assertEqual(reply['safety_profile'],'thermal_supervised')
        self.assertEqual(reply['manual_floor'],30)

    def test_load_and_thermal_limits(self):
        self.assertIsNone(self.s.health_reason(dict(self.sample,util_gpu_pct=100,power_w=100)))
        self.assertEqual(self.s.health_reason(dict(self.sample,gpu_c=65.)),'thermal_limit')
        self.assertEqual(self.s.health_reason(dict(self.sample,mem_c=88.)),'thermal_limit')
        self.assertEqual(self.s.health_reason(dict(self.sample,power_w=146.)),'thermal_or_power')
        self.assertEqual(self.s.health_reason(dict(self.sample,fresh=False)),'stale')
        self.assertEqual(self.s.health_reason(dict(self.sample,rpm=[0,1100])),'tachometer_below_min')
        self.p.pct=25
        self.assertEqual(self.s.health_reason(self.sample),'manual_floor_violation')

    def fake_exit(self,reason,rc=8,rescue_rc=0,auto=True):
        self.p.token='a'*32;self.p.used=True
        events=[{'operation':'guard','event':'safety_metrics','reason':reason,'pct':100}]
        if auto:events.append({'operation':'guard','event':'auto_verified','pct':0})
        self.p.guard=Mock(returncode=rc,stdout=io.BytesIO(b''.join((json.dumps(e)+'\n').encode() for e in events)))
        self.p.guard.poll.return_value=rc
        self.p.rescue=Mock();self.p.rescue.poll.return_value=rescue_rc
        self.p.lease_fd=os.open(os.devnull,os.O_RDONLY)
        self.addCleanup(lambda:os.close(self.p.lease_fd) if self.p.lease_fd is not None else None)

    def test_thermal_trip_requires_rescue_and_cooldown(self):
        self.fake_exit('gpu_temperature_limit',rescue_rc=None)
        self.assertTrue(self.p.status()['rearm_pending'])
        self.p.rescue.poll.return_value=0
        self.assertTrue(self.p.status()['one_shot_used'])
        self.p.thermal_rearm_after=0
        self.assertFalse(self.p.status()['one_shot_used'])
        self.assertEqual(self.p.last_trip,'gpu_temperature_limit')

    def test_thermal_trip_without_auto_event_remains_latched(self):
        self.fake_exit('gpu_temperature_limit',auto=False)
        self.assertTrue(self.p.status()['one_shot_used'])
        self.assertFalse(self.p.status()['rearm_pending'])

    def test_sensor_fault_remains_latched(self):
        self.fake_exit('fan_timestamp_invalid')
        self.assertTrue(self.p.status()['one_shot_used'])
        self.assertFalse(self.p.status()['rearm_pending'])

    def test_heartbeat_trip_rearms_only_after_independent_auto(self):
        self.fake_exit('heartbeat_timeout',rescue_rc=None)
        self.assertTrue(self.p.status()['rearm_pending'])
        self.p.rescue.poll.return_value=0
        self.assertFalse(self.p.status()['one_shot_used'])
        self.assertEqual(self.p.last_end,'auto_independently_verified_ready')

    def test_heartbeat_trip_without_auto_stays_latched(self):
        self.fake_exit('heartbeat_timeout',auto=False)
        self.assertTrue(self.p.status()['one_shot_used'])
        self.assertFalse(self.p.status()['rearm_pending'])

    def test_failed_rescue_never_rearms(self):
        self.fake_exit('gpu_temperature_limit',rescue_rc=4)
        self.assertTrue(self.p.status()['one_shot_used'])
        self.assertFalse(self.p.status()['rearm_pending'])

    def test_lowering_high_speed_does_not_use_idle_limit(self):
        self.assertTrue(self.p._low_idle_ok(self.sample,95,True))
        self.assertFalse(self.p._low_idle_ok(self.sample,45,False))

    def test_high_idle_power_report_and_distinct_entry_limits(self):
        observed=dict(self.sample,gpu_c=55.8,mem_c=78.,power_w=47.81,util_gpu_pct=1,rpm=[1176,1173])
        self.assertFalse(self.p.idle_ok(observed,{'mode':'auto'}))
        self.assertTrue(self.p.idle_ok(observed,{'mode':'auto'},True))
        self.assertTrue(self.p._low_idle_ok(observed,30,False,True))
        for key,value in [('gpu_c',60),('mem_c',84),('power_w',60.01),('util_gpu_pct',21),('fresh',False)]:
            self.assertFalse(self.p.idle_ok(dict(observed,**{key:value}),{'mode':'auto'},True))
        for key,value in [('gpu_c',58),('mem_c',82),('power_w',50.01),('rpm',[0,1173])]:
            self.assertFalse(self.p._low_idle_ok(dict(observed,**{key:value}),30,False,True))

    def test_background_progress_does_not_renew_gui_lease(self):
        self.p.token='a'*32;self.p.last_heartbeat=100.;self.p.last_guard_pulse=100.
        self.p._send=Mock();self.p.watchdog_tick=Mock()
        with patch('physical.time.monotonic',return_value=102.):self.p.guard_progress()
        self.p._send.assert_called_once_with('PING',None)
        self.assertEqual(self.p.last_heartbeat,100.)
        self.p._send.reset_mock()
        with patch('physical.time.monotonic',return_value=106.):self.p.guard_progress()
        self.p._send.assert_not_called()

    def test_low_margin_refuses_before_set(self):
        from physical import PhysicalError
        self.p.token='a'*32;self.p.pct=50
        self.p._validate=Mock();self.p.heartbeat=Mock();self.p._send=Mock()
        with self.assertRaisesRegex(PhysicalError,'lower_thermal_margin_insufficient_no_write'):
            self.p.lower_percent(501,self.p.token,45,probe=lambda:dict(self.sample,power_w=55,util_gpu_pct=1),inspect={'mode':'manual'})
        self.p._send.assert_not_called();self.assertEqual(self.p.pct,50)
