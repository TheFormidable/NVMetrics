"""Hardware-free independent watchdog checks, including a blocked server."""
import os,sys,threading,time,tempfile,unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'service'))
import rescue,physical

class RescueWatchdog(unittest.TestCase):
 def test_server_stall_triggers_recovery_without_8h_wait(self):
  r,w=os.pipe()
  try:
   started=time.monotonic()
   reason=rescue.supervise(r,os.getppid(),os.getpid(),time.monotonic()+2,timeout=.22)
   self.assertEqual(reason,'server_pulse_timeout')
   self.assertLess(time.monotonic()-started,1)
  finally:os.close(r);os.close(w)
 def test_parent_closing_pipe_causes_early_recovery(self):
  r,w=os.pipe();os.close(w)
  try:self.assertEqual(rescue.supervise(r,os.getppid(),os.getpid(),time.monotonic()+2,timeout=1),'server_pipe_closed')
  finally:os.close(r)
 def test_repeated_authenticated_server_bytes_keep_watchdog_alive(self):
  r,w=os.pipe()
  def pulse():
   try:
    for _ in range(6):os.write(w,b'.');time.sleep(.045)
   finally:os.close(w)
  thread=threading.Thread(target=pulse);thread.start()
  try:
   self.assertEqual(rescue.supervise(r,os.getppid(),os.getpid(),time.monotonic()+1.5,timeout=.15),'server_pipe_closed')
  finally:os.close(r);thread.join()
 def test_root_physical_sends_only_to_inherited_pipe(self):
  r,w=os.pipe();ph=physical.Physical(Path('/fake'),False)
  ph.watchdog_write_fd=w
  try:
   ph.watchdog_tick();self.assertEqual(os.read(r,1),b'.')
   ph._close_watchdog();self.assertEqual(os.read(r,1),b'')
  finally:os.close(r)
 def test_readonly_mode_refuses_manual(self):
  p=physical.Physical(Path('/fake'),False)
  with self.assertRaisesRegex(physical.PhysicalError,'physical_mode_disabled'):
   p.begin(501,{'gpu_c':45}, {'mode':'auto'})
 def test_preflight_blocks_gpu_load(self):
  s={'gpu_c':45.,'mem_c':60.,'power_w':100.,'util_gpu_pct':70,'fresh':True,'rpm':[]}
  self.assertFalse(physical.Physical.idle_ok(s,{'mode':'auto'}))
 def test_no_corrupt_stale_fan_stop(self):
  s={'gpu_c':45.,'mem_c':60.,'power_w':9.,'util_gpu_pct':2,'fresh':False,'rpm':[]}
  self.assertFalse(physical.Physical.idle_ok(s,{'mode':'auto'}))
if __name__=='__main__':unittest.main()
