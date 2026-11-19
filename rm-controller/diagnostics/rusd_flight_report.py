#!/usr/bin/env python3
"""Resume el log del RUSD Flight Recorder del guard root (solo lectura).

    sudo python3 rusd_flight_report.py \\
        /Library/PrivilegedHelperTools/org.nvmetrics.rmguard/rusd-flight.jsonl [rusd-flight.jsonl.1]

Por sesión del guard: salida, muestras, ventanas de anomalía y causas por fuente.
Por trip: fuente y causa, timestamps brutos alrededor y si la fuente se recuperó
tras AUTO. Al final, una lectura del origen con su grado de certeza.

Las cifras salen de session_summary.causes (cuenta TODAS las muestras de la
sesión). Las muestras volcadas solo son ventanas de ejemplo: nunca se usan para
concluir que algo no ocurrió. No contacta con el servicio, el driver ni RM, y no
escribe nada.

Limitación de diseño (0.15.7): si el guard no pudo verificar AUTO no escribe
nada en este log (el rescate independiente no debe esperar). Esas sesiones solo
constan en controller-events.jsonl (auto_failed). Por eso un log vacío o sin
sesiones NUNCA se interpreta como "sin anomalías" (0.15.8): es no concluyente
y el programa termina con código 1.
"""
import json
import sys
from collections import Counter, defaultdict

NOT_ANOMALY = ('ok', 'ts_warmup')

# Lo que cada causa permite afirmar, y con qué certeza. Nada aquí es una
# conclusión sobre el incidente original: sólo sobre lo que muestran los datos.
CAUSES = {
    'ts_zero_producer_invalid': ('productor', 'alta',
        'el timestamp vivo valía 0 al leer: el productor (driver/GSP) retiró o no publicó la sección. '
        'NVMetrics nunca escribe ese valor.'),
    # Since 0.15.8 a WIP read always ends with one of the retry endings below; these two remain as the
    # final cause only in 0.15.7 records, whose retry slept and may have made no retry at all.
    'write_in_progress': ('no atribuible (registro 0.15.7)', 'baja',
        'el timestamp vivo estaba en WRITE_IN_PROGRESS al terminar la última lectura. Con el reintento de '
        '0.15.7 (nanosleep) puede no haber habido ninguna segunda observación; no prueba una escritura larga.'),
    'seqlock_timeout': ('no atribuible en exclusiva (registro 0.15.7)', 'baja',
        'la lectura coincidió con una publicación y se agotaron los intentos; el timestamp vivo era válido '
        'justo después. Interviene la estrategia de lectura (consumidor) y la duración de la escritura '
        '(productor); estos datos no separan una de otra.'),
    'ts_frozen': ('productor (probable)', 'media',
        'timestamp válido que no avanzó en >= 3 s: la sección dejó de publicarse (o cambió el intervalo '
        'de polling).'),
    'seq_marker_abi': ('ABI / layout', 'media',
        'una sección polled llevó un marcador de secuencia: posible desajuste de layout; compara '
        'session_begin (sizeof/offsets) con el kext.'),
    'read_inconsistent': ('no atribuible', 'baja', 'resultado de lectura y timestamp copiado no concuerdan.'),
    # 0.15.8: final causes of a bounded retry path that ended without a valid copy
    'producer_write_persistent': ('productor', 'alta',
        'WRITE_IN_PROGRESS en CADA observación durante >= 2 ms, sin ninguna interrupción del planificador: el '
        'productor mantuvo la escritura al menos ese tiempo.'),
    'scheduler_preempted': ('planificador de macOS (observación interrumpida)', 'media',
        'el guard estuvo desplanificado >= 500 us dentro del reintento: no se observó al productor durante ese '
        'tiempo, así que no puede atribuírsele aunque se viera WIP antes y después.'),
    'retry_budget_exhausted': ('no atribuible en exclusiva', 'baja',
        'durante 2 ms de observación continua el timestamp vivo siguió cambiando (no siempre WIP); ninguna '
        'copia fue coherente y no se aceptó ninguna.'),
    'ts_backwards': ('no atribuible', 'baja', 'un timestamp válido retrocedió (reinicio del productor o copia anómala).'),
    'value_out_of_range': ('dato del sensor', 'media', 'timestamp válido pero valor físicamente imposible.'),
    'clock_anomaly': ('reloj del guard', 'media', 'el reloj monotónico del guard retrocedió.'),
}


def load(paths):
    rows = []
    for path in paths:
        with open(path, encoding='utf-8') as f:
            for n, line in enumerate(f, 1):
                try:
                    obj = json.loads(line)
                except ValueError:
                    print(f'{path}:{n}: no es JSON, se ignora', file=sys.stderr)
                    continue
                if obj.get('rec') == 'rusd_fr':
                    rows.append(obj)
    return rows


def sample_causes(samples):
    """Fallback only (session without summary): count each sequence number once."""
    seen, causes = set(), defaultdict(Counter)
    for s in samples:
        if s['seq'] in seen or s.get('phase') == 'post_auto':
            continue
        seen.add(s['seq'])
        for src, d in s['src'].items():
            causes[src][d['cause']] += 1
    return causes


def describe_session(items):
    begin = next((r for r in items if r['kind'] == 'session_begin'), {})
    summary = next((r for r in items if r['kind'] == 'session_summary'), None)
    samples = [r for r in items if r['kind'] == 'sample']
    out = [f'  layout={begin.get("rusd_layout")} sizeof={begin.get("sizeof_shared_data")} '
           f'off_temperatures={begin.get("off_temperatures")} off_fan={begin.get("off_fan")}']
    if summary:
        causes = summary.get('causes', {})
        fa = summary.get('first_anomaly') or {}
        out.append(f'  salida={summary["exit_code"]} ({summary["why"]}) muestras={summary["samples"]} '
                   f'ventanas={summary.get("windows")} conservadas={summary.get("windows_kept")} '
                   f'sobrescritas={summary.get("windows_overwritten")} caminos_reintento={summary.get("retry_paths", "?")} '
                   f'con_reintentos={summary["seqlock_retried"]} recuperados={summary["seqlock_retry_recovered"]} '
                   f'retry_max_us={summary["retry_max_us"]} interrumpidos={summary.get("preempted_paths", "?")} '
                   f'max_gap_us={summary.get("max_gap_us", "?")} primera={fa.get("src")}:{fa.get("cause")}')
        recovered = summary['seqlock_retry_recovered']
        zero = max(0, summary.get('retry_paths', summary['seqlock_retried']) - summary['seqlock_retried'])
        complete = True
    else:
        causes = sample_causes(samples)
        recovered = sum(1 for s in samples for d in s['src'].values()
                        if d.get('retries') and d['cause'] == 'ok')
        zero = len({s['seq'] for s in samples for d in s['src'].values()
                    if d.get('retry_us') and not d.get('retries')})
        complete = False
        out.append('  SIN session_summary: guard terminado sin resumen o log rotado. Cifras parciales, '
                   'tomadas de las muestras volcadas; la ausencia de una causa NO prueba que no ocurriera.')
    for src, counts in sorted(causes.items()):
        bad = {c: n for c, n in counts.items() if c not in NOT_ANOMALY}
        if bad:
            out.append(f'    {src}: ' + ', '.join(f'{c}={n} muestras' for c, n in sorted(bad.items())))
    for ev in (r for r in items if r['kind'] == 'guard_event'):
        if ev['event'] in ('termination', 'auto_verified', 'rm_readback_failed', 'rm_write_failed', 'entry_refused'):
            out.append(f'  evento {ev["event"]} pct={ev["pct"]} motivo={ev["reason"]} seq={ev["seq"]}')
    for b in (r for r in items if r['kind'] == 'dump_begin' and r['segment'] == 'trip'):
        src, seq = b['trigger_src'], b['trigger_seq']
        out.append(f'  TRIP {src}:{b["trigger_cause"]} en seq={seq} ({b["why"]})')
        for s in (s for s in samples if s['segment'] == 'trip' and seq - 3 <= s['seq'] <= seq):
            d = s['src'][src]
            out.append(f'    seq={s["seq"]} fase={s["phase"]} conf_pct={s["conf_pct"]} {src}.cause={d["cause"]} '
                       f'ts={d["ts"]} ts_s64={d["ts_s64"]} vivo_antes={d["live_before"]} vivo_despues={d["live_after"]} '
                       f'age_ms={d["age_ms"]} reintentos={d.get("retries", 0)} retry_us={d.get("retry_us", 0)} '
                       f'fin={d.get("retry_end", "-")} max_gap_us={d.get("max_gap_us", "-")}')
        post = [s for s in samples if s['segment'] == 'post_trip']
        if post:
            ok = sum(1 for s in post if s['src'][src]['cause'] == 'ok')
            out.append(f'    tras AUTO verificado: {src} correcta en {ok}/{len(post)} muestras (~{len(post) * 250} ms); '
                       + ('se recuperó: pérdida transitoria.' if ok else 'no se recuperó en ese intervalo.'))
    if zero:
        out.append(f'  ATENCIÓN: {zero} camino(s) de reintento terminaron SIN ningún reintento (firma del 18-nov: '
                   'el nanosleep de 0.15.7 agotó el presupuesto antes del primer reintento).')
    return out, causes, recovered, complete, zero


def verdict(total, recovered, incomplete, sessions=1, zero=0):
    lines = []
    found = {c: n for c, n in total.items() if c not in NOT_ANOMALY}
    observed = sum(total.values())
    if not sessions or not observed:
        # 0.15.8: no data is never "no anomalies" (an unverified-AUTO or still-running session writes nothing)
        return ['No concluyente: no hay ninguna muestra registrada. Un log vacío no indica ausencia de fallos: '
                'una sesión en curso aún no ha escrito y una con AUTO no verificado no escribe aquí.']
    if not found and not recovered:
        if incomplete:
            return ['Sin anomalías en las muestras disponibles, pero hay sesiones sin resumen: no concluyente.']
        return [f'Sin anomalías RUSD registradas en {observed} muestras de sesiones con resumen.']
    if zero:
        lines.append(f'{zero} camino(s) de reintento sin ningún reintento: la lectura no se repitió, así que esos fallos '
                     'no informan sobre el productor (comportamiento corregido en 0.15.8).')
    if recovered:
        lines.append(f'{recovered} lectura(s) chocaron con una publicación y se recuperaron con la relectura '
                     'acotada. Confirma que hay colisiones de lectura con este productor; no indica por sí '
                     'solo de qué lado está el fallo original.')
    for cause, n in sorted(found.items(), key=lambda kv: -kv[1]):
        origin, certainty, text = CAUSES.get(cause, ('desconocido', 'baja', 'causa no documentada'))
        lines.append(f'{cause}: {n} muestras. Origen: {origin} (certeza {certainty}). {text}')
    if incomplete:
        lines.append('Hay sesiones sin resumen: las cifras pueden ser incompletas.')
    certain = {c for c in found if CAUSES.get(c, ('', 'baja'))[1] == 'alta'}
    if not certain:
        lines.append('Ninguna causa observada identifica el origen con certeza alta: hacen falta más sesiones o '
                     'datos del productor.')
    return lines


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip()); return 2
    rows = load(argv[1:])
    sessions = defaultdict(list)
    for r in rows:
        sessions[(r.get('run'), r.get('pid'))].append(r)
    total = Counter(); recovered = 0; incomplete = False; zero = 0
    for (run, pid), items in sessions.items():
        print(f'sesión run={run} pid={pid}')
        out, causes, rec, complete, z = describe_session(items)
        zero += z
        print('\n'.join(out))
        for counts in causes.values():
            total.update(counts)
        recovered += rec
        incomplete |= not complete
    if not sessions:
        print('Log vacío o sin registros rusd_fr.')
    lines = verdict(total, recovered, incomplete, len(sessions), zero)
    print('\nLectura (solo lo que permiten estos datos):\n  ' + '\n  '.join(lines))
    return 1 if lines[0].startswith('No concluyente') else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
