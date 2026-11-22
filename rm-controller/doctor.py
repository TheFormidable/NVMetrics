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


def build_report(agent, read=read_agent, ping=lambda: request('ping', timeout=3)):
    checks = []

    def add(name, status, detail):
        checks.append(dict(name=name, status=status, detail=detail))

    if not agent.is_file():
        add('Agente', 'error', 'El ejecutable integrado no existe.')
    else:
        add('Agente', 'ok', 'Ejecutable integrado encontrado.')
    samples = read(agent, ('once',))
    sample = samples[-1] if samples else {}
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
    if len(sys.argv) != 1 or sys.platform != 'darwin' or not getattr(sys, 'frozen', False):
        print('Doctor requiere el runtime integrado de NVMetrics en macOS.', file=sys.stderr)
        return 2
    agent = Path(sys.executable).parent.parent.parent / 'nvmetrics-agent'
    print(json.dumps(build_report(agent), ensure_ascii=False, allow_nan=False))
    return 0
