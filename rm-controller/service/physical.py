"""Bounded privileged physical control bridge to the already-validated C v0.4 guard.
Sequential sessions only after AUTO and the previous root rescue independently verify RM. No direct private RM writes in Python.
The C guard has its own 8-hour hard limit, 6s heartbeat and 250ms sensor checks.
Detached rescue monitors parent health via a private inherited pipe (not UID client traffic).
"""
from __future__ import annotations
import fcntl
import json
import os
from pathlib import Path
import select
import secrets
import stat
import subprocess
import sys
import time

from protocol import ProtocolError,rpm_floor,MANUAL_FLOOR

ROOT=Path('/Library/PrivilegedHelperTools/org.nvmetrics.rmguard')
LEASE=Path('/var/run/nvmetrics-rm-v05-session.lock')
MAX_MANUAL_SECONDS=8*3600
RESCUE_DELAY_SECONDS=8*3600+15

class PhysicalError(ValueError):
    def __init__(self,message,*,sample=None,requested=None,applied=None):
        super().__init__(message)
        self.details=None if sample is None else {
            'sample':{k:sample.get(k) for k in ('gpu_c','mem_c','power_w','util_gpu_pct','rpm','fresh')},
            'requested_percent':requested,'applied_percent':applied}


class Physical:
    def __init__(self,agent:Path,enabled:bool=False,rescue_module:Path|None=None,lease_path:Path=LEASE,load_profile:bool=False,thermal_profile:bool=False):
        self.agent=agent
        self.enabled=enabled
        self.load_profile=bool(load_profile)
        self.thermal_profile=bool(thermal_profile) and not self.load_profile
        self.rescue_module=rescue_module or ROOT/'rescue.py'
        self.lease_path=lease_path
        self.guard=None
        self.rescue=None
        self.lease_fd=None
        self.token=None
        self.pct=0
        self.start_time=0.
        self.last_heartbeat=0.
        self.last_guard_pulse=0.
        self.last_end='never_started'
        self.used=False
        self.failure=False
        self.guard_log=None
        self.watchdog_write_fd=None
        self.service_deadline=None
        self.run_id=None
        self.last_watchdog_sent=0.
        self.last_guard_event=None
        self.last_rm_transaction=None
        self.last_guard_safety=None
        self.last_trip=None
        self.last_trip_percent=None
        self.last_change_time=0.
        self.thermal_rearm_after=0.

    @property
    def safety_profile(self):
        return 'thermal_supervised' if self.thermal_profile else 'attended_load_test' if self.load_profile else 'idle_only'

    def _recoverable_thermal(self):
        return self.thermal_profile and self.failure and self.last_end in (
            'guard_thermal_trip_rescue_pending','server_thermal_trip_rescue_pending')

    def _recoverable_heartbeat(self):
        # Only an explicit native heartbeat trip with verified AUTO can rearm.
        # A missing guard event, unknown fault or unverified rescue stays latched.
        return self.thermal_profile and self.failure and self.last_end=='guard_heartbeat_trip_rescue_pending'

    def _recoverable_tach(self):
        # Only an absent sample in the low-speed IDLE profile is eligible.
        # Explicit stall/low RPM, C guard faults, thermal trips and rescue
        # failures retain the original fail-closed latch.
        return (not self.load_profile and self.failure and self.last_trip_percent in (None,30,35,40,45) and
                self.last_end=='load_health_trip_tachometer_missing_rescue_pending')

    def _audit(self,event,**extra):
        # Root-only JSONL: session identifier is a diagnostic nonce, not the IPC bearer token.
        # Never write user-provided strings as shell commands or filesystem paths.
        obj={'t':round(time.time(),3),'event':event,'run_id':self.run_id,**extra}
        path=ROOT/'controller-events.jsonl'
        flags=os.O_WRONLY|os.O_APPEND|os.O_CREAT|getattr(os,'O_NOFOLLOW',0)
        fd=os.open(path,flags,0o600)
        try:
            st=os.fstat(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_uid!=os.geteuid() or st.st_nlink!=1 or st.st_mode&0o077:
                raise PhysicalError('unsafe_audit_log')
            os.write(fd,(json.dumps(obj,separators=(',',':'),ensure_ascii=True)+'\n').encode('ascii'))
        finally:os.close(fd)

    def watchdog_tick(self):
        """Only the running privileged server can keep the independent rescue armed.
        Server blocking/death stops these bytes; rescue then verifies/restores AUTO.
        """
        if self.watchdog_write_fd is None:return
        now=time.monotonic()
        if now-self.last_watchdog_sent<0.5:return
        try:
            os.write(self.watchdog_write_fd,b'.')
            self.last_watchdog_sent=now
        except (BlockingIOError,OSError):
            # Once AUTO was read back, the rescue closing its read end is normal.
            # Only a lost watchdog DURING MANUAL is an immediate fail-safe.
            self._close_watchdog()
            if self.token is not None:
                self.failure=True
                self.stop('watchdog_pipe_failed')

    def _close_watchdog(self):
        fd=self.watchdog_write_fd
        self.watchdog_write_fd=None
        if fd is not None:
            try:os.close(fd)
            except OSError:pass

    def status(self)->dict:
        self._poll_child()
        left=max(0.,MAX_MANUAL_SECONDS-(time.monotonic()-self.start_time)) if self.token else 0.
        if self.token and self.service_deadline is not None:
            left=min(left,max(0.,self.service_deadline-time.monotonic()))
        return {'active':self.token is not None,'mode':'manual' if self.token else 'auto_or_unverified',
                'percent':self.pct,'seconds_left':round(left,1),'physical_enabled':self.enabled,
                'safety_profile':self.safety_profile,
                'one_shot_used':self.used,'rearm_pending':self.used and (self._recoverable_tach() or self._recoverable_thermal() or self._recoverable_heartbeat() or (not self.failure and self.last_end=='user_auto_verified')),
                'last_end_reason':self.last_end,
                'last_trip':self.last_trip,
                'last_trip_percent':self.last_trip_percent,
                'manual_floor':50 if self.load_profile else MANUAL_FLOOR,
                'last_rm_transaction':self.last_rm_transaction,
                'last_guard_safety':self.last_guard_safety,
                'readback_verified':self.token is not None and not self.failure,'watchdog_armed':self.rescue is not None and self.rescue.poll() is None}

    def _poll_child(self):
        if (self.token is not None and self.guard and self.guard.poll() is None
                and self.rescue is not None and self.rescue.poll() is not None):
            # A dead backup while MANUAL is active is an immediate fail-safe.
            self.failure=True
            self.stop('rescue_exited_during_manual')
        if self.guard and self.guard.poll() is not None and self.token is not None:
            detail='unreported'
            # A synchronous transaction may already have consumed the trip's
            # first event. Keep that evidence when draining the remaining EOF.
            guard_events=[]
            for ev in (self.last_guard_safety,self.last_guard_event):
                if isinstance(ev,dict) and ev.get('event') in ('safety_metrics','safety_auto','failure','auto_failed','auto_verified') and ev not in guard_events:
                    guard_events.append(ev)
            if self.guard.stdout:
                try:
                    # Pipe is at EOF because guard.poll() has already returned an exit.
                    # Preserve each event: the old loop overwrote the specific
                    # safety_auto reason with the generic termination reason.
                    for line in self.guard.stdout.read(8192).splitlines():
                        try:ev=json.loads(line)
                        except (UnicodeDecodeError,ValueError,TypeError):continue
                        if isinstance(ev,dict) and ev.get('operation')=='guard':
                            guard_events.append(ev)
                except OSError:pass
            for ev in guard_events:
                if ev.get('event')=='safety_metrics':
                    self.last_guard_safety=ev
                if 'set_status' in ev and ev.get('event') in ('manual','failure'):
                    self.last_rm_transaction=ev
                if ev.get('event') in ('safety_metrics','safety_auto','failure','termination','auto_failed','auto_verified'):
                    # Only agent-generated fields are logged. No IPC token is present.
                    self._audit('guard_exit_event',guard_event=ev)
            # Most informative event takes precedence over later generic messages.
            for event_type in ('safety_metrics','safety_auto','failure','termination','auto_failed'):
                found=next((ev for ev in guard_events if ev.get('event')==event_type),None)
                if found is not None:
                    detail=str(found.get('reason','unspecified'))[:80]
                    break
            self._audit('guard_terminated_unexpected',exit_code=self.guard.returncode,detail=detail)
            self.token=None;self.pct=0;self.failure=True
            self.last_end='guard_terminated_'+detail+'_rescue_pending'
            if (self.thermal_profile and self.guard.returncode==8 and detail in
                ('gpu_temperature_limit','memory_temperature_limit','thermal_rise_near_limit') and
                any(ev.get('event')=='auto_verified' and ev.get('pct')==0 for ev in guard_events)):
                self.last_trip=detail;self.last_trip_percent=(self.last_guard_safety or {}).get('pct')
                self.last_end='guard_thermal_trip_rescue_pending'
                self.thermal_rearm_after=time.monotonic()+10
            elif (self.thermal_profile and self.guard.returncode==8 and detail=='heartbeat_timeout' and
                  any(ev.get('event')=='auto_verified' and ev.get('pct')==0 for ev in guard_events)):
                self.last_trip=detail;self.last_trip_percent=(self.last_guard_safety or {}).get('pct')
                self.last_end='guard_heartbeat_trip_rescue_pending'
        # Unexpected guard endings remain fail-closed.  Still report whether the
        # *independent* rescue eventually confirmed AUTO, so the GUI does not
        # conflate "rearm forbidden" with "rescue failed".
        thermal=self._recoverable_thermal()
        heartbeat=self._recoverable_heartbeat()
        recoverable=self._recoverable_tach() or thermal or heartbeat
        if self.failure and not recoverable and self.last_end.endswith('_rescue_pending') and self.rescue is not None:
            rc=self.rescue.poll()
            if rc is not None:
                if rc==0:
                    self.last_end=self.last_end[:-len('_rescue_pending')]+'_rescue_verified_restart_service'
                    self._audit('failure_rescue_verified_auto',exit_code=rc)
                else:
                    self.last_end=self.last_end[:-len('_rescue_pending')]+'_rescue_unverified_restart_service'
                    self._audit('failure_rescue_unverified',exit_code=rc)
        # A successful manual AUTO is not sufficient to re-arm: the independent
        # root rescue MUST also finish, verify RM AUTO, and release its flock.
        # Failure is deliberately fail-closed until the service is restarted.
        if not self.used or self.token or not (recoverable or (not self.failure and self.last_end=='user_auto_verified')):
            return
        if not self.guard or self.guard.poll() is None:
            return
        # An explicit parent EOF normally returns C exit code 12. A C safety
        # fault or RM readback failure could race the tachometer probe; never
        # reinterpret that independent guard failure as a recoverable gap.
        if recoverable and not thermal and not heartbeat and self.guard.poll()!=12:
            self.last_end='tachometer_missing_guard_fault_rescue_pending'
            self._audit('tachometer_rearm_refused_guard_fault',exit_code=self.guard.poll())
            return
        if thermal and self.guard.poll()!=(8 if self.last_end=='guard_thermal_trip_rescue_pending' else 12):
            self.last_end='thermal_rearm_guard_fault_rescue_pending';return
        if heartbeat and self.guard.poll()!=8:
            self.last_end='heartbeat_rearm_guard_fault_rescue_pending';return
        if self.rescue is None:
            self.failure=True;self.last_end='rescue_missing_restart_service';self._audit('rescue_missing');return
        rescue_rc=self.rescue.poll()
        if rescue_rc is None:return
        if rescue_rc != 0:
            self.failure=True;self.last_end='rescue_unverified_restart_service';self._audit('rescue_failed',exit_code=rescue_rc);return
        if thermal and time.monotonic()<self.thermal_rearm_after:return
        # The independent rescue returned 0 only after both RM records read AUTO.
        # It no longer owns the inherited lock. Release the server's FD too:
        # a second open+flock by this process would otherwise fail on macOS.
        if self.lease_fd is None:
            self.failure=True;self.last_end='lease_missing_restart_service';return
        os.close(self.lease_fd);self.lease_fd=None
        self._close_watchdog()
        if self.guard.stdout:
            self.guard.stdout.close()
        if self.guard_log:
            self.guard_log.close();self.guard_log=None
        self.guard=None;self.rescue=None
        self.used=False
        self.failure=False
        self.last_end='auto_independently_verified_ready'
        self._audit('rescue_verified_auto_rearmed',previous_trip=self.last_trip,automatic_recovery=recoverable)

    @staticmethod
    def idle_ok(s,rm,thermal=False,low=False):
        import math
        try:
            gpu=s['gpu_c'];mem=s['mem_c'];power=s['power_w'];util=s['util_gpu_pct']
            g,m,w=(58,82,50) if thermal and low else (60,84,60) if thermal else (55,82,35)
            return rm['mode']=='auto' and s['fresh'] is True and all(type(v) in (int,float) and math.isfinite(v) for v in (gpu,mem,power)) and type(util) is int and 0<gpu<g and 0<mem<m and 0<=power<=w and 0<=util<=20
        except (KeyError,TypeError):return False

    @staticmethod
    def preflight_rejection(s,rm,thermal=False):
        # Specific, safe-to-display rejection codes for the original Mac GUI.
        # This function never authorizes manual control; idle_ok is authoritative.
        import math
        try:
            if rm.get('mode')!='auto':return 'preflight_requires_AUTO'
            if s.get('fresh') is not True:return 'preflight_telemetry_not_fresh'
            for key in ('gpu_c','mem_c','power_w'):
                x=s.get(key)
                if type(x) not in (float,int) or not math.isfinite(x):
                    return 'preflight_'+key+'_invalid'
            gpu,mem,power=s['gpu_c'],s['mem_c'],s['power_w']
            g,m,w=(60,84,60) if thermal else (55,82,35)
            if not 0<gpu<g:return f'preflight_gpu_temperature_not_below_{g}C'
            if not 0<mem<m:return f'preflight_memory_temperature_not_below_{m}C'
            if not 0<=power<=w:return f'preflight_power_above_{w}W'
            util=s.get('util_gpu_pct')
            if type(util) is not int or not 0<=util<=20:
                return 'preflight_utilization_above_20pct_or_unavailable'
        except (TypeError,ValueError,AttributeError):pass
        return 'preflight_not_idle_or_auto'

    def _read_event(self,timeout=5):
        if not self.guard or self.guard.stdout is None:raise PhysicalError('guard_missing')
        fd=self.guard.stdout.fileno()
        deadline=time.monotonic()+timeout
        buf=b''
        while time.monotonic()<deadline and len(buf)<2048:
            if self.guard.poll() is not None and not select.select([fd],[],[],0)[0]:break
            r,_,_=select.select([fd],[],[],max(0.,deadline-time.monotonic()))
            if not r:break
            ch=os.read(fd,1)
            if not ch:break
            if ch==b'\n':
                try:
                    event=json.loads(buf)
                    if event.get('operation')=='guard':
                        self.last_guard_event=event
                        if event.get('event')=='safety_metrics':
                            self.last_guard_safety=event
                            self._audit('guard_safety_event',guard_event=event)
                        if 'set_status' in event and event.get('event') in ('manual','failure'):
                            self.last_rm_transaction=event
                        return event
                except (UnicodeDecodeError,ValueError):pass
                buf=b''
            else:buf+=ch
        raise PhysicalError('guard_no_response_or_terminated')

    def _send(self,line,expect):
        if expect:self.last_guard_event=None
        self._poll_child()
        if not self.guard or self.guard.poll() is not None or not self.guard.stdin:
            raise PhysicalError('guard_not_running')
        try:
            self.guard.stdin.write((line+'\n').encode('ascii'));self.guard.stdin.flush()
            if line=='PING':self.last_guard_pulse=time.monotonic()
        except (OSError,BrokenPipeError):raise PhysicalError('guard_pipe_closed') from None
        if expect:
            ev=self._read_event(4)
            if ev.get('event')=='lower_refused' and line.startswith('LOWER '):
                raise PhysicalError('lower_preflight_changed_no_write')
            if ev.get('event')!=expect:raise PhysicalError('guard_'+str(ev.get('event')))
            return ev
        return None

    def guard_progress(self,starting=False):
        """Bridge bounded read-only subprocess waits without renewing a GUI lease.
        START is the only token-less exception, after SET and rescue arming."""
        self.watchdog_tick()
        now=time.monotonic()
        if not starting and (not self.token or now-self.last_heartbeat>=6):
            return
        if starting and (self.guard is None or self.rescue is None or self.rescue.poll() is not None):
            raise PhysicalError('spinup_rescue_not_running')
        if now-self.last_guard_pulse>=1:
            self._send('PING',None)

    def _acquire_lease(self):
        flags=os.O_RDWR|os.O_CREAT|getattr(os,'O_NOFOLLOW',0)
        fd=os.open(self.lease_path,flags,0o600)
        try:
            st=os.fstat(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_uid!=0 or st.st_nlink!=1 or st.st_mode & 0o077:
                raise PhysicalError('unsafe_lease')
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        except Exception:
            os.close(fd);raise
        self.lease_fd=fd

    def rescue_command(self):
        # Onedir frozen executable: no extraction parent process, so existing
        # parent-PID and inherited FD checks in rescue.py remain applicable.
        if getattr(sys,'frozen',False):
            return [str(ROOT/'Runtime/nvmetrics-runtime'),'rescue']
        return ['/usr/bin/python3','-E','-s','-B',str(self.rescue_module)]

    def _arm_rescue(self):
        ack_read,ack_write=os.pipe()
        pulse_read,pulse_write=os.pipe()
        proc=None
        try:
            remaining=max(20.,(self.service_deadline-time.monotonic())+10.) if self.service_deadline else RESCUE_DELAY_SECONDS
            remaining=min(RESCUE_DELAY_SECONDS,remaining)
            proc=subprocess.Popen(self.rescue_command()+[
                                   '--write-fd',str(ack_write),'--lease-fd',str(self.lease_fd),
                                   '--pulse-fd',str(pulse_read),
                                   '--delay',str(remaining),'--parent-pid',str(os.getpid()),
                                   '--guard-pid',str(self.guard.pid)],stdin=subprocess.DEVNULL,
                                  stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                                  start_new_session=True,close_fds=True,pass_fds=(ack_write,pulse_read,self.lease_fd),
                                  env={'PATH':'/usr/bin:/bin','HOME':'/var/root','LANG':'C','PYTHONNOUSERSITE':'1'})
            self.rescue=proc
        except BaseException:
            os.close(ack_read);os.close(pulse_write)
            raise
        finally:
            os.close(ack_write);os.close(pulse_read)
        try:
            ready,_,_=select.select([ack_read],[],[],4.)
            data=os.read(ack_read,32) if ready else b''
            if data!=b'ARMED\n':raise PhysicalError('root_rescue_not_armed')
            os.set_blocking(pulse_write,False)
            self.watchdog_write_fd=pulse_write
            self.last_watchdog_sent=0.
            self.watchdog_tick()
            self._audit('rescue_armed',rescue_pid=proc.pid,deadline_s=round(remaining,1))
        except BaseException:
            os.close(pulse_write)
            raise
        finally:os.close(ack_read)

    def _load_spinup_probe(self,probe):
        deadline=time.monotonic()+4.0
        last_reason='missing_rpm'
        while time.monotonic()<deadline:
            self.watchdog_tick()
            if self.guard is None or self.guard.poll() is not None:
                raise PhysicalError('guard_exited_during_spinup')
            try:
                self._send('PING',None)
                s=probe()
                rpm=s.get('rpm')
                if (self.idle_ok(s,{'mode':'auto'},self.thermal_profile) and type(rpm) is list and len(rpm)==2 and
                    all(type(n) is int and 950<=n<=5500 for n in rpm[:2]) and
                    self.guard.poll() is None):
                    self._send('PING',None)
                    self._audit('load_profile_spinup_verified',rpm=rpm[:2],gpu_c=s['gpu_c'],mem_c=s['mem_c'])
                    return
                last_reason='tachometers_or_idle_preflight_not_verified'
            except (KeyError,TypeError,ValueError,OSError):
                last_reason='sensor_probe_failed'
            time.sleep(0.45)
        raise PhysicalError('load_profile_spinup_failed_'+last_reason)

    def begin(self,uid,status,inspect,probe=None):
        if not self.enabled:raise PhysicalError('physical_mode_disabled')
        self._poll_child()
        if self.used or self.token:
            if self.last_end=='user_auto_verified' or self._recoverable_tach() or self._recoverable_thermal() or self._recoverable_heartbeat():raise PhysicalError('previous_rescue_still_verifying_AUTO')
            raise PhysicalError('previous_session_or_rescue_not_safe_to_rearm')
        if not self.idle_ok(status,inspect,self.thermal_profile):
            reason=self.preflight_rejection(status,inspect,self.thermal_profile)
            self._audit('manual_begin_refused',reason=reason,
                        gpu_c=status.get('gpu_c'),mem_c=status.get('mem_c'),
                        power_w=status.get('power_w'),util_pct=status.get('util_gpu_pct'))
            raise PhysicalError(reason)
        self.used=True  # deny retries even after failures or expired session
        self.last_trip=None
        self.last_trip_percent=None
        self.last_rm_transaction=None
        self.last_guard_safety=None
        self.last_guard_event=None
        self.run_id=secrets.token_hex(8)
        self._audit('manual_begin_requested',gpu_c=status.get('gpu_c'),mem_c=status.get('mem_c'),
                    safety_profile=self.safety_profile)
        try:
            self._acquire_lease()
            # C guard does read-only preflight/warm-up first. Arm the independent
            # rescue AFTER its ready event, BEFORE the first physical SET.
            self.guard_log=open(ROOT/'guard-last.stderr.log','ab',buffering=0)
            self.guard=subprocess.Popen([str(self.agent),'fan','rm-guard'],
                                        stdin=subprocess.PIPE,stdout=subprocess.PIPE,
                                        stderr=self.guard_log,bufsize=0,
                                        env={'PATH':'/usr/bin:/bin','HOME':'/var/root','LANG':'C',
                                             'NVMETRICS_FAN_GUARD':'ACEPTO_GUARD_V04',
                                             **({'NVMETRICS_RM_THERMAL_SUPERVISOR':'USER_AUTHORIZED_V0141'} if self.thermal_profile else {}),
                                             **({'NVMETRICS_RM_LOAD_TEST':'USER_AUTHORIZED_V10'} if self.load_profile else {})})
            ev=self._read_event(6)
            if ev.get('event')!='ready':raise PhysicalError('guard_not_ready')
            self._arm_rescue()  # ACK mandatory before START 50; rescue also checks server pulse
            ev=self._send('START 50','manual')
            if ev.get('pct')!=50:raise PhysicalError('guard_percent_mismatch')
            # A successful private RM readback is NOT enough to authorize load:
            # make sure both tachometers show real spinning fans at 50%.
            # Keep the rescue alive and bound total spin-up check to ~4s.
            if self.load_profile or self.thermal_profile:
                if not callable(probe):raise PhysicalError('load_profile_requires_tachometer_probe')
                self._load_spinup_probe(probe)
            self.token=secrets.token_hex(16);self.pct=50
            self.start_time=time.monotonic();self.last_heartbeat=self.start_time
            self.last_change_time=self.start_time
            self._audit('manual_started',percent=50,rm_verified=True,rm_transaction=self.last_rm_transaction)
            return {'session':self.token,**self.status()}
        except Exception:
            self.failure=True;self.last_end='begin_failed_rescue_pending'
            self._audit('begin_failed_restore_pending',guard_event=self.last_guard_event)
            self._close_guard()
            raise

    def _validate(self,uid,token):
        self._poll_child()
        if not self.token or not secrets.compare_digest(self.token,token):
            raise PhysicalError('invalid_or_expired_session')
        if time.monotonic()-self.start_time>=MAX_MANUAL_SECONDS:
            self.stop('hard_deadline');raise PhysicalError('session_expired')

    def heartbeat(self,uid,token):
        self._validate(uid,token)
        # Child does not emit a PING event; it controls the hard timeout itself.
        try:self._send('PING',None)
        except PhysicalError:
            self.stop('heartbeat_failed');raise
        self.last_heartbeat=time.monotonic()
        self.watchdog_tick()
        return self.status()

    def raise_percent(self,uid,token,pct):
        self._validate(uid,token)
        if type(pct) is not int or pct<=self.pct or pct not in range(35,101,5):
            raise PhysicalError('only_raise_in_5_percent_steps')
        try:
            self.heartbeat(uid,token)
            ev=self._send('RAISE '+str(pct),'manual')
            if ev.get('pct')!=pct:raise PhysicalError('guard_percent_mismatch')
            self._send('PING',None)
            self.last_heartbeat=time.monotonic()
        except PhysicalError as exc:
            self._audit('raise_failed',requested=pct,reason=str(exc),
                        guard_reason=(self.last_guard_event or {}).get('reason'),guard_event=self.last_guard_event)
            if self.token is not None:
                self.failure=True;self.stop('raise_failed_rescue_pending')
            raise
        previous=self.pct
        self.pct=pct
        self.last_change_time=time.monotonic()
        self._audit('manual_raised',previous=previous,percent=pct,rm_verified=True,rm_transaction=self.last_rm_transaction)
        self.watchdog_tick()
        return self.status()

    def lower_percent(self,uid,token,pct,probe=None,inspect=None):
        """Guarded lower step: 50→45→...→30, with live tach verification.

        A successful RM GET is not proof that a fan motor keeps spinning.
        Only a bounded electrical/utilization excursion can be retried; a
        missing or implausible tachometer, stale sensor or thermal fault
        still fails closed. An unsuccessful soft transition is not committed.
        """
        self._validate(uid,token)
        if self.load_profile:
            raise PhysicalError('lower_not_permitted_in_load_profile')
        if not (type(pct) is int and MANUAL_FLOOR <= pct <= (95 if self.thermal_profile else 45) and pct % 5 == 0 and pct == self.pct - 5):
            raise PhysicalError('lower_only_one_5_percent_step_from_50')
        if not callable(probe):raise PhysicalError('lower_needs_live_tach_probe')
        if inspect is None or inspect.get('mode')!='manual':
            raise PhysicalError('lower_requires_verified_manual_rm')
        # The C guard repeats idle checks before SET. The server does not
        # make a hardware write without a fresh read-only preflight.
        self.heartbeat(uid,token)
        try:initial=probe()
        except (ProtocolError,OSError,ValueError,TypeError):
            self.failure=True;self.stop('lower_sensor_failed')
            raise PhysicalError('lower_sensor_failed_AUTO_requested') from None
        active_thermal=self.thermal_profile and pct>=50
        # A desktop compositing burst can cross 50 W / 20% for one reading.
        # Wait for a fresh, strictly eligible sample; never SET on the grace
        # envelope. The native C guard repeats the strict preflight itself.
        if self.thermal_profile and pct<50:
            for _ in range(2):
                if self._low_idle_ok(initial,self.pct,False,True) or not self._soft_lower_excursion(initial,self.pct,True):break
                self._audit('manual_lower_preflight_retry',requested=pct,percent=self.pct,sample=initial)
                time.sleep(.45)
                self.heartbeat(uid,token)
                initial=probe()
        if not self._low_idle_ok(initial,self.pct,active_thermal,self.thermal_profile):
            self._audit('manual_lower_refused',requested=pct,percent=self.pct,reason='lower_preflight',sample=initial)
            if self.thermal_profile and pct<50 and self._low_idle_ok(initial,self.pct,True,True):
                raise PhysicalError('lower_thermal_margin_insufficient_no_write',sample=initial,requested=pct,applied=self.pct)
            if self.thermal_profile:
                # The existing active step was NOT shown to be safe either.
                # Do not leave an unverified physical session running.
                self.failure=True
                self.stop('low_calibration_failed')
                self._poll_child()
                raise PhysicalError('low_calibration_failed_AUTO_requested')
            raise PhysicalError('lower_requires_idle_and_two_spinning_fans')
        previous=self.pct
        sample={}
        try:
            ev=self._send('LOWER '+str(pct),'manual')
            if ev.get('pct')!=pct:raise PhysicalError('guard_percent_mismatch')
            self.pct=pct
            self.last_change_time=time.monotonic()
            # Bounded tach stabilization after SET (not just RM readback).
            # Keep rescue pulses alive through the validation delay.
            samples=[]
            # At most five probes, still require three CONSECUTIVE strictly
            # eligible readings. No grace for bad tachometers or temperatures.
            for _ in range(5 if self.thermal_profile and pct<50 else 3):
                time.sleep(0.45)
                self.watchdog_tick()
                self._send('PING',None)
                self.last_heartbeat=time.monotonic()
                sample=probe()
                rpm=sample.get('rpm')
                if self._low_idle_ok(sample,pct,active_thermal,self.thermal_profile):
                    samples.append(rpm[:2])
                    if len(samples)==3:break
                elif self.thermal_profile and pct<50 and self._soft_lower_excursion(sample,pct,True):
                    samples.clear()
                    self._audit('manual_lower_postwrite_retry',requested=pct,previous=previous,sample=sample)
                elif self.thermal_profile and pct<50 and self._soft_lower_excursion(sample,pct,False):
                    samples.clear();break
                else:raise PhysicalError('low_calibration_tach_or_idle_failed')
            if len(samples)!=3:
                if self.thermal_profile and pct<50 and self._low_idle_ok(sample,pct,True,True) and sample['gpu_c']<58 and sample['mem_c']<82:
                    self._recover_lower(uid,token,previous,pct,probe)
                    raise PhysicalError('lower_recovered_verified_higher',sample=sample,requested=pct,applied=self.pct)
                raise PhysicalError('low_calibration_tach_or_idle_failed')
            # The final successful probe can itself consume several seconds.
            # Refresh the authenticated operation lease before returning.
            self.heartbeat(uid,token)
            if not self.token:raise PhysicalError('guard_exited_after_lower_verification')
            self._audit('manual_lower_verified',previous=previous,percent=pct,
                        rpm_samples=samples,rpm_floor=rpm_floor(pct),rm_verified=True,rm_transaction=self.last_rm_transaction)
            return self.status()
        except (PhysicalError,OSError,ValueError,KeyError,TypeError) as exc:
            if str(exc)=='lower_recovered_verified_higher':raise
            if str(exc)=='lower_preflight_changed_no_write' and self.pct==previous:
                self._audit('manual_lower_refused',requested=pct,percent=previous,reason=str(exc))
                # The native guard refused before the SET. Preserve an active
                # manual step only after rechecking its actual safety envelope.
                if not self.thermal_profile:raise
                try:
                    self.heartbeat(uid,token)
                    sample=probe()
                    safe=self._low_idle_ok(sample,previous,True,True)
                except (PhysicalError,OSError,ValueError,KeyError,TypeError):safe=False
                if safe:raise PhysicalError('lower_thermal_margin_insufficient_no_write',sample=sample,requested=pct,applied=self.pct) from None
            self._audit('manual_lower_failed',previous=previous,requested=pct,reason=str(exc),
                        guard_event=self.last_guard_event,
                        preflight={k:initial.get(k) for k in ('rpm','fresh','gpu_c','mem_c','power_w','util_gpu_pct')},
                        sample={k:sample.get(k) for k in ('rpm','fresh','gpu_c','mem_c','power_w','util_gpu_pct')})
            self.failure=True
            self.stop('low_calibration_failed')
            self._audit('manual_lower_guard_exit',requested=pct,exit_code=self.guard.poll() if self.guard else None)
            self._poll_child()
            raise PhysicalError('low_calibration_failed_AUTO_requested') from None

    @classmethod
    def _soft_lower_excursion(cls,s,pct,bounded):
        # Strictly valid physical readings and low temperature; only power
        # and/or utilization may be outside the low-speed entry envelope.
        if not cls._low_idle_ok(s,pct,True,True):return False
        if not (s['gpu_c']<58 and s['mem_c']<82):return False
        if s['power_w']<=50 and s['util_gpu_pct']<=20:return False
        return not bounded or (s['power_w']<=60 and s['util_gpu_pct']<=30)

    def _recover_lower(self,uid,token,previous,attempted,probe):
        # Do not retry a downward write. Restore at least 50% using the same
        # native verified RAISE command; require independent physical tach
        # samples before exposing the recovered percentage as a usable state.
        recovery=max(previous,50)
        self._audit('manual_lower_recovery_requested',attempted=attempted,previous=previous,target=recovery)
        self.raise_percent(uid,token,recovery)
        samples=[]
        for _ in range(3):
            time.sleep(.45)
            self.heartbeat(uid,token)
            current=probe()
            if not self._low_idle_ok(current,recovery,True,True):
                raise PhysicalError('lower_recovery_tach_or_health_failed')
            samples.append(current['rpm'][:2])
        self.heartbeat(uid,token)
        if self.token!=token or self.pct!=recovery:raise PhysicalError('lower_recovery_session_lost')
        self._audit('manual_lower_recovered',attempted=attempted,percent=recovery,rpm_samples=samples,rm_verified=True,rm_transaction=self.last_rm_transaction)

    @staticmethod
    def _low_idle_ok(s,pct=30,active_thermal=False,thermal=False):
        import math
        if pct<MANUAL_FLOOR:return False
        try:
            rpm=s['rpm']
            return (s['fresh'] is True and isinstance(rpm,list) and len(rpm)==2 and
                all(type(n) is int and rpm_floor(pct)<=n<=5500 for n in rpm[:2]) and
                all(type(s.get(k)) in (int,float) and math.isfinite(s[k])
                    for k in ('gpu_c','mem_c','power_w')) and
                0<s['gpu_c']<(65 if active_thermal else 58 if thermal else 55) and 0<s['mem_c']<(88 if active_thermal else 82) and 0<=s['power_w']<=(145 if active_thermal else 50 if thermal else 35) and
                type(s.get('util_gpu_pct')) is int and 0<=s['util_gpu_pct']<=(100 if active_thermal else 20))
        except (KeyError,TypeError,ValueError):return False

    def check_client_lease(self,deadline_seconds=8.0):
        """Fail closed if a GUI exits/locks up while the root server is alive.

        The existing C guard still has a shorter independent 6s PING limit;
        this lease makes the parent-side intent explicit and auditable.
        """
        self._poll_child()
        if self.token and self.last_heartbeat and time.monotonic()-self.last_heartbeat>deadline_seconds:
            self.failure=True
            self._audit('client_lease_expired',age_s=round(time.monotonic()-self.last_heartbeat,2))
            self.stop('client_lease_expired')

    def auto(self,uid,token):
        self._validate(uid,token)
        try:ev=self._send('AUTO','auto_verified')
        except PhysicalError:
            if self.token is not None:
                self.failure=True
                self.stop('auto_failed_rescue_pending')
            raise
        # C verified both RM records; still best effort for hardware.
        self._audit('user_auto_verified',previous=self.pct,rm_verified=True)
        self.token=None;self.pct=0;self.last_end='user_auto_verified'
        self._close_guard()
        self._poll_child()  # may rearm now if the independent rescue has finished
        return {'mode':'auto','readback_verified':True,'physical_enabled':self.enabled,'last_end_reason':self.last_end}

    def _close_guard(self):
        if self.guard:
            try:
                if self.guard.stdin: self.guard.stdin.close()  # C restores on EOF
            except OSError:pass
            try:self.guard.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.guard.terminate()  # C signal handler should restore
                try:self.guard.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    # Do not SIGKILL: leave root rescue armed for recovery.
                    pass
        if self.guard_log:
            self.guard_log.close();self.guard_log=None

    def stop(self,reason):
        if self.token or (self.guard and self.guard.poll() is None):
            self._audit('session_stopped',reason=reason,percent=self.pct)
            previous=self.pct
            self.token=None;self.pct=0;self.last_end=reason
            if self.failure and reason.startswith('load_health_trip_'):
                self.last_trip=reason
                self.last_trip_percent=previous
                self.last_end=reason+'_rescue_pending'
                if self.thermal_profile and reason=='load_health_trip_thermal_limit':
                    self.last_end='server_thermal_trip_rescue_pending'
                    self.thermal_rearm_after=time.monotonic()+10
            elif self.failure and reason=='low_calibration_failed':
                self.last_end=reason+'_rescue_pending'
            elif self.failure and not self.last_end.endswith('_rescue_pending'):
                self.last_end=reason+'_rescue_pending'
            self._close_guard()

    def close(self):
        self.stop('server_shutdown')
        self._close_watchdog()
        if self.lease_fd is not None:
            # Detached rescue continues to own its lock until RM AUTO verification.
            os.close(self.lease_fd);self.lease_fd=None
