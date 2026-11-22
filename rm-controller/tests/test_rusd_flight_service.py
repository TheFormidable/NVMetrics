"""Hardware-free tests of the root service side of the RUSD flight recorder:
the guard inherits an append-only, root-only JSONL fd and a run_id; a guard
trip's exact RUSD cause reaches controller-events.jsonl; the fail-closed
rescue flow after `gpu_temperature_timestamp_invalid` is unchanged."""
import fcntl
import json
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'service'))
import physical
from physical import Physical

IDLE={'gpu_c':44.,'mem_c':66.,'power_w':9.,'util_gpu_pct':1,'fresh':True,'rpm':[]}

class Rescue:
    def __init__(self,rc=None):self.rc=rc
    def poll(self):return self.rc

# A fake guard that behaves like the C guard on the wire. TRIP_AFTER_START makes it
# report the 18-Nov signature with the flight recorder's new fields, then exit 8.
GUARD='''#!{py}
import json,os,sys
fd=os.environ.get('NVMETRICS_RUSD_FR_FD');run=os.environ.get('NVMETRICS_RUSD_FR_RUN')
def fr(obj):
    if fd:os.write(int(fd),(json.dumps(dict(rec='rusd_fr',v=1,run=run,**obj))+'\\n').encode())
# like the C guard (0.15.7): nothing is written to the sink before the session ends
print(json.dumps({{'operation':'guard','event':'ready','pct':0}}),flush=True)
for line in sys.stdin:
    line=line.strip()
    if line=='START 50':
        print(json.dumps({{'operation':'guard','event':'manual','pct':50,'set_status':0}}),flush=True)
        if {trip}:
            print(json.dumps({{'operation':'guard','event':'safety_metrics','reason':'gpu_temperature_timestamp_invalid',
                'pct':50,'fan_fresh':False,'gpu_ts_ok':False,'gpu_temp_age_ms':18446744073709551615,
                'rusd_cause':'gpu:seqlock_timeout','rusd':{{'gpu':{{'cause':'seqlock_timeout','ts':18446744073709551615,
                'ts_s64':-1,'first_ok':0,'retries':3}}}}}}),flush=True)
            print(json.dumps({{'operation':'guard','event':'safety_auto','pct':50,'reason':'sensor_thermal_power_or_util'}}),flush=True)
            print(json.dumps({{'operation':'guard','event':'termination','pct':50,'reason':'safety_condition'}}),flush=True)
            print(json.dumps({{'operation':'guard','event':'auto_verified','pct':0}}),flush=True)
            fr(dict(kind='session_begin'));fr(dict(kind='dump_begin',segment='trip'))
            fr(dict(kind='session_summary',exit_code=8))
            sys.exit(8)
    elif line=='AUTO':
        print(json.dumps({{'operation':'guard','event':'auto_verified','pct':0}}),flush=True)
        fr(dict(kind='session_begin'));fr(dict(kind='session_summary',exit_code=0))
        break
'''

class FlightService(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.d=Path(self.tmp.name)
        original=physical.ROOT;physical.ROOT=self.d
        self.addCleanup(lambda:setattr(physical,'ROOT',original))

    def make(self,trip):
        agent=self.d/'fake_guard'
        agent.write_text(GUARD.format(py=sys.executable,trip='True' if trip else 'False'))
        agent.chmod(0o700)
        p=Physical(agent,True,lease_path=self.d/'lease',thermal_profile=True)
        self.rescues=[]
        def arm():
            r=Rescue();self.rescues.append(r);p.rescue=r
        p._arm_rescue=arm
        def acquire():
            fd=os.open(self.d/'lease',os.O_CREAT|os.O_RDWR,0o600)
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB);p.lease_fd=fd
        p._acquire_lease=acquire
        p._load_spinup_probe=lambda probe:None
        self.addCleanup(p.close)
        return p

    def events(self):
        return [json.loads(l) for l in (self.d/'controller-events.jsonl').read_text().splitlines()]

    def flight(self):
        return [json.loads(l) for l in (self.d/physical.FLIGHT_LOG).read_text().splitlines()]

    def test_guard_inherits_root_only_sink_with_run_id(self):
        p=self.make(trip=False)
        sid=p.begin(501,IDLE,{'mode':'auto'},probe=lambda:None)['session']
        p.auto(501,sid)
        lines=self.flight()
        self.assertEqual([l['kind'] for l in lines],['session_begin','session_summary'])
        self.assertTrue(all(l['run']==p.run_id for l in lines))
        mode=os.stat(self.d/physical.FLIGHT_LOG).st_mode&0o777
        self.assertEqual(mode,0o600)
        self.assertIn({'sink':True,'log':physical.FLIGHT_LOG},
                      [{k:e[k] for k in ('sink','log')} for e in self.events() if e['event']=='rusd_flight_recorder'])

    def test_trip_cause_is_audited_and_rescue_flow_is_unchanged(self):
        p=self.make(trip=True)
        p.begin(501,IDLE,{'mode':'auto'},probe=lambda:None)   # START verified, then the guard trips
        for _ in range(100):
            if p.guard.poll() is not None:break
            time.sleep(0.02)
        s=p.status()
        self.assertTrue(p.failure)
        self.assertEqual(s['last_end_reason'],'guard_terminated_gpu_temperature_timestamp_invalid_rescue_pending')
        term=[e for e in self.events() if e['event']=='guard_terminated_unexpected'][-1]
        self.assertEqual(term['detail'],'gpu_temperature_timestamp_invalid')
        self.assertEqual(term['rusd_cause'],'gpu:seqlock_timeout')
        exits=[e['guard_event'] for e in self.events() if e['event']=='guard_exit_event']
        self.assertTrue(any(e.get('rusd',{}).get('gpu',{}).get('ts_s64')==-1 for e in exits))
        # independent rescue verifies AUTO -> the same latched end state as before this change
        self.rescues[-1].rc=0
        self.assertEqual(p.status()['last_end_reason'],
                         'guard_terminated_gpu_temperature_timestamp_invalid_rescue_verified_restart_service')
        self.assertFalse(p.status()['active'])
        kinds=[l['kind'] for l in self.flight()]
        self.assertIn('dump_begin',kinds);self.assertEqual(kinds[-1],'session_summary')

    def test_sink_rotates_and_refuses_unsafe_files(self):
        p=self.make(trip=False)
        path=self.d/physical.FLIGHT_LOG
        original=physical.FLIGHT_MAX_BYTES;physical.FLIGHT_MAX_BYTES=10
        self.addCleanup(lambda:setattr(physical,'FLIGHT_MAX_BYTES',original))
        path.write_text('x'*64);path.chmod(0o600)
        fd=p._open_flight_log();self.assertIsNotNone(fd);os.close(fd)
        self.assertEqual((self.d/'rusd-flight.jsonl.1').read_text(),'x'*64)
        self.assertEqual(path.stat().st_size,0)
        path.chmod(0o644)                                   # group/world readable: refused, audited
        self.assertIsNone(p._open_flight_log())
        self.assertEqual(self.events()[-1]['event'],'rusd_flight_log_refused')
        path.unlink();path.symlink_to(self.d/'elsewhere')    # never follow a link
        self.assertIsNone(p._open_flight_log())
        self.assertFalse((self.d/'elsewhere').exists())

class FlightReport(unittest.TestCase):
    """diagnostics/rusd_flight_report.py: counts come from session_summary.causes, dumped samples never
    prove absence, and the origin is stated with an explicit certainty."""
    def report(self,lines,rc=0):
        import contextlib,io
        sys.path.insert(0,str(ROOT/'diagnostics'))
        import rusd_flight_report
        with tempfile.NamedTemporaryFile('w',suffix='.jsonl',delete=False) as f:
            for l in lines:f.write(json.dumps({'rec':'rusd_fr','v':2,'run':'ab','pid':1,**l})+'\n')
        self.addCleanup(os.unlink,f.name)
        out=io.StringIO()
        with contextlib.redirect_stdout(out):self.assertEqual(rusd_flight_report.main(['x',f.name]),rc)
        return out.getvalue()
    def sample(self,seq,cause,segment='trip',**gpu):
        src={k:{'cause':'ok','ts':10+seq,'ts_s64':10+seq,'live_before':10+seq,'live_after':10+seq,'age_ms':250}
             for k in ('gpu','mem','power','fan','util')}
        src['gpu'].update(cause=cause,**gpu)
        return {'kind':'sample','segment':segment,'seq':seq,'phase':'active','conf_pct':30,'src':src}
    def summary(self,causes,recovered=0,retried=0):
        return {'kind':'session_summary','exit_code':8,'why':'safety_condition','samples':100,'windows':1,
                'windows_kept':1,'windows_overwritten':0,'seqlock_retried':retried,'seqlock_retry_recovered':recovered,
                'retry_max_us':300,'first_anomaly':None,'causes':causes}
    def test_producer_zero_is_the_only_high_certainty_origin(self):
        out=self.report([self.sample(2,'ts_zero_producer_invalid',ts=0,ts_s64=0),
                         self.summary({'gpu':{'ok':99,'ts_zero_producer_invalid':1}})])
        self.assertIn('ts_zero_producer_invalid: 1 muestras. Origen: productor (certeza alta)',out)
        self.assertNotIn('Ninguna causa observada identifica',out)
    def test_seqlock_timeout_is_not_attributed_to_one_side(self):
        out=self.report([self.sample(2,'seqlock_timeout',ts=2**64-1,ts_s64=-1),
                         self.summary({'gpu':{'ok':99,'seqlock_timeout':1}},retried=1)])
        self.assertIn('no atribuible en exclusiva (registro 0.15.7) (certeza baja)',out)
        self.assertIn('Ninguna causa observada identifica el origen con certeza alta',out)
        self.assertNotIn('consumer',out.lower())
    def test_recovered_retries_are_reported_without_claiming_a_cause(self):
        out=self.report([self.sample(2,'ok',segment='anomaly',first_cause='seqlock_timeout',retries=1),
                         self.summary({'gpu':{'ok':100}},recovered=1,retried=1)])
        self.assertIn('1 lectura(s) chocaron con una publicación y se recuperaron',out)
        self.assertIn('no indica por sí solo',out)
        self.assertNotIn('Sin anomalías',out)
    def test_summary_without_samples_is_not_a_false_negative(self):
        # windows overwritten in a long session: the summary still counts every sample
        out=self.report([self.summary({'gpu':{'ok':5000,'seqlock_timeout':2},'fan':{'ok':5000,'ts_frozen':12}})])
        self.assertIn('seqlock_timeout=2 muestras',out);self.assertIn('ts_frozen=12 muestras',out)
        self.assertNotIn('Sin anomalías',out)
    def test_samples_without_summary_are_marked_incomplete(self):
        out=self.report([self.sample(1,'ok'),self.sample(2,'write_in_progress',ts=2**64-1,ts_s64=-1),
                         self.sample(2,'write_in_progress',ts=2**64-1,ts_s64=-1)])
        self.assertIn('SIN session_summary',out)
        self.assertIn('write_in_progress: 1 muestras',out)          # the duplicated seq counts once
        self.assertIn('las cifras pueden ser incompletas',out)
    def test_clean_and_empty_logs(self):
        self.assertIn('Sin anomalías RUSD registradas en 100 muestras',self.report([self.summary({'gpu':{'ok':100}})]))
        self.assertIn('no concluyente',self.report([self.sample(1,'ok')]))
    def test_empty_log_is_inconclusive_not_clean(self):
        # 0.15.8 item 7: an empty log used to end in "Sin anomalías RUSD registradas" (a false negative)
        for lines in ([],[{'kind':'guard_event','event':'auto_verified','pct':0,'reason':'x','seq':0}],
                      [self.summary({})]):
            out=self.report(lines,rc=1)
            self.assertIn('No concluyente: no hay ninguna muestra registrada',out)
            self.assertNotIn('Sin anomalías',out)
    def test_incident_0157_record_is_flagged_and_not_blamed_on_the_producer(self):
        # the real 18-Nov record: first_ok=0, write_in_progress, retries=0, retry_us=13388
        smp=self.sample(30,'write_in_progress',ts=2**64-1,ts_s64=-1,live_after=2**64-1,first_ok=0,
                        first_cause='write_in_progress',retries=0,retry_us=13388)
        summ=self.summary({'gpu':{'ok':30,'write_in_progress':1}})
        summ.update(retry_paths=1,seqlock_retried=0,retry_max_us=13388)
        out=self.report([smp,summ])
        self.assertIn('1 camino(s) de reintento terminaron SIN ningún reintento',out)
        self.assertIn('write_in_progress: 1 muestras. Origen: no atribuible (registro 0.15.7) (certeza baja)',out)
        self.assertIn('retry_max_us=13388',out)
        self.assertNotIn('Origen: productor',out)
        # without the summary the samples alone still reveal it
        out=self.report([smp])
        self.assertIn('1 camino(s) de reintento terminaron SIN ningún reintento',out)
    def test_new_retry_endings_have_their_own_origin(self):
        out=self.report([self.summary({'gpu':{'ok':90,'producer_write_persistent':1,'scheduler_preempted':2,
                                              'retry_budget_exhausted':3}},retried=6)])
        self.assertIn('producer_write_persistent: 1 muestras. Origen: productor (certeza alta)',out)
        self.assertIn('scheduler_preempted: 2 muestras. Origen: planificador de macOS (observación interrumpida) (certeza media)',out)
        self.assertIn('retry_budget_exhausted: 3 muestras. Origen: no atribuible en exclusiva (certeza baja)',out)

if __name__=='__main__':
    unittest.main()
