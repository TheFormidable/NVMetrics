#!/usr/bin/env python3
"""Upgrade an exactly-verified root v0.11 helper; install launchd on-demand.

One-time sudo only. Never issues RM SET. Blocks if server/rescue/guard locks
are held and checks RM AUTO before touching any privileged file. Backup v0.11
is independent of older backups. The launchd socket is a local Unix endpoint;
peer UID checks and a per-session token remain mandatory.
"""
from __future__ import annotations
import argparse,fcntl,hashlib,json,os,plistlib,stat,subprocess,sys,tempfile
from pathlib import Path

HERE=Path(__file__).resolve().parent
ROOT=Path('/Library/PrivilegedHelperTools/org.nvmetrics.rmguard')
PLIST=Path('/Library/LaunchDaemons/org.nvmetrics.rmguard.plist')
LABEL='org.nvmetrics.rmguard'
MAN=ROOT/'.v012-manifest.json'
BACK=ROOT/'.v012-backup'
NEW={'server.py':HERE/'service/server.py',
     'physical.py':HERE/'service/physical.py',
     'protocol.py':HERE/'service/protocol.py',
     'launchd_socket.py':HERE/'service/launchd_socket.py'}
# Old protocol/agent are the same binary tested on RTX 5050 in v0.11.
BASE_MANIFEST_FILES=('server.py','physical.py','nvmetrics-agent')
OLD_FILES=('server.py','physical.py','protocol.py','nvmetrics-agent')
BASE_PROTOCOL_SHA='28f732c91802fe73e730a5948c3b08ca13d85ccb6329777d04f9f19f3595bb89'
LOCKS=(ROOT/'control.lock',Path('/var/run/nvmetrics-rm-v05-session.lock'),
       Path('/var/run/nvmetrics-rm-v04.lock'))

def sha(b):return hashlib.sha256(b).hexdigest()

def safe_file(p):
    st=p.lstat()
    if (not stat.S_ISREG(st.st_mode) or st.st_uid!=0 or st.st_nlink!=1 or
        st.st_mode&0o022 or p.is_symlink()):
        raise RuntimeError('root_file_untrusted:'+str(p))

def safe_dir(p):
    cur=Path('/')
    for name in p.parts[1:]:
        cur=cur/name;st=cur.lstat()
        if not stat.S_ISDIR(st.st_mode) or st.st_uid!=0 or st.st_mode&0o022 or cur.is_symlink():
            raise RuntimeError('root_dir_untrusted:'+str(cur))

def atomic(path,data,mode):
    fd,tmp=tempfile.mkstemp(prefix='.v012-stage-',dir=str(path.parent))
    try:
        os.fchown(fd,0,0);os.fchmod(fd,mode)
        with os.fdopen(fd,'wb') as f:
            f.write(data);f.flush();os.fsync(f.fileno())
        os.replace(tmp,path)
    finally:
        if os.path.exists(tmp):os.unlink(tmp)

def get_locks():
    held=[]
    try:
        for path in LOCKS:
            fd=os.open(path,os.O_CREAT|os.O_RDWR|getattr(os,'O_NOFOLLOW',0),0o600)
            try:
                safe_file(path)
                if path.stat().st_mode&0o077:raise RuntimeError('unsafe_lock_mode')
                fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
            except BaseException:
                os.close(fd);raise
            held.append(fd)
        return held
    except BaseException:
        for fd in reversed(held):os.close(fd)
        raise

def rm_is_auto():
    # A read-only RM GET; under no circumstance infer AUTO from idle RPM.
    p=subprocess.run([str(ROOT/'nvmetrics-agent'),'fan','rm-inspect'],
        stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=10,
        env={'PATH':'/usr/bin:/bin','HOME':'/var/root','LANG':'C'})
    if p.returncode:return False
    try:
        from service.protocol import parse_agent
        return parse_agent(p.stdout,'inspect').get('mode')=='auto'
    except (ValueError,TypeError,ImportError):return False

def daemon_plist(uid,profile):
    args=['/usr/bin/python3','-E','-s','-B',str(ROOT/'server.py'),
          '--uid',str(uid),'--lifetime','28800','--enable-physical',
          '--acknowledge','RM_V10_LOAD_TEST' if profile=='load' else 'RM_V08_EXPERIMENT',
          '--launchd-socket']
    if profile=='load':args.append('--load-test')
    d={
        'Label':LABEL,'ProgramArguments':args,'RunAtLoad':False,
        'Sockets':{'Listener':{'SockPathName':str(ROOT/'control.sock'),
                               'SockType':'stream','SockPathMode':0o666}},
        'StandardOutPath':str(ROOT/'launchd.stdout.log'),
        'StandardErrorPath':str(ROOT/'launchd.stderr.log'),
        'ProcessType':'Background',
    }
    return plistlib.dumps(d,sort_keys=True)

def launchctl(*args,ok_missing=False):
    out=subprocess.run(['/bin/launchctl',*args],capture_output=True,text=True,timeout=18)
    if out.returncode and not ok_missing:
        raise RuntimeError('launchctl '+str(args[0])+' failed: '+out.stderr[-400:])
    return out.returncode==0

def bootstrap():launchctl('bootstrap','system',str(PLIST))

def bootout():launchctl('bootout','system/'+LABEL,ok_missing=True)

def base_verified():
    safe_dir(ROOT)
    prior=ROOT/'.v011-manifest.json'
    safe_file(prior)
    m=json.loads(prior.read_text())
    if not set(BASE_MANIFEST_FILES).issubset(m):raise RuntimeError('v011_manifest_incomplete')
    for name in BASE_MANIFEST_FILES:
        safe_file(ROOT/name)
        if sha((ROOT/name).read_bytes())!=m[name]['after']:
            raise RuntimeError('v011_root_hash_mismatch:'+name)
    safe_file(ROOT/'protocol.py')
    if sha((ROOT/'protocol.py').read_bytes())!=BASE_PROTOCOL_SHA:
        raise RuntimeError('v011_protocol_hash_mismatch')
    if b'NVMETRICS_RM_V011_TELEMETRY_GUARD' not in (ROOT/'nvmetrics-agent').read_bytes():
        raise RuntimeError('v011_agent_marker_missing')

def install(uid,profile,agent):
    if MAN.is_file():
        status()  # Verify every installed hash before claiming idempotence.
        m=json.loads(MAN.read_text())
        if m['plist']['uid']==uid and m['plist']['profile']==profile:
            print('v0.12 ya instalado y verificado. No hay cambios que aplicar.');return
        raise RuntimeError('v012_installed_with_different_uid_or_profile')
    if BACK.exists() or PLIST.exists() or (ROOT/'launchd_socket.py').exists():
        raise RuntimeError('v012_partial_install_refused')
    safe_dir(PLIST.parent);base_verified()
    if not rm_is_auto():raise RuntimeError('RM_not_verified_AUTO_do_not_install')
    held=get_locks()
    try:
        # Root service cannot still be listening if its exclusive lock was free.
        if not rm_is_auto():raise RuntimeError('RM_not_AUTO_after_lock')
        if not agent or not agent.is_file() or agent.is_symlink():
            raise RuntimeError('new_agent_missing')
        ast=agent.lstat()
        if not stat.S_ISREG(ast.st_mode) or ast.st_mode&0o022 or ast.st_size>25000000:
            raise RuntimeError('new_agent_untrusted')
        new_agent=agent.read_bytes()
        if b'NVMETRICS_RM_V012_LOW_CALIBRATION' not in new_agent:
            raise RuntimeError('v012_agent_marker_missing')
        old={k:(ROOT/k).read_bytes() for k in OLD_FILES}
        new={k:v.read_bytes() for k,v in NEW.items()}
        new['nvmetrics-agent']=new_agent
        if b'NVMetrics RM v0.12' not in new['server.py']:
            raise RuntimeError('new_server_identity_missing')
        if b'LOWER ' not in new['physical.py'] or b'launch_activate_socket' not in new['launchd_socket.py']:
            raise RuntimeError('new_feature_marker_missing')
        plist=daemon_plist(uid,profile)
        BACK.mkdir(mode=0o700);os.chown(BACK,0,0)
        for name,content in old.items():
            atomic(BACK/name,content,0o600)
        committed=[]
        try:
            # Old foreground control.sock is expected to have been unlinked at shutdown.
            sock=ROOT/'control.sock'
            if os.path.lexists(sock):
                ss=sock.lstat()
                if not stat.S_ISSOCK(ss.st_mode) or ss.st_uid not in (0,uid):
                    raise RuntimeError('unexpected_existing_socket')
                sock.unlink()
            for name,data in new.items():
                atomic(ROOT/name,data,0o700 if name=='nvmetrics-agent' else 0o644)
                committed.append(name)
            atomic(PLIST,plist,0o600)
            if subprocess.run(['/usr/bin/plutil','-lint',str(PLIST)],capture_output=True,timeout=6).returncode!=0:
                raise RuntimeError('plist_failed_plutil')
            bootstrap()
            manifest={name:{'before':sha(old[name]) if name in old else None,
                            'after':sha(new[name])} for name in new}
            manifest['plist']={'after':sha(plist),'uid':uid,'profile':profile}
            atomic(MAN,(json.dumps(manifest,indent=2)+'\n').encode(),0o600)
        except BaseException:
            bootout()
            for name in reversed(committed):
                if name in old:atomic(ROOT/name,old[name],0o700 if name=='nvmetrics-agent' else 0o644)
                else:(ROOT/name).unlink(missing_ok=True)
            PLIST.unlink(missing_ok=True)
            for name in old:(BACK/name).unlink(missing_ok=True)
            BACK.rmdir()
            raise
        print('NVMetrics v0.12 helper launchd instalado. Socket local bajo demanda; modo',profile)
        print('No se invocó RM SET. Abre NVMetrics para activar el IPC automáticamente.')
    finally:
        for fd in reversed(held):os.close(fd)

def status():
    if not MAN.is_file():print('v0.12 no instalado');return 2
    safe_file(MAN);m=json.loads(MAN.read_text())
    for name in NEW:
        safe_file(ROOT/name)
        if sha((ROOT/name).read_bytes())!=m[name]['after']:
            raise RuntimeError('changed_file:'+name)
    safe_file(PLIST)
    if sha(PLIST.read_bytes())!=m['plist']['after']:
        raise RuntimeError('changed_plist')
    print('v0.12 instalado; hashes OK; uid=',m['plist']['uid'],'perfil=',m['plist']['profile'])
    print('launchd socket on-demand; abrir GUI dispara inicio del servicio.')
    return 0

def set_profile(profile):
    """Explicit administrative change, only after a verified AUTO baseline.

    No RM SET. The on-demand listener is restarted with the new opt-in policy.
    """
    if not MAN.is_file():raise RuntimeError('v012_not_installed')
    status()
    if not rm_is_auto():raise RuntimeError('RM_not_verified_AUTO_do_not_switch')
    old_manifest=MAN.read_bytes()
    m=json.loads(old_manifest)
    if m['plist']['profile']==profile:
        print('Perfil',profile,'ya configurado; sin cambios.');return
    old_plist=PLIST.read_bytes()
    uid=m['plist']['uid']
    new_plist=daemon_plist(uid,profile)
    bootout()
    try:
        held=get_locks()
    except BaseException:
        # Restore the previous listener when a guard/rescue still owns a lock.
        bootstrap()
        raise
    try:
        if not rm_is_auto():raise RuntimeError('RM_not_AUTO_after_bootout')
        try:
            atomic(PLIST,new_plist,0o600)
            m['plist']['after']=sha(new_plist)
            m['plist']['profile']=profile
            atomic(MAN,(json.dumps(m,indent=2)+'\n').encode(),0o600)
            bootstrap()
        except BaseException:
            bootout()
            atomic(PLIST,old_plist,0o600)
            atomic(MAN,old_manifest,0o600)
            bootstrap()
            raise
        print('Perfil launchd cambiado a',profile,'. Reinicia la aplicación para consultar IPC.')
        print('Nunca cambies de perfil durante Manual: AUTO fue verificado antes del cambio.')
    finally:
        for fd in reversed(held):os.close(fd)

def rollback():
    if (ROOT/'.v0121-backup').exists():
        raise RuntimeError('rollback_v0121_first_using_update_v0121.py')
    if not MAN.is_file() or not BACK.is_dir():
        raise RuntimeError('no_v012_to_rollback')
    safe_file(MAN);safe_dir(BACK)
    m=json.loads(MAN.read_text())
    for name in NEW:
        safe_file(ROOT/name)
        if sha((ROOT/name).read_bytes())!=m[name]['after']:
            raise RuntimeError('third_party_edit:'+name)
    safe_file(PLIST)
    if sha(PLIST.read_bytes())!=m['plist']['after']:
        raise RuntimeError('third_party_edit:plist')
    if not rm_is_auto():raise RuntimeError('RM_not_verified_AUTO_do_not_rollback')
    # It is safe to stop the on-demand service when no manual RM is active.
    bootout()
    held=get_locks()
    try:
        if not rm_is_auto():raise RuntimeError('RM_not_AUTO_after_bootout')
        for name in NEW:
            if name in OLD_FILES:
                safe_file(BACK/name)
                expected=m[name]['before']
                if sha((BACK/name).read_bytes())!=expected:
                    raise RuntimeError('backup_corrupted:'+name)
        for name in NEW:
            if name in OLD_FILES:atomic(ROOT/name,(BACK/name).read_bytes(),0o700 if name=='nvmetrics-agent' else 0o644)
            else:(ROOT/name).unlink()
        PLIST.unlink();MAN.unlink()
        for name in OLD_FILES:(BACK/name).unlink()
        BACK.rmdir()
        # Launchd no longer owns the endpoint; remove only the known stale socket.
        sock=ROOT/'control.sock'
        if os.path.lexists(sock) and stat.S_ISSOCK(sock.lstat().st_mode):sock.unlink()
        print('v0.11 root restaurado. IPC bajo demanda desinstalado; RM AUTO verificado.')
    finally:
        for fd in reversed(held):os.close(fd)

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--uid',type=int)
    ap.add_argument('--agent',type=Path)
    ap.add_argument('--profile',choices=('idle','load'),default='idle')
    g=ap.add_mutually_exclusive_group();g.add_argument('--rollback',action='store_true');g.add_argument('--status',action='store_true');g.add_argument('--set-profile',choices=('idle','load'))
    args=ap.parse_args()
    if sys.platform!='darwin' or os.geteuid()!=0:
        ap.error('macOS con sudo requerido')
    if args.status:return status()
    if args.rollback:return rollback()
    if args.set_profile:return set_profile(args.set_profile)
    uid=args.uid
    if type(uid) is not int or uid<501 or uid>60000:
        ap.error('indica --uid $(id -u) desde la cuenta normal')
    if os.environ.get('SUDO_UID') and uid!=int(os.environ['SUDO_UID']):
        ap.error('UID distinto del usuario que autorizó sudo')
    if args.profile=='load':
        print('ADVERTENCIA: perfil de carga permite hasta 145W/65°C GPU/88°C VRAM; prohíbe bajar de 50%.')
    install(uid,args.profile,args.agent)
    return 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (RuntimeError,OSError,ValueError,KeyError,subprocess.TimeoutExpired,BlockingIOError) as e:
        raise SystemExit('ABORTADO: '+str(e))
