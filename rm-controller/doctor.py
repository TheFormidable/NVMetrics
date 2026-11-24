"""Read-only, privacy-preserving diagnostic report for the desktop app.

No sudo, no RM SET, no profiles, no session tokens, no raw command output.
The helper never infers permission to write from GPU Caps or successful GETs.
"""
from __future__ import annotations

import json
import math
import platform
import subprocess
import sys
import time
from pathlib import Path

from nvmetrics_ipc_v08 import request

COMMANDS = (('once',), ('caps',), ('eco', 'status'), ('policy', 'status'))


def read_agent(agent, args):
    if args not in COMMANDS:
        raise ValueError('not a read-only diagnostic')
    try:
        result = subprocess.run([str(agent), *args], stdin=subprocess.DEVNULL,
                                capture_output=True, timeout=6, check=False)
        if len(result.stdout) > 1_048_576:
            return []
        if result.returncode != 0 and args != ('caps',):
            return []
        rows = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
        return [row for row in rows if type(row) is dict]
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return []


def fresh_snapshot(sample, now_ms=None):
    if type(sample) is not dict:
        return False
    stamp = sample.get('t')
    if type(stamp) not in (int, float) or not 0 < stamp < 1e15:
        return False
    age = (time.time() * 1000 if now_ms is None else now_ms) - stamp
    return 0 <= age <= 5000


def build_report(agent, read=read_agent, ping=lambda: request('ping', timeout=3), snapshot=None):
    checks = []

    def add(name, status, detail):
        checks.append(dict(name=name, status=status, detail=detail))

    if not agent.is_file():
        add('Agente', 'error', 'El ejecutable integrado no existe.')
    else:
        add('Agente', 'ok', 'Ejecutable integrado encontrado.')
    reused = fresh_snapshot(snapshot)
    samples = [snapshot] if reused else read(agent, ('once',))
    sample = samples[-1] if samples else {}
    add('Fuente de telemetría', 'info', 'Muestra reciente del monitor (sin abrir otra conexión RUSD).'
        if reused else 'Lectura directa del agente.')
    def numeric(value):
        return type(value) is int or (type(value) is float and math.isfinite(value))

    if not sample:
        add('RUSD', 'error', 'No se obtuvo telemetría. Comprueba el driver y la GPU.')
    else:
        for name, key, unit, lo, hi in (
            ('Temperatura GPU', 'temp_gpu_c', '°C', 0, 150),
            ('Temperatura VRAM', 'temp_mem_c', '°C', 0, 150),
            ('Potencia', 'power_w', 'W', 0, 1000)):
            value = sample.get(key)
            valid = numeric(value) and lo <= value <= hi and (unit != '°C' or value > 0)
            add(name, 'ok' if valid else 'unknown', f'{value:.1f} {unit}' if valid else 'Sin lectura válida.')
        profile = sample.get('gpu_profile')
        mask = profile.get('tach_slot_mask') if type(profile) is dict else None
        slots = [str(i) for i in range(16) if type(mask) is int and 0 <= mask <= 65535 and mask & (1 << i)]
        add('Tacómetros', 'ok' if slots else 'unknown',
            'Slots observados: ' + ', '.join(slots) + ' (no indica número físico de ventiladores).' if slots else 'Sin slots confirmados; no implica Fan Stop.')
        rpm = sample.get('fan_rpm')
        if type(rpm) is list and all(type(v) is int and v >= 0 for v in rpm):
            if any(rpm):
                add('Ventilación', 'ok', 'RUSD informa rotación; no certifica el número físico de ventiladores.')
            else:
                add('Ventilación', 'info', 'RUSD informa cero RPM; compatible con Fan Stop, no confirmado físicamente.')
        else:
            add('Ventilación', 'unknown', 'Lectura RPM ausente o inválida; no implica Fan Stop.')
        health = sample.get('health')
        for key, label in (('xid_count', 'Errores Xid'), ('bugchecks', 'Fallos de firmware'), ('rm_asserts', 'RM asserts')):
            count = health.get(key) if type(health) is dict else None
            valid = type(count) is int and count >= 0
            add(label, ('warn' if count else 'ok') if valid else 'unknown', str(count) if valid else 'Contador no disponible.')

    caps = read(agent, ('caps',))
    verdicts = {
        'usable': 'ABI compatible',
        'unsupported_build': 'Agente compilado sin el encabezado GPU Caps',
        'open_failed': 'No se pudo abrir el servicio NVRM',
        'no_service': 'Servicio NVRM no encontrado',
        'old_kext': 'El driver activo no anuncia la ABI',
        'other_major': 'Versión mayor de ABI incompatible',
        'bad_reply': 'Respuesta ABI inválida',
        'bad_call': 'Consulta ABI rechazada',
        'pending': 'Capacidades pendientes',
        'off': 'Capacidades desactivadas',
        'lost': 'Capacidades perdidas',
        'stale': 'Capacidades no actualizadas',
    }
    states = [r.get('verdict') if r.get('verdict') in verdicts else 'unknown'
              for r in caps if type(r.get('verdict')) is str]
    details = []
    for row in caps:
        v = row.get('verdict')
        if type(v) is not str:
            continue
        detail = f'{verdicts[v]} ({v})' if v in verdicts else 'Respuesta no reconocida'
        facts = row.get('capabilities')
        if v == 'usable' and type(facts) is list:
            verified = sum(type(f) is dict and f.get('state') == 'verified' for f in facts)
            detail += f'; {verified} capacidades verificadas'
        details.append(detail)
    add('GPU Caps ABI', 'ok' if states and all(s == 'usable' for s in states)
        else 'warn' if any(s != 'unknown' for s in states) else 'unknown',
        '; '.join(details) if details else 'ABI no disponible. No concede permisos de escritura.')
    if len(caps) == 1 and states == ['usable']:
        facts = caps[0].get('capabilities')
        if type(facts) is list:
            for key, label in (('CHIP_NAME', 'Chip'), ('RM_VERSION', 'Versión RM'), ('GSP_FW_VERSION', 'Firmware GSP')):
                value = next((f.get('value') for f in facts if type(f) is dict and
                              f.get('name') == key and f.get('state') == 'verified'), None)
                valid = type(value) is str and 0 < len(value) <= 64 and all(c.isalnum() or c in '.-_ ' for c in value)
                add(label, 'ok' if valid else 'unknown', value if valid else 'No verificado por la ABI.')
    for args, key, allowed, label in ((('eco', 'status'), 'eco', ('on', 'off'), 'Eco'),
                                      (('policy', 'status'), 'policy', ('idle', 'normal', 'undecided'), 'Política de boost')):
        rows = read(agent, args)
        value = rows[-1].get(key) if rows else None
        add(label, 'ok' if value in allowed else 'unknown', value if value in allowed else 'Estado no verificado.')
    try:
        status = ping()
    except (OSError, ValueError, TypeError):
        status = {}
    online = type(status) is dict and status.get('ok') is True
    add('Servicio RM', 'ok' if online else 'warn', 'IPC autenticado disponible.' if online else 'Sin respuesta IPC autenticada.')
    enabled = status.get('writes_enabled') if online else None
    add('Control Manual', 'info' if type(enabled) is bool else 'unknown',
        'Servicio habilitado; cada sesión requiere cualificación y guard.' if enabled is True else
        'Escrituras deshabilitadas.' if enabled is False else 'Permiso no verificado.')
    return {'schema': 1, 'macos': platform.mac_ver()[0] or 'no macOS', 'checks': checks}


def main():
    if sys.argv[1:] not in ([], ['--snapshot']) or sys.platform != 'darwin' or not getattr(sys, 'frozen', False):
        print('Doctor requiere el runtime integrado de NVMetrics en macOS.', file=sys.stderr)
        return 2
    agent = Path(sys.executable).parent.parent.parent / 'nvmetrics-agent'
    sample = None
    if sys.argv[1:] == ['--snapshot']:
        try:
            raw = sys.stdin.buffer.read(65537)
            if len(raw) <= 65536:
                sample = json.loads(raw)
        except (OSError, ValueError):
            pass
    print(json.dumps(build_report(agent, snapshot=sample), ensure_ascii=False, allow_nan=False))
    return 0
