"""Real policy paths with simulated driver/IPC failures; never writes hardware."""
import io
import json
import unittest
from pathlib import Path
from unittest.mock import Mock, patch
from physical import Physical, PhysicalError
from protocol import ProtocolError
from server import IPCServer
import test_lower_transition as transition
GOOD=transition.GOOD

class TransitionFailures(unittest.TestCase):
    def setUp(self):
        self.p=Physical(Path('/fake/agent'),True,thermal_profile=True)
        self.p.token='a'*32;self.p.pct=50
        for name in ('_validate','heartbeat','_audit','_poll_child','stop'):
            setattr(self.p,name,Mock())
        self.p._send=Mock()
    def test_failed_probe_requests_auto_before_write(self):
        for fault in (ProtocolError('rpm_not_available'),OSError('driver unavailable')):
            self.p.stop.reset_mock()
            with self.assertRaisesRegex(PhysicalError,'lower_sensor_failed_AUTO_requested'):
                self.p.lower_percent(501,self.p.token,45,probe=Mock(side_effect=fault),inspect={'mode':'manual'})
            self.p._send.assert_not_called();self.p.stop.assert_called_once_with('lower_sensor_failed')
    def test_failed_rm_inspection_requests_auto(self):
        backend=Mock();backend.call.side_effect=ProtocolError('rm_get_failed')
        server=IPCServer('/fake/socket',501,backend,self.p)
        r=server.dispatch(json.dumps({'v':1,'op':'manual_lower','session':self.p.token,'pct':45}).encode()+b'\n',501)
        self.assertFalse(r['ok']);self.p.stop.assert_called_once_with('lower_sensor_failed')
    def test_invalid_request_does_not_terminate_valid_session(self):
        server=IPCServer('/fake/socket',501,Mock(),self.p)
        r=server.dispatch(b'{"v":1,"op":"manual_lower","pct":25}\n',501)
        self.assertFalse(r['ok']);self.p.stop.assert_not_called()
    def test_failed_auto_is_latched_and_rescue_pending(self):
        self.p._send.side_effect=PhysicalError('guard_no_response_or_terminated')
        with self.assertRaises(PhysicalError):self.p.auto(501,self.p.token)
        self.assertTrue(self.p.failure)
        self.p.stop.assert_called_once_with('auto_failed_rescue_pending')
    def test_extra_fan_channel_is_not_ignored(self):
        self.assertFalse(Physical._low_idle_ok(dict(GOOD,rpm=[1500,1500,0]),50,True,True))
    def test_idle_high_speed_is_still_tach_supervised(self):
        self.p.thermal_profile=False;self.p.pct=100
        backend=Mock();backend.call.return_value=dict(GOOD,power_w=20,gpu_c=40,rpm=[0,1500])
        server=IPCServer('/fake/socket',501,backend,self.p)
        server.load_healthcheck()
        backend.call.assert_called_once()
        self.p.stop.assert_called_once_with('load_health_trip_tachometer_below_min')
    def test_consumed_trip_event_survives_exit_drain(self):
        p=Physical(Path('/fake/agent'),True,thermal_profile=True)
        p._audit=Mock();p.token='a'*32;p.used=True
        p.last_guard_safety={'operation':'guard','event':'safety_metrics','reason':'memory_temperature_limit','pct':50}
        p.guard=Mock(returncode=8,stdout=io.BytesIO(b'{"operation":"guard","event":"auto_verified","pct":0}\n'))
        p.guard.poll.return_value=8;p.rescue=Mock();p.rescue.poll.return_value=None
        self.assertTrue(p.status()['rearm_pending'])
        self.assertEqual(p.last_trip,'memory_temperature_limit')

class FullRangeTransition(unittest.TestCase):
    setUp=transition.LowerTransition.setUp
    native=transition.LowerTransition.native
    call=transition.LowerTransition.call
    writes=transition.LowerTransition.writes
    def test_full_descent_100_to_30(self):
        self.p.pct=100
        for target in range(95,29,-5):
            self.assertEqual(self.call(target,[GOOD]*4)['percent'],target)
        self.assertEqual(self.writes(),['LOWER '+str(x) for x in range(95,29,-5)])
        self.p.stop.assert_not_called()
    def test_direct_raise_30_to_100(self):
        self.p.pct=30
        self.assertEqual(self.p.raise_percent(501,self.p.token,100)['percent'],100)
        self.assertEqual(self.writes(),['RAISE 100'])

if __name__=='__main__':unittest.main()
