import json
import unittest
from types import SimpleNamespace
from unittest.mock import Mock
from server import IPCServer
from protocol import parse_agent, ProtocolError

AUTO={'mode':'auto','regular':[{'mode':0,'q16':0}]*2,'default':[{'mode':0,'q16':0}]*2}
DATA={'gpu_c':42.8,'mem_c':62.,'rpm':[],'power_w':18.,'util_gpu_pct':1,'fresh':True,'pstate':5,'clock_gpu_mhz':300}

class AutoObservation(unittest.TestCase):
    def setUp(self):
        self.physical=SimpleNamespace(token=None,used=False)
        self.backend=Mock()
        self.server=IPCServer('/unused',501,self.backend,self.physical)
    def observe(self,uid=501):
        return self.server.dispatch(b'{"v":1,"op":"auto_observe"}\n',uid)
    def test_valid_zero(self):
        self.backend.call.side_effect=[AUTO,DATA,AUTO]
        r=self.observe();self.assertTrue(r['ok']);self.assertEqual(r['data']['rpm'],[])
        self.assertTrue(r['data']['auto_bracketed']);self.assertEqual(r['data']['pstate'],5)
        self.assertEqual([c.args[0] for c in self.backend.call.call_args_list],['inspect','status','inspect'])
    def test_spinning(self):
        self.backend.call.side_effect=[AUTO,{**DATA,'rpm':[1150,1176]},AUTO]
        self.assertEqual(self.observe()['data']['rpm'],[1150,1176])
    def test_active_or_rescue(self):
        for token,used in [('active',False),(None,True)]:
            self.physical.token=token;self.physical.used=used
            self.assertEqual(self.observe()['error'],'observation_manual_or_rescue')
        self.backend.call.assert_not_called()
    def test_manual_initial(self):
        self.backend.call.return_value={'mode':'manual'}
        self.assertEqual(self.observe()['error'],'observation_requires_auto')
        self.assertEqual(self.backend.call.call_count,1)
    def test_auto_changed(self):
        self.backend.call.side_effect=[AUTO,DATA,{'mode':'manual'}]
        self.assertEqual(self.observe()['error'],'observation_auto_changed')
    def test_bad_telemetry(self):
        for patch in ({'fresh':False},{'rpm':None},{'rpm':[900]},{'rpm':[6000,6000]},{'gpu_c':float('nan')},{'power_w':None},{'util_gpu_pct':None}):
            self.backend.call.side_effect=[AUTO,{**DATA,**patch},AUTO]
            self.assertFalse(self.observe()['ok'],patch)
    def test_unauthorized(self):
        self.assertEqual(self.observe(502)['error'],'unauthorized_peer');self.backend.call.assert_not_called()
    def test_parse_optional_clocks(self):
        raw={'temp_gpu_c':42.8,'temp_mem_c':62,'fan_rpm':[],'fan_live':{'temperature_fresh':True},'power_w':18,'util_gpu_pct':0,'pstate':5,'clock_gpu_mhz':300}
        result=parse_agent(json.dumps(raw),'status')
        self.assertEqual(result['pstate'],5);self.assertEqual(result['clock_gpu_mhz'],300)
        raw['fan_rpm']=None
        with self.assertRaises(ProtocolError):parse_agent(json.dumps(raw),'status')

if __name__=='__main__':unittest.main()
