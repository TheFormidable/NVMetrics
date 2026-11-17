"""Package transaction, run as root ONLY by macOS Installer. No RM SET.

Initial scope: local, explicitly experimental RTX 5050, exact v0.13.4 migration
or fresh helper. The root runtime is never executed from /Applications.
"""
from __future__ import annotations
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import sys
import tempfile
import time

import install_v012 as base
from protocol import parse_agent

VERSION = '0.14.9'
ROOT = base.ROOT
APP = Path('/Applications/NVMetrics.app')
RUNTIME = ROOT/'Runtime'
RECEIPT = ROOT/'.v014-package.json'
BACK = ROOT/'.v0149-package-backup'
FILES = ('server.py','backend.py','peer.py','physical.py','rescue.py','protocol.py','launchd_socket.py','nvmetrics-agent','nvmetrics-setting.sh')
LEGACY = {
    'server.py':'aa833add4bd6377672441802fce5d38d9226862a98ca1b272e9e22e99378d64b',
    'backend.py':'7a8167ae21ec72277a0ffa66f0f2223db75b7bc936568457e34921d10b35b632',
    'peer.py':'4ee407d4443dbace4441b6037b72b7498c89288ba30ec5717c96adf1b3bb3bd6',
    'physical.py':'6bd9904856f88560e4573d0822379b0aee7bc2ebb71d4aa6ec7c31da11153c46',
    'rescue.py':'98ee1db6028b5db0fa24cce7b44c816cb2a41f53e8214345ce56046a78360c7e',
    'protocol.py':'0b385a66267e1fec6f781b29c57e1fab0556b8bdaf3a5c6d8590b10f9527643e',
    'launchd_socket.py':'3f5645aaf924af8536586772a68c8e671418ad9b6797d2744233a38fdaaf8bf5',
}
MARKER = b'NVMETRICS_RM_V0149_AUDIT_HARDENING'
V0148_MARKER = b'NVMETRICS_RM_V0148_BOOST_POLICY'
SESSION_MARKER = b'NVMETRICS_RM_V0147_SESSION_READER'
FRESHNESS_MARKER = b'NVMETRICS_RM_V0146_SENSOR_FRESHNESS'
ADAPTIVE_MARKER = b'NVMETRICS_RM_V0143_ADAPTIVE_ENTRY'
PREVIOUS_MARKER = b'NVMETRICS_RM_V0141_THERMAL_SUPERVISOR'
LEGACY_MARKER = b'NVMETRICS_RM_V0134_RUSD_IDLE_PROFILE'
ENV = {'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','HOME':'/var/root','LANG':'C'}


def run(args, timeout=20):
    return subprocess.run([str(a) for a in args], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=timeout, env=ENV)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_entries(path):
    """Enumerate the names, bytes and internal symlinks of an immutable tree."""
    if not path.is_dir() or path.is_symlink():
        raise RuntimeError('invalid_payload_tree:'+str(path))
    root=path.resolve()
    for p in sorted(path.rglob('*')):
        rel=p.relative_to(path).as_posix()
        st=p.lstat()
        if stat.S_ISLNK(st.st_mode):
            target=os.readlink(p)
            if os.path.isabs(target) or not p.resolve(strict=True).is_relative_to(root):
                raise RuntimeError('escaping_payload_symlink:'+rel)
            value='link:'+target
        elif stat.S_ISDIR(st.st_mode): value='dir'
        elif stat.S_ISREG(st.st_mode): value='file:'+sha(p)
        else: raise RuntimeError('special_payload_file:'+rel)
        yield rel,value


def tree_digest(path):
    """Bind an immutable payload to its names, bytes and internal symlinks."""
    h=hashlib.sha256()
    for rel,value in tree_entries(path):
        h.update(json.dumps([rel,value],ensure_ascii=True).encode()+b'\n')
    return h.hexdigest()


def secure_tree(path, public=False):
    """Called only on private root-created staging copies; never follow links."""
    tree_digest(path)
    for p in [path,*path.rglob('*')]:
        st=p.lstat()
        os.chown(p,0,0,follow_symlinks=False)
        if not stat.S_ISLNK(st.st_mode):
            mode=(stat.S_IMODE(st.st_mode)&~0o7022) | (0o500 if stat.S_ISDIR(st.st_mode) else 0o400)
            if public: mode|=0o444 | (0o111 if mode&0o100 else 0)
            os.chmod(p,mode)


def inspect_auto(agent, *, qualified=False):
    p=run([agent,'fan','rm-inspect'],10)
    if p.returncode: raise RuntimeError('RM inspection failed; no changes made')
    result=parse_agent(p.stdout,'inspect')
    if result['mode']!='auto': raise RuntimeError('RM AUTO not verified; close NVMetrics, stop Manual and retry')
    if qualified and any((r.get('rpm_min_candidate'),r.get('rpm_max_candidate'))!=(1150,3700)
                         for name in ('regular','default') for r in result[name]):
        raise RuntimeError('Unqualified cooler layout; this experimental package is for the tested RTX 5050')


def qualified_gpu():
    result=run(['/usr/sbin/system_profiler','SPDisplaysDataType','-json'],30)
    if result.returncode: raise RuntimeError('GPU identification unavailable')
    displays=json.loads(result.stdout).get('SPDisplaysDataType',[])
    names=[str(d.get('sppci_model','')) for d in displays if isinstance(d,dict)]
    # No claim of multi-GPU routing or general compatibility.
    if len(displays)!=1 or len(names)!=1 or 'rtx 5050' not in names[0].casefold():
        raise RuntimeError('Only the single RTX 5050 validation platform is supported by this package')


def launch_plist(uid):
    d=plistlib.loads(base.daemon_plist(uid,'idle'))
    d['ProgramArguments']=[str(RUNTIME/'nvmetrics-runtime'),'server','--uid',str(uid),'--lifetime','28800',
                           '--enable-physical','--thermal-supervised','--acknowledge','RM_V08_EXPERIMENT','--launchd-socket']
    return plistlib.dumps(d,sort_keys=True)


def acquire(path):
    fd=os.open(path,os.O_CREAT|os.O_RDWR|getattr(os,'O_NOFOLLOW',0),0o600)
    try:
        base.safe_file(path)
        if os.fstat(fd).st_mode&0o077: raise RuntimeError('unsafe lock permissions')
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        return fd
    except BaseException:
        os.close(fd)
        raise


def loaded(): return base.launchctl('print','system/'+base.LABEL,ok_missing=True)


def stop_job():
    if loaded(): base.bootout()
    for _ in range(30):
        if not loaded():
            time.sleep(1.5)
            if not loaded(): return
        time.sleep(.15)
    raise RuntimeError('launchd listener did not stop')


def start_job():
    for n in range(4):
        try:
            if not loaded(): base.bootstrap()
            if loaded(): return
        except RuntimeError:
            if n==3: raise
        time.sleep(1.5*(n+1))
    raise RuntimeError('launchd registration unsuccessful')


def verify_client(uid):
    """Test real on-demand startup as the authorized non-root desktop UID."""
    client=APP/'Contents/Resources/GuardTools/Runtime/nvmetrics-runtime'
    prefix=['/usr/bin/sudo','-n','-u',f'#{uid}','--',client,'client']
    for attempt in range(3):
        try:
            ping=run(prefix+['ping'],15)
            status=json.loads(ping.stdout) if ping.returncode==0 else {}
            if (status.get('ok') is True and status.get('revision')==VERSION and
                status.get('authorized_uid')==uid and status.get('writes_enabled') is True and
                status.get('manual_floor')==30 and status.get('safety_profile')=='thermal_supervised'):
                inspect=run(prefix+['inspect'],15)
                reply=json.loads(inspect.stdout) if inspect.returncode==0 else {}
                if reply.get('ok') is True and reply.get('data',{}).get('mode')=='auto': return
        except (ValueError,TypeError,subprocess.SubprocessError): pass
        time.sleep(1)
    raise RuntimeError('installed GUI client could not verify launchd service and firmware AUTO')


def installed_state(uid):
    if RECEIPT.exists():
        base.safe_file(RECEIPT)
        record=json.loads(RECEIPT.read_text())
        if record.get('version') not in ('0.14.0','0.14.1','0.14.2','0.14.3','0.14.4','0.14.5','0.14.6','0.14.7','0.14.8',VERSION) or record.get('uid')!=uid:
            raise RuntimeError('different installed package/authorized user')
        base.safe_file(base.PLIST)
        if sha(base.PLIST)!=record['plist'] or tree_digest(RUNTIME)!=record['runtime'] or tree_digest(APP)!=record['app']:
            raise RuntimeError('installed package differs; do not overwrite')
        for n in FILES:
            if n=='nvmetrics-setting.sh' and record.get('version')!=VERSION:continue
            base.safe_file(ROOT/n)
            if sha(ROOT/n)!=record['files'][n]: raise RuntimeError('installed file changed:'+n)
        if record.get('version')==VERSION:return 'same'
        expected_marker = {'0.14.0':LEGACY_MARKER,'0.14.3':ADAPTIVE_MARKER,'0.14.4':ADAPTIVE_MARKER,'0.14.5':ADAPTIVE_MARKER,'0.14.6':FRESHNESS_MARKER,'0.14.7':SESSION_MARKER,'0.14.8':V0148_MARKER}.get(record.get('version'),PREVIOUS_MARKER)
        if expected_marker not in (ROOT/'nvmetrics-agent').read_bytes():raise RuntimeError('wrong previous agent')
        if BACK.exists():raise RuntimeError('previous v0.14.9 backup exists')
        return 'upgrade'
    if BACK.exists() or RUNTIME.exists(): raise RuntimeError('unfinished or unknown package install; ask maintainer')
    if not base.PLIST.exists() and not base.MAN.exists():
        if loaded() or any((ROOT/n).exists() for n in FILES): raise RuntimeError('unrecognized previous helper')
        return 'fresh'
    if base.status()!=0: raise RuntimeError('unknown previous installation')
    record=json.loads(base.MAN.read_text())
    if record['plist']['uid']!=uid or record['plist']['profile']!='idle':
        raise RuntimeError('previous helper is not the same idle console user')
    for n,digest in LEGACY.items():
        base.safe_file(ROOT/n)
        if sha(ROOT/n)!=digest: raise RuntimeError('expected exact v0.13.4:'+n)
    base.safe_file(ROOT/'nvmetrics-agent')
    if LEGACY_MARKER not in (ROOT/'nvmetrics-agent').read_bytes(): raise RuntimeError('wrong previous agent')
    return 'legacy'


def app_identity(path):
    with (path/'Contents/Info.plist').open('rb') as f:
        return plistlib.load(f)['CFBundleIdentifier']


def install(payload, uid):
    # Installer package content, not a user-provided agent path. Exact expected
    # payload shape and integrity are checked before any privileged mutation.
    app=payload/'NVMetrics.app'
    runtime=app/'Contents/Resources/GuardTools/Runtime'
    source=payload/'service'
    agent=app/'Contents/Resources/nvmetrics-agent'
    expected=json.loads((payload/'integrity.json').read_text())
    for name,path in (('app',app),('service',source)):
        if tree_digest(path)!=expected[name]:
            raise RuntimeError('package integrity mismatch: '+name)
    if MARKER not in agent.read_bytes() or not os.access(runtime/'nvmetrics-runtime',os.X_OK):
        raise RuntimeError('incorrect native agent or frozen runtime')
    if os.path.lexists(APP) and (APP.is_symlink() or app_identity(APP)!=app_identity(app)):
        raise RuntimeError('existing app identity differs; refusing overwrite')
    qualified_gpu()
    inspect_auto(agent,qualified=True)
    base.safe_dir(ROOT.parent.parent);base.safe_dir(base.PLIST.parent)
    if not os.path.lexists(ROOT.parent):
        ROOT.parent.mkdir(mode=0o755);ROOT.parent.chmod(0o755)
    base.safe_dir(ROOT.parent)
    if not os.path.lexists(ROOT):
        ROOT.mkdir(mode=0o755);ROOT.chmod(0o755)
    base.safe_dir(ROOT)
    serial=acquire(ROOT/'.pkg-install.lock')
    held=[];was_loaded=False; stopped=False; mutated=False; moved_app=False; new_app=False;control_released=False
    old_runtime_moved=False;new_runtime=False
    previous={};stage=None;appstage=None;state=None
    try:
        state=installed_state(uid)
        if state=='same':
            start_job();verify_client(uid);print('NVMetrics package already verified');return
        # Acquire the physical lease BEFORE bootout; prevent new manual starts.
        for path in base.LOCKS[1:]: held.append(acquire(path))
        if state in ('legacy','upgrade'): inspect_auto(ROOT/'nvmetrics-agent',qualified=True)
        was_loaded=loaded()
        if was_loaded: stopped=True;stop_job()
        held.append(acquire(ROOT/'control.lock'))
        inspect_auto(agent,qualified=True)
        if state in ('legacy','upgrade'): inspect_auto(ROOT/'nvmetrics-agent',qualified=True)
        backup_app=APP.parent/'.NVMetrics.app.before-v0149'
        if os.path.lexists(backup_app): raise RuntimeError('previous app backup already exists')
        stage=Path(tempfile.mkdtemp(prefix='.pkg-stage-',dir=ROOT))
        appstage=Path(tempfile.mkdtemp(prefix='.nvmetrics-install-',dir=APP.parent))
        appcopy=appstage/'NVMetrics.app'; rtcopy=stage/'Runtime'
        for a,b in ((app,appcopy),(runtime,rtcopy)):
            p=run(['/usr/bin/ditto',a,b],90)
            if p.returncode: raise RuntimeError('unable to stage package files')
            secure_tree(b,public=(b==appcopy))
            if tree_digest(a)!=tree_digest(b): raise RuntimeError('staged file integrity mismatch')
        changed_paths=[ROOT/n for n in FILES]+[base.MAN,base.PLIST,RECEIPT]
        for path in changed_paths:
            if os.path.lexists(path):
                base.safe_file(path);previous[path]=(path.read_bytes(),stat.S_IMODE(path.stat().st_mode))
        BACK.mkdir(mode=0o700)
        mutated=True
        for path,(data,mode) in previous.items(): base.atomic(BACK/path.name,data,0o600)
        for n in FILES:
            data=(agent if n=='nvmetrics-agent' else source/n).read_bytes()
            base.atomic(ROOT/n,data,0o700 if n in ('nvmetrics-agent','nvmetrics-setting.sh') else 0o644)
        if state=='upgrade':
            RUNTIME.rename(BACK/'Runtime');old_runtime_moved=True
        rtcopy.rename(RUNTIME)
        new_runtime=True
        config=launch_plist(uid);base.atomic(base.PLIST,config,0o600)
        if run(['/usr/bin/plutil','-lint',base.PLIST]).returncode: raise RuntimeError('invalid daemon plist')
        manifest={n:{'before':None,'after':sha(ROOT/n)} for n in FILES}
        manifest['plist']={'after':sha(base.PLIST),'uid':uid,'profile':'idle'}
        base.atomic(base.MAN,json.dumps(manifest).encode(),0o600)
        inspect_auto(ROOT/'nvmetrics-agent',qualified=True)
        # Do not unlink a live listener. A stale socket is removed only after
        # launchd stopped and all three physical/service locks are held.
        sock=ROOT/'control.sock'
        if os.path.lexists(sock):
            st=sock.lstat()
            if not stat.S_ISSOCK(st.st_mode) or st.st_uid!=0: raise RuntimeError('unexpected socket')
            sock.unlink()
        if APP.exists(): APP.rename(backup_app);moved_app=True
        appcopy.rename(APP);new_app=True
        record={'version':VERSION,'uid':uid,'app':tree_digest(APP),'runtime':tree_digest(RUNTIME),
                'plist':sha(base.PLIST),'files':{n:sha(ROOT/n) for n in FILES}}
        base.atomic(RECEIPT,json.dumps(record).encode(),0o600)
        start_job()
        # Only the service lock is released; the physical session/guard locks
        # remain held while the actual unprivileged IPC handshake is verified.
        os.close(held.pop());control_released=True
        verify_client(uid)
        print('NVMetrics installed; firmware AUTO verified. No RM SET. Open NVMetrics in Applications.')
    except BaseException:
        if mutated:
            stop_job()
            if control_released:
                for attempt in range(20):
                    try: held.append(acquire(ROOT/'control.lock'));break
                    except BlockingIOError:
                        if attempt==19: raise RuntimeError('service still owns lock; backup retained')
                        time.sleep(.15)
            if new_app: APP.rename(appstage/'failed-app')
            if moved_app: backup_app.rename(APP)
            if new_runtime and RUNTIME.exists(): RUNTIME.rename(stage/'failed-runtime')
            if old_runtime_moved: (BACK/'Runtime').rename(RUNTIME)
            for path in changed_paths:
                if path in previous: base.atomic(path,*previous[path])
                elif path.exists(): path.unlink()
            # All backup files were created by this transaction.
            for path in BACK.iterdir(): path.unlink()
            BACK.rmdir()
        if stopped and was_loaded: start_job()
        raise
    finally:
        for fd in reversed(held): os.close(fd)
        os.close(serial)
        for path in (stage,appstage):
            if path is not None: shutil.rmtree(path)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('payload',type=Path);args=parser.parse_args()
    if sys.platform!='darwin' or os.geteuid()!=0: parser.error('macOS Installer root context required')
    uid=Path('/dev/console').stat().st_uid
    if uid<501: parser.error('Log in to a regular macOS desktop account before installation')
    os.umask(0o077)
    try: install(args.payload.resolve(strict=True),uid)
    except (OSError,ValueError,RuntimeError,KeyError,subprocess.SubprocessError) as e:
        print('NVMetrics Installer: '+str(e),file=sys.stderr);return 1
    return 0
