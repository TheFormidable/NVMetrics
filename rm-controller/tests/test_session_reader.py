import json
import tempfile
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock
from backend import ReadOnlyAgent
from protocol import ProtocolError

class SessionReaderTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.path=Path(self.tmp.name)/'agent'
        self.mode=self.path.with_suffix('.mode');self.mode.write_text('good')
        self.path.write_text('#!'+sys.executable+'''
import json,sys,time
from pathlib import Path
assert sys.argv[1:]==['fan','sensor-read']
for line in sys.stdin:
 mode=Path(__file__).with_suffix('.mode').read_text()
 if mode=='exit':break
 if mode=='stall':time.sleep(3)
 i=int(line.split()[1])
 d={'temp_gpu_c':46.1,'temp_mem_c':68.,'power_w':49.,'util_gpu_pct':10,'fan_rpm':[1880,1881],'fan_live':{'temperature_fresh':True},'control_request_id':i,'control_fresh':True,'control_sources_advanced':True}
 if mode=='wrong':d['control_request_id']=i-1
 if mode=='stale':d['control_fresh']=False
 if mode=='duplicate':d['control_sources_advanced']=False
 print(json.dumps(d),flush=True)
''');self.path.chmod(0o700)
        self.backend=ReadOnlyAgent(self.path);self.addCleanup(self.backend.close)
    def sample(self,timeout=1):return self.backend.call('status',timeout=timeout,progress=Mock())
    def test_reuses_process_with_distinct_requests(self):
        self.sample();pid=self.backend.reader.pid
        for _ in range(5):self.assertEqual(self.sample()['rpm'],[1880,1881])
        self.assertEqual(self.backend.reader.pid,pid);self.assertEqual(self.backend.request_id,6)
    def test_close_ends_reader(self):
        self.sample();p=self.backend.reader;self.backend.close()
        self.assertIsNotNone(p.poll());self.assertIsNone(self.backend.reader)
    def test_bad_id_is_not_reused(self):
        self.mode.write_text('wrong')
        with self.assertRaisesRegex(ProtocolError,'response_mismatch'):self.sample()
        self.assertIsNone(self.backend.reader)
    def test_stale_and_duplicate_fail_closed(self):
        for mode in ('stale','duplicate'):
            self.mode.write_text(mode)
            with self.assertRaisesRegex(ProtocolError,'telemetry_not_fresh'):self.sample()
            self.assertIsNone(self.backend.reader)
    def test_timeout_closes_reader(self):
        self.mode.write_text('stall')
        with self.assertRaisesRegex(ProtocolError,'agent_unavailable'):self.sample(.1)
        self.assertIsNone(self.backend.reader)
    def test_exit_fails_closed(self):
        self.mode.write_text('exit')
        with self.assertRaisesRegex(ProtocolError,'sensor_reader_exited'):self.sample()
    def test_progress_failure_closes_reader(self):
        with self.assertRaisesRegex(RuntimeError,'guard stopped'):
            self.backend.call('status',progress=Mock(side_effect=RuntimeError('guard stopped')))
        self.assertIsNone(self.backend.reader)
