#!/usr/bin/env python3
"""Experimental root IPC server with sequential, guarded physical fan sessions.
Root must explicitly arm this foreground-only server. This is not a production helper.
"""
from __future__ import annotations
import argparse
import contextlib
import fcntl
import math
import os
from pathlib import Path
import signal
import socket
import stat
import time
from backend import ReadOnlyAgent
from peer import peer_uid
from physical import Physical,PhysicalError,MAX_MANUAL_SECONDS
from protocol import ProtocolError,MAX_REQUEST_BYTES,decode_request,response,rpm_floor
from launchd_socket import activate_socket,validate_listener

ROOT=Path('/Library/PrivilegedHelperTools/org.nvmetrics.rmguard')
FILES=('server.py','backend.py','peer.py','physical.py','rescue.py','protocol.py','launchd_socket.py','nvmetrics-agent')

def verified_root_path(path:Path,executable=False):
    if not path.is_absolute():raise RuntimeError('path_not_absolute')
    cur=Path('/')
    for i,segment in enumerate(path.parts[1:]):
        cur/=segment;st=cur.lstat();last=i==len(path.parts[1:])-1
        if st.st_uid!=0 or st.st_mode&0o022 or stat.S_ISLNK(st.st_mode):
            raise RuntimeError(f'untrusted_root_path: {cur}')
        if last:
            if not stat.S_ISREG(st.st_mode):raise RuntimeError('root_file_not_regular')
            if executable and not st.st_mode&0o100:raise RuntimeError('agent_not_executable')
        elif not stat.S_ISDIR(st.st_mode):raise RuntimeError('ancestor_not_directory')

def read_frame(conn):
    buf=b''
    while len(buf)<=MAX_REQUEST_BYTES and b'\n' not in buf:
        c=conn.recv(min(256,MAX_REQUEST_BYTES+1-len(buf)))
        if not c:break
        buf+=c
    return buf

class IPCServer:
    def __init__(self,path,allowed_uid,backend,physical,peer_identity=peer_uid):
        self.path=Path(path);self.allowed_uid=allowed_uid;self.backend=backend
        self.physical=physical;self.peer_identity=peer_identity;self.stop=False
        self.service_deadline=None
        self.next_load_health_check=0.
        self.last_good_tach=None
        self.last_tach_retry=None
    def dispatch(self,payload,caller_uid):
        if caller_uid!=self.allowed_uid:return {'ok':False,'error':'unauthorized_peer'}
        op=None
        try:
            req=decode_request(payload);op=req['op']
            if op=='ping':
                return {'ok':True,'service':'NVMetrics RM v0.14.8','writes_enabled':self.physical.enabled,
                        'authorized_uid':self.allowed_uid,'sequential_sessions':True,'physical_one_shot':False,
                        'revision':'0.14.8','manual_floor':50 if self.physical.load_profile else 30,
                        'safety_profile':self.physical.safety_profile}
            if op in ('status','inspect'):
                return {'ok':True,'operation':op,'data':self.backend.call(op,progress=self.physical.guard_progress) if self.physical.token else self.backend.call(op)}
            if op=='auto_observe':data=self.auto_observe()
            elif op=='control_status':data=self.physical.status()
            elif op=='manual_begin':
                if self.service_deadline is not None and time.monotonic()+60>=self.service_deadline:
                    raise PhysicalError('server_lifetime_less_than_60s')
                data=self.physical.begin(caller_uid,self.backend.call('status'),self.backend.call('inspect'),
                                         probe=(lambda:self.backend.call('status',timeout=3.5,progress=lambda:self.physical.guard_progress(starting=True))))
            elif op=='manual_raise':data=self.physical.raise_percent(caller_uid,req['session'],req['pct'])
            elif op=='manual_lower':
                self.physical.heartbeat(caller_uid,req['session'])
                data=self.physical.lower_percent(caller_uid,req['session'],req['pct'],
                    probe=(lambda:self.backend.call('status',timeout=3.5,progress=self.physical.guard_progress)),
                    inspect=self.backend.call('inspect',timeout=3.5,progress=self.physical.guard_progress))
            elif op=='manual_heartbeat':data=self.physical.heartbeat(caller_uid,req['session'])
            elif op=='manual_auto':data=self.physical.auto(caller_uid,req['session'])
            else:raise ProtocolError('operation_not_allowed')
            if op=='control_status' and self.service_deadline is not None:
                data['service_seconds_left']=round(max(0.,self.service_deadline-time.monotonic()),1)
            return {'ok':True,'operation':op,'data':data}
        except (ProtocolError,PhysicalError) as exc:
            if isinstance(exc,ProtocolError) and self.physical.token is not None and op=='manual_lower':
                self.physical.failure=True
                self.physical.stop('lower_sensor_failed')
            return {'ok':False,'error':str(exc),**({'details':exc.details} if isinstance(exc,PhysicalError) and exc.details else {})}
    def auto_observe(self):
        # Never poll/modify a physical session from this operation. The server
        # serializes requests; do not queue sensor work while rescue owns RM.
        if self.physical.token is not None or self.physical.used:
            raise ProtocolError('observation_manual_or_rescue')
        before=self.backend.call('inspect',timeout=2.5)
        if before.get('mode')!='auto':raise ProtocolError('observation_requires_auto')
        started=time.time()
        sample=self.backend.call('status',timeout=3.5)
        after=self.backend.call('inspect',timeout=2.5)
        if after!=before or after.get('mode')!='auto':raise ProtocolError('observation_auto_changed')
        if self.physical.token is not None or self.physical.used:
            raise ProtocolError('observation_manual_or_rescue')
        if sample.get('fresh') is not True or any(type(sample.get(k)) not in (int,float) or not math.isfinite(sample[k]) or not 0<sample[k]<=150 for k in ('gpu_c','mem_c')):
            raise ProtocolError('observation_invalid_temperature')
        if type(sample.get('power_w')) not in (int,float) or not math.isfinite(sample['power_w']) or not 0<=sample['power_w']<=1000:
            raise ProtocolError('observation_invalid_power')
        if type(sample.get('util_gpu_pct')) is not int or not 0<=sample['util_gpu_pct']<=100:
            raise ProtocolError('observation_invalid_utilization')
        rpm=sample.get('rpm')
        if type(rpm) is not list or len(rpm) not in (0,2) or any(type(r) is not int or not 0<=r<=5500 for r in rpm):
            raise ProtocolError('observation_invalid_tachometers')
        return {**sample,'observed_at':started,'auto_bracketed':True}
    def health_reason(self,s):
        load=self.physical.load_profile
        thermal=self.physical.thermal_profile
        if self.physical.pct < (50 if load else 30) or self.physical.pct>100 or self.physical.pct%5:return 'manual_floor_violation'
        if s.get('fresh') is not True:return 'stale'
        if any(type(s.get(k)) not in (float,int) or not math.isfinite(s[k]) for k in ('gpu_c','mem_c','power_w')):return 'telemetry_invalid'
        if not 0<s['gpu_c']<(65 if load or thermal else 55) or not 0<s['mem_c']<(88 if load or thermal else 82):return 'thermal_limit' if thermal and s['gpu_c']>0 and s['mem_c']>0 else 'thermal_or_power'
        if not 0<=s['power_w']<=(145 if load or thermal else 35):return 'thermal_or_power'
        if not load and (type(s.get('util_gpu_pct')) is not int or not 0<=s['util_gpu_pct']<=(100 if thermal else 20)):return 'utilization_invalid' if thermal else 'idle_utilization_limit'
        rpm=s.get('rpm')
        if rpm==[]:return 'tachometer_missing'
        if not isinstance(rpm,list) or len(rpm)!=2 or any(type(n) is not int or n<0 or n>5500 for n in rpm):return 'tachometer_invalid'
        if any(n<(950 if load else rpm_floor(self.physical.pct)) for n in rpm[:2]):return 'tachometer_below_min'
        return None

    @staticmethod
    def health_fields(s):
        return {'rpm':s.get('rpm'),'gpu_c':s.get('gpu_c'),'mem_c':s.get('mem_c'),
                'watts':s.get('power_w'),'util_pct':s.get('util_gpu_pct'),'fresh':s.get('fresh')}

    def load_healthcheck(self):
        """Independent server-side tach check every ~5s in attended load mode.
        GUI/monitor need not be running for this check.  The C guard continues
        thermal checks every 250ms and keeps the last valid RM percentage.
        """
        if self.physical.token is None:
            self.next_load_health_check=0.
            self.last_good_tach=None
            return
        now=time.monotonic()
        if now<self.next_load_health_check:return
        self.next_load_health_check=now+(5.0 if self.physical.load_profile else 2.0)
        s={};error=None
        key=(self.physical.run_id,self.physical.pct)
        try:
            s=self.backend.call('status',timeout=3.5,progress=self.physical.guard_progress)
            reason=self.health_reason(s)
            now=time.monotonic()
            # ONLY an empty RPM vector in idle, after a fresh good sample at
            # the SAME setting. Explicit low/zero RPM, bad thermals or stale
            # telemetry never receive grace. One retry per session per 30s.
            previous=self.last_good_tach
            if (reason=='tachometer_missing' and not self.physical.load_profile and key[1]>=30 and
                previous is not None and previous[0]==key and now-previous[1]<=3.5 and
                (self.last_tach_retry is None or self.last_tach_retry[0]!=key[0] or now-self.last_tach_retry[1]>=30)):
                self.last_tach_retry=(key[0],now)
                self.physical._audit('tachometer_retry',percent=key[1],**self.health_fields(s))
                s=self.backend.call('status',timeout=.75,progress=self.physical.guard_progress)
                reason=self.health_reason(s)
                if reason is None:
                    self.physical._audit('tachometer_recovered',percent=key[1],**self.health_fields(s))
            if reason is None:
                self.last_good_tach=(key,time.monotonic())
                self.physical._audit('load_health_sample',percent=key[1],**self.health_fields(s))
                return
        except (ProtocolError,ValueError,OSError,TypeError) as exc:
            reason='read_only_sensor_error';error=str(exc)[:120]
        self.physical._audit('load_health_trip',reason=reason,percent=key[1],error=error,**self.health_fields(s))
        self.physical.failure=True
        self.physical.stop('load_health_trip_'+reason)

    def handle(self,conn):
        conn.settimeout(14)
        try:
            uid=self.peer_identity(conn)
            data=self.dispatch(read_frame(conn),uid) if uid==self.allowed_uid else {'ok':False,'error':'unauthorized_peer'}
        except (OSError,TimeoutError):data={'ok':False,'error':'transport_error'}
        try:conn.sendall(response(data))
        except (OSError,TimeoutError):pass
    def serve(self,seconds,socket_uid=None,launchd_listener=None):
        lock=self.path.with_suffix('.lock')
        fd=os.open(lock,os.O_RDWR|os.O_CREAT|getattr(os,'O_NOFOLLOW',0),0o600)
        try:
            st=os.fstat(fd)
            if not stat.S_ISREG(st.st_mode) or st.st_uid!=os.geteuid() or st.st_nlink!=1 or st.st_mode&0o077:
                raise RuntimeError('unsafe_service_lock')
            fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            if self.path.parent.stat().st_mode&0o022:raise RuntimeError('unsafe_socket_parent')
            if launchd_listener is None:
                if os.path.lexists(self.path):
                    st=self.path.lstat()
                    if not stat.S_ISSOCK(st.st_mode) or st.st_uid not in (os.geteuid(),self.allowed_uid):
                        raise RuntimeError('unsafe_existing_socket')
                    self.path.unlink()
                listener=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
                try:
                    listener.bind(str(self.path))
                    if socket_uid is not None:os.chown(self.path,socket_uid,-1)
                    os.chmod(self.path,0o600)
                    listener.listen(4)
                except BaseException:
                    listener.close();raise
            else:
                listener=launchd_listener
            with listener as s:
                original=self.path.lstat()
                expected_uid=os.geteuid() if launchd_listener is not None or socket_uid is None else socket_uid
                if not stat.S_ISSOCK(original.st_mode) or original.st_uid!=expected_uid:
                    raise RuntimeError('unsafe_socket_owner')
                pending=validate_listener(s,self.path) if launchd_listener is not None else None
                s.settimeout(.2)
                print(f'IPC READY socket={self.path} uid={self.allowed_uid} '+
                      f'lifetime={seconds}s physical={"ARMED" if self.physical.enabled else "DISABLED"} safety_profile={self.physical.safety_profile} sequential-sessions',flush=True)
                last_client_activity=time.monotonic()
                limit=time.monotonic()+seconds
                self.service_deadline=limit
                if hasattr(self.physical,'service_deadline'):self.physical.service_deadline=limit
                try:
                    while not self.stop and time.monotonic()<limit:
                        self.physical._poll_child()
                        if hasattr(self.physical,'check_client_lease'):self.physical.check_client_lease()
                        if hasattr(self.physical,'watchdog_tick'):self.physical.watchdog_tick()
                        if self.physical.token is None:self.backend.close()
                        self.load_healthcheck()
                        # On-demand daemon returns idle resources to launchd;
                        # never exit while any guard/rescue or session is pending.
                        if (launchd_listener is not None and time.monotonic()-last_client_activity>45 and
                            not self.physical.used and not self.physical.token and
                            self.physical.rescue is None):
                            break
                        if pending is not None:
                            c,pending=pending,None
                        else:
                            try:c,_=s.accept()
                            except socket.timeout:continue
                        last_client_activity=time.monotonic()
                        with c:self.handle(c)
                finally:
                    if pending is not None:pending.close()
                    try:self.physical.close()
                    finally:self.backend.close()
                    try:
                        current=self.path.lstat()
                        if launchd_listener is None and (current.st_dev,current.st_ino)==(original.st_dev,original.st_ino):self.path.unlink()
                    except FileNotFoundError:pass
        finally:os.close(fd)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--uid',type=int,required=True)
    ap.add_argument('--launchd-socket',action='store_true')
    ap.add_argument('--lifetime',type=int,default=3600)
    ap.add_argument('--enable-physical',action='store_true',help='Allow sequential 50%-100% sessions with independently verified AUTO between sessions')
    profiles=ap.add_mutually_exclusive_group()
    profiles.add_argument('--load-test',action='store_true',help='Explicitly allow GPU workloads only after 50% tachometers are confirmed')
    profiles.add_argument('--thermal-supervised',action='store_true',help='Thermal supervised profile with 30% minimum')
    ap.add_argument('--acknowledge',default='',help='Must be RM_V08_EXPERIMENT for physical mode')
    args=ap.parse_args()
    if os.geteuid()!=0 or args.uid<1 or not 90<=args.lifetime<=28800:
        ap.error('root, non-root client UID and 90..28800s lifetime required')
    if (args.load_test or args.thermal_supervised) and not args.enable_physical:
        ap.error('--load-test requires --enable-physical')
    expected_ack='RM_V10_LOAD_TEST' if args.load_test else ('RM_V08_EXPERIMENT' if args.enable_physical else '')
    if args.acknowledge!=expected_ack:
        ap.error('explicit acknowledgment required: '+(expected_ack or 'none'))
    for name in FILES:verified_root_path(ROOT/name,executable=name=='nvmetrics-agent')
    os.umask(0o077)
    physical=Physical(ROOT/'nvmetrics-agent',enabled=args.enable_physical,load_profile=args.load_test,thermal_profile=args.thermal_supervised)
    srv=IPCServer(ROOT/'control.sock',args.uid,ReadOnlyAgent(ROOT/'nvmetrics-agent'),physical)
    def stop(sig,frame):srv.stop=True
    signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
    listener=activate_socket('Listener') if args.launchd_socket else None
    srv.serve(args.lifetime,socket_uid=args.uid,launchd_listener=listener)
    print('IPC STOPPED. RM AUTO requested if a physical session was active.',flush=True)

if __name__=='__main__':raise SystemExit(main())
