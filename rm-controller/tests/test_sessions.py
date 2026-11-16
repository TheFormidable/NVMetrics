"""Hardware-free tests of repeat sessions, exclusive lease and fail-closed states."""
import fcntl
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'service'))
import physical
from physical import Physical,PhysicalError
from server import IPCServer

AUTO={'mode':'auto'}
IDLE={'gpu_c':44.,'mem_c':66.,'power_w':9.,'util_gpu_pct':1,'fresh':True,'rpm':[]}

class Rescue:
    def __init__(self,rc=None):self.rc=rc
    def poll(self):return self.rc

class RepeatFlow(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        d=Path(self.tmp.name);self.agent=d/'fake_guard'
        original_root=physical.ROOT;physical.ROOT=d
        self.addCleanup(lambda:setattr(physical,'ROOT',original_root))
        self.agent.write_text('#!'+sys.executable+'''
import json,sys
print(json.dumps({'operation':'guard','event':'ready','pct':0}),flush=True)
for line in sys.stdin:
    line=line.strip()
    if line=='START 50':e,p='manual',50
    elif line.startswith('RAISE '):e,p='manual',int(line.split()[1])
    elif line=='AUTO':e,p='auto_verified',0
    else:continue
    print(json.dumps({'operation':'guard','event':e,'pct':p}),flush=True)
    if line=='AUTO':break
''')
        self.agent.chmod(0o700)
        self.physical=Physical(self.agent,True,lease_path=d/'lease')
        self.rescues=[]
        def arm():
            rescue=Rescue()
            self.rescues.append(rescue)
            self.physical.rescue=rescue
        self.physical._arm_rescue=arm
        # The production implementation checks owner=root. The test runs without root.
        def acquire():
            fd=os.open(d/'lease',os.O_CREAT|os.O_RDWR,0o600)
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            self.physical.lease_fd=fd
        self.physical._acquire_lease=acquire
        self.addCleanup(self.physical.close)
    def start(self):return self.physical.begin(501,IDLE,AUTO)['session']
    def test_manual_auto_rescue_then_repeat(self):
        sid=self.start();self.assertEqual(self.physical.status()['percent'],50)
        for pct in (55,60,65,70,75):
            self.assertEqual(self.physical.raise_percent(501,sid,pct)['percent'],pct)
        self.physical.auto(501,sid)
        self.assertTrue(self.physical.used)
        self.assertTrue(self.physical.status()['rearm_pending'])
        with self.assertRaisesRegex(PhysicalError,'rescue_still_verifying'):
            self.start()
        # The inherited rescue lock is NOT free while both holders exist.
        other=os.open(self.physical.lease_path,os.O_RDWR)
        try:
            with self.assertRaises(BlockingIOError):fcntl.flock(other,fcntl.LOCK_EX|fcntl.LOCK_NB)
        finally:os.close(other)
        self.rescues[-1].rc=0
        s=self.physical.status()
        self.assertFalse(s['one_shot_used'])
        self.assertEqual(s['last_end_reason'],'auto_independently_verified_ready')
        self.assertFalse(s['rearm_pending'])
        sid2=self.start();self.assertNotEqual(sid,sid2)
        self.physical.auto(501,sid2)
        self.rescues[-1].rc=0
        self.assertFalse(self.physical.status()['one_shot_used'])
        self.assertEqual(len(self.rescues),2)
        self.assertTrue((ROOT/'service/physical.py').is_file())
    def test_rescue_failed_fails_closed(self):
        sid=self.start();self.physical.auto(501,sid)
        self.rescues[-1].rc=4
        s=self.physical.status()
        self.assertTrue(s['one_shot_used'])
        self.assertTrue(self.physical.failure)
        self.assertEqual(s['last_end_reason'],'rescue_unverified_restart_service')
        with self.assertRaises(PhysicalError):self.start()
    def test_rescue_alive_refuses_early_rearm(self):
        sid=self.start();self.physical.auto(501,sid)
        for _ in range(5):self.assertTrue(self.physical.status()['one_shot_used'])
        self.assertIsNotNone(self.physical.lease_fd)
    def test_rescue_missing_fails_closed(self):
        sid=self.start();self.physical.auto(501,sid)
        self.physical.rescue=None
        self.physical.status()
        self.assertTrue(self.physical.failure)
    def test_unexpected_guard_exit_refuses_rearm(self):
        sid=self.start()
        self.physical.guard.terminate()
        self.physical.guard.wait(timeout=3)
        self.physical.status()
        self.assertTrue(self.physical.failure)
        self.assertTrue(self.physical.used)
        with self.assertRaises(PhysicalError):self.start()
    def test_rescue_exits_during_manual_requests_auto(self):
        sid=self.start()
        self.rescues[-1].rc=6
        self.physical.status()
        self.assertTrue(self.physical.failure)
        self.assertEqual(self.physical.last_end,'rescue_exited_during_manual_rescue_unverified_restart_service')
        self.assertFalse(self.physical.token)
        with self.assertRaises(PhysicalError):self.start()

if __name__=='__main__':unittest.main()
