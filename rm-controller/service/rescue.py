#!/usr/bin/env python3
"""Detached root recovery guardian for a bounded (<8h) local IPC service.

A kernel-level recovery guarantee is impossible here. This process is deliberately
independent of both the SwiftUI application and the root IPC server: its
inherited pipe stops receiving authenticated bytes when the server blocks or
dies. It re-reads RM and sends AUTO only when AUTO is not already reported.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import select
import stat
import subprocess
import sys
import time

ROOT=Path('/Library/PrivilegedHelperTools/org.nvmetrics.rmguard')
MAX_SILENCE=10.0

def run(agent,cmd):
    try:
        p=subprocess.run([str(agent),'fan',cmd],stdin=subprocess.DEVNULL,capture_output=True,
                         text=True,timeout=8,env={'PATH':'/usr/bin:/bin','HOME':'/var/root','LANG':'C'})
        return p.returncode,p.stdout[-30000:],p.stderr[-1000:]
    except (OSError,subprocess.TimeoutExpired) as e:return -1,'',str(e)

def is_auto(text):
    from protocol import parse_agent
    try:return parse_agent(text,'inspect')['mode']=='auto'
    except Exception:return False

def guard_exists(pid):
    try:os.kill(pid,0);return True
    except ProcessLookupError:return False
    except PermissionError:return True

def supervise(pulse_fd,parent_pid,guard_pid,deadline,clock=time.monotonic,timeout=MAX_SILENCE):
    """Return a reason. Reads bytes only from the server's inherited FD.

    A *live* Unix socket client cannot reset this timer. Failed or blocked root
    service calls do not get an unlimited period to hold MANUAL.
    """
    last=clock()
    while clock()<deadline:
        if os.getppid()!=parent_pid:return 'parent_disappeared'
        if not guard_exists(guard_pid):return 'guard_exited'
        remaining=min(0.5,max(0.,deadline-clock()))
        ready,_,_=select.select([pulse_fd],[],[],remaining)
        if ready:
            data=os.read(pulse_fd,4096)
            if not data:return 'server_pipe_closed'
            last=clock()
        if clock()-last>=timeout:return 'server_pulse_timeout'
    return 'hard_rescue_deadline'

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--write-fd',type=int,required=True)
    ap.add_argument('--pulse-fd',type=int,required=True)
    ap.add_argument('--lease-fd',type=int,required=True)
    ap.add_argument('--delay',type=float,required=True)
    ap.add_argument('--parent-pid',type=int,required=True)
    ap.add_argument('--guard-pid',type=int,required=True)
    args=ap.parse_args()
    if (os.geteuid()!=0 or not 10<=args.delay<=28830 or args.parent_pid<2 or
        args.guard_pid<2 or min(args.write_fd,args.pulse_fd,args.lease_fd)<3):
        raise SystemExit('invalid_privileged_rescue')
    if os.getppid()!=args.parent_pid:raise SystemExit('parent_pid_mismatch')
    st=os.fstat(args.lease_fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid!=0 or st.st_mode&0o077:
        raise SystemExit('untrusted_inherited_lease')
    agent=ROOT/'nvmetrics-agent'; log=ROOT/'rescue-v09.jsonl'
    if not agent.is_file():raise SystemExit('agent_missing')
    flags=os.O_CREAT|os.O_APPEND|os.O_WRONLY|getattr(os,'O_NOFOLLOW',0)
    fd=os.open(log,flags,0o600)
    try:
        st=os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_uid!=0 or st.st_nlink!=1 or st.st_mode&0o077:
            raise RuntimeError('untrusted_rescue_log')
        if st.st_size>=4*1024*1024:
            os.replace(log,log.with_suffix('.jsonl.1'))
            os.close(fd);fd=-1
            fd=os.open(log,flags|os.O_EXCL,0o600)
        def event(**fields):
            os.write(fd,(json.dumps({'t':time.time(),**fields},separators=(',',':'))+'\n').encode('utf8'))
        event(event='armed',pid=os.getpid(),delay_s=args.delay,watchdog_s=MAX_SILENCE)
        os.write(args.write_fd,b'ARMED\n');os.close(args.write_fd)
        reason=supervise(args.pulse_fd,args.parent_pid,args.guard_pid,time.monotonic()+args.delay)
        event(event='wake',reason=reason,guard_pid=args.guard_pid)
        os.close(args.pulse_fd)
        # The C guard might itself have just received EOF and be returning to AUTO.
        # A brief read-only grace avoids a redundant competing privileged SET.
        code,out,err=run(agent,'rm-inspect')
        if code!=0 or not is_auto(out):
            if reason in ('guard_exited','server_pipe_closed','parent_disappeared'):
                time.sleep(0.3)
                code,out,err=run(agent,'rm-inspect')
            if code!=0 or not is_auto(out):
                event(event='restore_required',rm_read_rc=code)
                code,out,err=run(agent,'rm-restore-experiment')
                event(event='auto_set',rc=code,stderr=err)
        code,out,err=run(agent,'rm-inspect')
        verified=code==0 and is_auto(out)
        event(event='complete',verified_auto=verified,inspect_rc=code,reason=reason,stderr=err)
        return 0 if verified else 4
    finally:
        if fd>=0:os.close(fd)
        os.close(args.lease_fd)

if __name__=='__main__':
    try:raise SystemExit(main())
    except BaseException as exc:
        if isinstance(exc,SystemExit):raise
        print('rescue_exception',repr(exc),file=sys.stderr)
        raise SystemExit(5)
