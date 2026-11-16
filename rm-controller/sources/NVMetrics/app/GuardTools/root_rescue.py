#!/usr/bin/env python3
"""NVMetrics temporary root backstop: authenticate BEFORE detaching.

Only invoked by the attended experiment. Never installs services or persists
outside its one delayed execution. 'inspect' is read-only; 'restore' writes RM.
"""
from __future__ import annotations
import json
import fcntl
import errno
import re
import os
import select
import stat
import subprocess
import sys
import time
from pathlib import Path

LEASE_PATH = "/var/run/nvmetrics-rm-v05-session.lock"

def acquire_session_lease(path: str = LEASE_PATH) -> int:
    """Root-owned, single-in-flight lease. Held by the detached rescue until completion.

    A second v0.5 trial cannot arm while an old rescue may still fire.
    """
    flags = os.O_RDWR | os.O_CREAT
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    fd = os.open(path, flags, 0o600)
    st = os.fstat(fd)
    if not stat.S_ISREG(st.st_mode) or st.st_uid != 0 or st.st_nlink != 1 or st.st_mode & 0o077:
        os.close(fd)
        raise RuntimeError("unsafe root session lease permissions")
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError as exc:
        os.close(fd)
        if exc.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
            raise RuntimeError("another v0.5 session/rescue is still active") from exc
        raise
    return fd


def snapshot_auto(text: str) -> bool:
    for line in reversed(text.splitlines()):
        try:
            d = json.loads(line)
        except (ValueError, TypeError):
            continue
        if not isinstance(d, dict) or d.get('operation') != 'rm-snapshot':
            continue
        if d.get('get_regular_status') != 0 or d.get('get_default_status') != 0:
            return False
        for key in ('regular', 'default'):
            v = d.get(key)
            if not isinstance(v, dict) or v.get('layout_matches_two') is not True:
                return False
            records = v.get('records')
            if not isinstance(records, list) or len(records) != 2:
                return False
            for record in records:
                if not isinstance(record, dict) or (record.get('policy_word_candidate'), record.get('requested_q16_candidate')) != (0, 0):
                    return False
        return True
    return False


def run_agent(agent: str, cmd: str, timeout: int = 10) -> dict:
    try:
        p = subprocess.run([agent, 'fan', cmd], stdin=subprocess.DEVNULL,
                           capture_output=True, text=True, timeout=timeout)
        return {'command': cmd, 'exit': p.returncode,
                'stdout': p.stdout[-18000:], 'stderr': p.stderr[-3000:]}
    except (OSError, subprocess.TimeoutExpired) as e:
        return {'command': cmd, 'exit': -1, 'error': repr(e)}


SESSION = ""
def log_event(**items) -> None:
    print(json.dumps({'operation': 'root-rescue', 'session': SESSION, **items}, ensure_ascii=False), flush=True)


def daemon_child(wfd: int, agent: str, logpath: str, mode: str, delay: float, session: str) -> None:
    global SESSION
    SESSION = session
    try:
        lease_fd = acquire_session_lease()  # Root holds this FD until after AUTO and readback.
        os.setsid()  # Called in the child of fork(), AFTER sudo's root privilege check.
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
        if hasattr(os, 'O_NOFOLLOW'):
            flags |= os.O_NOFOLLOW
        fd = os.open(logpath, flags, 0o644)
        os.fchmod(fd, 0o644)  # User can read the diagnostic log; root owns it.
        st = os.fstat(fd)
        if not stat.S_ISREG(st.st_mode) or st.st_uid != 0 or st.st_nlink != 1:
            raise RuntimeError('Rescue log failed secure creation checks')
        devnull = os.open(os.devnull, os.O_RDONLY)
        os.dup2(devnull, 0)
        os.dup2(fd, 1)
        os.dup2(fd, 2)
        os.close(devnull)
        if fd > 2:
            os.close(fd)
        log_event(event='armed', mode=mode, pid=os.getpid(), delay_s=delay, lease='held')
        os.write(wfd, b'ARMED\n')
        os.close(wfd)
        time.sleep(delay)
        log_event(event='executing', mode=mode)
        # This root subprocess never invokes sudo again.
        if mode == 'restore':
            r = run_agent(agent, 'rm-restore-experiment')
            log_event(event='restore_result', **r)
        else:
            r = None
        g = run_agent(agent, 'rm-inspect')
        verified = g.get('exit') == 0 and snapshot_auto(g.get('stdout', ''))
        log_event(event='inspection_result', **g, verified_auto=verified)
        success = verified and (mode == 'inspect' or (r is not None and r.get('exit') == 0))
        log_event(event='complete', mode=mode, ok=success, verified_auto=verified)
        fcntl.flock(lease_fd, fcntl.LOCK_UN)
        os.close(lease_fd)
        os._exit(0 if success else 4)
    except BaseException as exc:
        try:
            os.write(wfd, ('FAILED: ' + repr(exc)[:250] + '\n').encode())
        except OSError:
            pass
        try:
            log_event(event='error', error=repr(exc))
        except OSError:
            pass
        os._exit(5)


def main() -> int:
    if len(sys.argv) != 6 or sys.argv[3] not in ('inspect', 'restore'):
        print('usage: root_rescue.py AGENT LOG_PATH inspect|restore DELAY_SECONDS SESSION_HEX', file=sys.stderr)
        return 2
    agent, logpath, mode = sys.argv[1:4]
    session = sys.argv[5]
    if re.fullmatch(r"[0-9a-f]{32}", session) is None:
        return 2
    try:
        delay = float(sys.argv[4])
    except ValueError:
        return 2
    if not (0.1 <= delay <= 30) or os.geteuid() != 0:
        print('root-rescue: privileged launcher and bounded delay required', file=sys.stderr)
        return 2
    if not Path(agent).is_file() or not os.access(agent, os.X_OK):
        print('root-rescue: missing executable', file=sys.stderr)
        return 2
    if not Path(logpath).parent.is_dir():
        print('root-rescue: log folder absent', file=sys.stderr)
        return 2
    # fork a child while already root; detach AFTER sudo has finished authentication.
    rfd, wfd = os.pipe()
    pid = os.fork()
    if pid == 0:
        os.close(rfd)
        daemon_child(wfd, agent, logpath, mode, delay, session)
        os._exit(5)
    os.close(wfd)
    try:
        readable, _, _ = select.select([rfd], [], [], 5.0)
        response = os.read(rfd, 512).decode('utf-8', 'replace').strip() if readable else ''
    finally:
        os.close(rfd)
    if response != 'ARMED':
        print('root-rescue: refused to arm: ' + (response or 'no child acknowledgement'), file=sys.stderr)
        return 3
    print(json.dumps({'operation': 'root-rescue', 'event': 'armed', 'session': session, 'pid': pid,
                      'mode': mode, 'log': logpath, 'delay_s': delay}), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
