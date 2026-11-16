#!/usr/bin/env python3
"""NVMetrics RM v0.5 -- ATTENDED, experimental root companion test.

Default is read-only --dry-run. --run permits ONE 50% lease for 4 seconds.
Only the isolated native agent is elevated. A separately detached root AUTO
backstop attempts another AUTO at +18s even if this Python process crashes.
This is not a kernel watchdog, and root processes can still stall in RM.
"""
from __future__ import annotations
import argparse, datetime, json, math, os, pathlib, selectors, signal, subprocess, sys, time, uuid
BASE=pathlib.Path.home()/'Desktop'
ROOT_RESCUE=pathlib.Path(__file__).with_name('root_rescue.py')
BUNDLED=pathlib.Path(__file__).resolve().parent.parent/'nvmetrics-agent'
BUILT=BASE/'NVMetrics-rm-menu-guard-build/NVMetrics.app/Contents/Resources/nvmetrics-agent'
GUARD=BUNDLED if BUNDLED.is_file() else BUILT
READ=GUARD
SUDO=['/usr/bin/sudo','-n']

def fatal(msg):raise RuntimeError(msg)
def capture(cmd,timeout=8):
    r=subprocess.run(cmd,text=True,capture_output=True,timeout=timeout)
    if r.returncode: fatal('Fallo '+str(cmd[-2:])+' exit='+str(r.returncode)+' '+r.stderr.strip()[:350])
    return r.stdout

def json_last(content):
    for line in content.splitlines()[::-1]:
        try:
            obj=json.loads(line)
            if isinstance(obj,dict):return obj
        except ValueError:pass
    fatal('No hay respuesta JSON interpretable')

def auto_snapshot():
    d=json_last(capture([str(GUARD),'fan','rm-inspect']))
    if d.get('operation')!='rm-snapshot' or d.get('get_regular_status')!=0 or d.get('get_default_status')!=0:
        fatal('RM GET rechazado')
    for typ in ('regular','default'):
        v=d.get(typ) or {}; records=v.get('records') or []
        if v.get('layout_matches_two') is not True or len(records)!=2:
            fatal('RM layout inesperado')
        if not all(r.get('policy_word_candidate')==0 and r.get('requested_q16_candidate')==0 for r in records):
            fatal('RM no indica AUTO para ambos ventiladores; no iniciar')
    return d

def check_status():
    s=json_last(capture([str(READ),'fan','status'],10))
    live=s.get('fan_live') or {}; cap=s.get('fan_backend') or {}
    gpu,mem=s.get('temp_gpu_c'),s.get('temp_mem_c')
    if (type(gpu) not in (int,float) or type(mem) not in (int,float) or not math.isfinite(gpu) or
        not math.isfinite(mem) or not (0<gpu<55 and 0<mem<82) or
        live.get('temperature_fresh') is not True or live.get('mode')!='read_only' or
        live.get('applied') is not False or cap.get('fan_rpm_readable') is not True):
        fatal('GPU/VRAM o lectura RUSD no son seguras')
    state=s.get('fan_rpm')
    if not (state==[] or state==[0,0] or
            (isinstance(state,list) and len(state)>=2 and all(type(v)==int and v>0 for v in state[:2]))):
        fatal('No se puede clasificar el estado Fan Stop / RPM')
    print(f'GPU={gpu} °C, VRAM={mem} °C, fans={state}, RUSD=fresh')
    return state

def sample_stream():
    p=subprocess.Popen([str(READ),'watch','1000'],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    values=[]
    try:
        deadline=time.monotonic()+5.0
        sel=selectors.DefaultSelector();sel.register(p.stdout,selectors.EVENT_READ)
        buf=b''
        while time.monotonic()<deadline and len(values)<4:
            if not sel.select(.5):continue
            got=os.read(p.stdout.fileno(),4096)
            if not got:break
            buf+=got
            while b'\n' in buf:
                row,buf=buf.split(b'\n',1)
                try: v=json.loads(row)
                except ValueError:continue
                if isinstance(v,dict) and type(v.get('t')) in (int,float):values.append(v)
        sel.close()
    finally:
        p.terminate()
        try:p.wait(timeout=2)
        except subprocess.TimeoutExpired:p.kill();p.wait()
    if len(values)<4 or values[-1]['t']-values[0]['t']<2500:
        fatal('No hay cuatro muestras RUSD sostenidas y recientes')
    for v in values:
        gpu,mem,util,pwr=(v.get(k) for k in ('temp_gpu_c','temp_mem_c','util_gpu_pct','power_w'))
        if (type(gpu) not in (int,float) or type(mem) not in (int,float) or
            type(pwr) not in (int,float) or type(util)!=int or
            not all(math.isfinite(x) for x in (gpu,mem,pwr)) or
            not (0<gpu<55 and 0<mem<82 and 0<=pwr<=35 and 0<=util<=20) or
            (v.get('fan_live') or {}).get('temperature_fresh') is not True):
            fatal('RUSD no está estable o la tarjeta tiene carga; no se autoriza SET')
    print(f'Sensores estables: {len(values)} muestras, última potencia={values[-1]["power_w"]} W')

def read_event(proc,sel,deadline,log):
    """Wait for one JSON response, no unbounded blocking readline."""
    buffer=getattr(read_event,'buffer',b'')
    while time.monotonic()<deadline:
        if b'\n' in buffer:
            row,buffer=buffer.split(b'\n',1)
            read_event.buffer=buffer
            text=row.decode('utf-8','replace')
            print(' ',text)
            log.write(text+'\n');log.flush()
            try:return json.loads(text)
            except ValueError:continue
        if proc.poll() is not None:
            # Try to drain stdout even if it ended before the selector woke.
            events=sel.select(0)
            if not events:fatal('Companion root terminó sin informar verificación')
        events=sel.select(.25)
        if events:
            part=os.read(proc.stdout.fileno(),4096)
            if not part:fatal('EOF inesperado del companion root')
            buffer+=part
            read_event.buffer=buffer
    fatal('Tiempo de respuesta RM agotado')

def send(proc,cmd):
    if proc.poll() is not None:fatal('Companion root ya finalizó')
    proc.stdin.write((cmd+'\n').encode());proc.stdin.flush()

def stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + str(os.getpid())

def launch_root_backstop(mode, seconds, session_id):
    """Use sudo in the *current* terminal session, then fork/setsid as root.

    Do not send START 50 unless a root child has acknowledged it is armed.
    """
    if mode not in ('inspect', 'restore'):
        fatal('bad backstop mode')
    log_path=BASE/('NVMetrics-RM-v05-rescue-'+stamp()+'-'+mode+'.jsonl')
    cmd=SUDO+[sys.executable, str(ROOT_RESCUE), str(GUARD), str(log_path), mode, str(seconds), session_id]
    r=subprocess.run(cmd,stdin=subprocess.DEVNULL,capture_output=True,text=True,timeout=12)
    if r.returncode:
        fatal('No se pudo ARMAR rescate root: '+r.stderr.strip()[-500:])
    response=json_last(r.stdout)
    if response.get('event')!='armed' or response.get('mode')!=mode or response.get('log')!=str(log_path) or response.get('session')!=session_id:
        fatal('El backstop no confirmó que quedó armado')
    print(f'Rescate root ARMADO PID={response["pid"]} sesion={session_id[:12]} tipo={mode} plazo=+{seconds}s')
    print('Registro del rescate:',log_path)
    return log_path

def inspect_backstop_check():
    path=launch_root_backstop('inspect',2.0,uuid.uuid4().hex)
    deadline=time.monotonic()+12
    while time.monotonic()<deadline:
        if path.is_file():
            try:
                content=path.read_text(encoding='utf-8')
                for line in content.splitlines()[::-1]:
                    try: r=json.loads(line)
                    except ValueError:continue
                    if isinstance(r,dict) and r.get('event')=='complete':
                        if r.get('ok') is True and r.get('verified_auto') is True:
                            print('RESCUE CHECK OK: helper independiente ejecutó RM GET como root. NINGÚN SET.')
                            return
                        fatal('Backstop root de lectura falló: '+str(path))
            except (OSError,UnicodeDecodeError):pass
        time.sleep(.25)
    fatal('Backstop root no completó la consulta de solo lectura: '+str(path))

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    mode=ap.add_mutually_exclusive_group()
    mode.add_argument('--run',action='store_true',help='ACTIVA un ensayo de 4 segundos a 50%% después de confirmar')
    mode.add_argument('--dry-run',action='store_true',help='Sólo lectura (comportamiento por defecto)')
    mode.add_argument('--rescue-check',action='store_true',help='Valida rescate root diferido SOLO CON RM GET; ningún SET')
    ap.add_argument('--target',type=int,choices=[50,60,75,90,100],default=50,help='Porcentaje máximo del ensayo, solo ascendente desde 50')
    args=ap.parse_args()
    if os.geteuid()==0:fatal('Ejecutar sin sudo; el script eleva solo el companion')
    for p in (READ,GUARD):
        if not p.is_file() or not os.access(p,os.X_OK):fatal('No está compilado: '+str(p))
    print('NVMetrics RM v0.5 --', 'PRUEBA RM CON ESCRITURAS' if args.run else 'VERIFICACIÓN SOLO LECTURA')
    check_status();sample_stream();auto_snapshot()
    print('RM GET AUTO=0 para ambos ventiladores (regular y default)')
    if args.rescue_check:
        if subprocess.run(['/usr/bin/sudo','-v']).returncode:fatal('sudo denegado')
        inspect_backstop_check()
        return
    if not args.run:
        print('DRY RUN OK: ninguna orden RM SET enviada. No instalamos demonios ni servicios.')
        print('Para ensayos breves desde la GUI, usar los lanzadores de Terminal tras validar el rescate.')
        return
    print('PELIGRO: ensayo RM privado; restauración no garantizada si driver/kernel se bloquea.')
    print('Mantener GPU sin carga, supervisión presencial, no abrir otras sesiones RM.')
    if input(f'Escribe GUARD50 para UNA sesión de 4 segundos (50 → {args.target}%): ').strip()!='GUARD50':fatal('Autorización cancelada')
    if subprocess.run(['/usr/bin/sudo','-v']).returncode:fatal('sudo denegado')
    if capture(SUDO+['/usr/bin/id','-u']).strip()!='0':fatal('sin root')
    # Perform another read-only check just before privileged activity.
    auto_snapshot()
    log_path=BASE/('NVMetrics-RM-v05-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'.jsonl')
    rescue_log=None
    print('Log:',log_path)
    # HARD GATE: refuse manual SET unless the second root process is armed.
    # No sudo invocation happens inside the delayed process.
    session_id=uuid.uuid4().hex
    rescue_log=launch_root_backstop('restore',18.0,session_id)
    proc=None;final_auto=False
    with log_path.open('w',encoding='utf8') as log:
        try:
            proc=subprocess.Popen(SUDO+['/usr/bin/env','NVMETRICS_FAN_GUARD=ACEPTO_GUARD_V04',
                           str(GUARD),'fan','rm-guard'],stdin=subprocess.PIPE,
                           stdout=subprocess.PIPE,stderr=log,bufsize=0)
            sel=selectors.DefaultSelector();sel.register(proc.stdout,selectors.EVENT_READ)
            evt=read_event(proc,sel,time.monotonic()+8,log)
            if evt.get('event')!='ready':fatal('Companion root no validó baseline')
            send(proc,'START 50')
            evt=read_event(proc,sel,time.monotonic()+3.5,log)
            if evt.get('event')!='manual' or evt.get('pct')!=50:
                fatal('SET manual no fue confirmado mediante RM GET')
            if args.target > 50:
                send(proc,'RAISE '+str(args.target))
                evt=read_event(proc,sel,time.monotonic()+3.5,log)
                if evt.get('event')!='manual' or evt.get('pct')!=args.target:
                    fatal('RAISE no verificado: '+str(args.target))
            since=time.monotonic();tick=since
            while time.monotonic()-since<4:
                if proc.poll() is not None:fatal('El companion salió durante la sesión')
                if time.monotonic()-tick>=.45:
                    send(proc,'PING');tick=time.monotonic()
                time.sleep(.08)
            send(proc,'AUTO')
            evt=read_event(proc,sel,time.monotonic()+5,log)
            final_auto=evt.get('event')=='auto_verified' and evt.get('pct')==0
            try:exitcode=proc.wait(timeout=4)
            except subprocess.TimeoutExpired:exitcode=None
            if not final_auto or exitcode!=0:fatal(f'Restauración no verificada o worker exit={exitcode}')
            print('AUTO verificado por RM GET en ambos registros; sigue sin ser garantía de firmware.')
        finally:
            if proc is not None:
                if proc.poll() is None:
                    try:send(proc,'AUTO')
                    except Exception:pass
                if proc.stdin:
                    try:proc.stdin.close()
                    except Exception:pass
                try:proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    print('AVISO: companion root no finalizó; el rescate independiente sigue programado.',file=sys.stderr)
                if proc.stdout:proc.stdout.close()
            try:sel.close()
            except (NameError,Exception):pass
        print('Rescate root ya ARMADO a +18 s. Espera su registro; no repitas escritura.')
        print('Consulta del rescate:',rescue_log)
    print('Esperando la restauración independiente con el bloqueo de sesión…')
    deadline=time.monotonic()+28
    completed=None
    while time.monotonic()<deadline:
        try:
            if rescue_log.is_file():
                for row in reversed(rescue_log.read_text(encoding='utf8').splitlines()):
                    try:r=json.loads(row)
                    except ValueError:continue
                    if r.get('event')=='complete' and r.get('session')==session_id:
                        completed=r;break
        except (OSError, UnicodeDecodeError):pass
        if completed:break
        time.sleep(.25)
    if not completed or completed.get('ok') is not True or completed.get('verified_auto') is not True:
        fatal('Rescate independiente NO confirmado, inspeccionar '+str(rescue_log))
    print('Rescate root COMPLETADO y AUTO verificado mediante RM GET. Registro:',rescue_log)

if __name__=='__main__':
    try:main()
    except (RuntimeError,subprocess.TimeoutExpired,KeyboardInterrupt,BrokenPipeError) as e:
        print('ABORTADO:',str(e),file=sys.stderr)
        sys.exit(1)
