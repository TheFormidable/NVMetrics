"""Offline analysis of actual per-frame times; no GPU writes, injection or synthetic FPS."""
import csv
import io
import json
import math
import os
from pathlib import Path
import stat
import sys

MAX_BYTES = 16 * 1024 * 1024
MAX_FRAMES = 1_000_000


def frame_report(text):
    rows = csv.reader(io.StringIO(text))
    if next(rows, None) != ['frametime_ms']:
        raise ValueError('CSV debe tener una única columna frametime_ms (un valor por fotograma).')
    values = []
    for row in rows:
        if len(row) != 1 or len(values) >= MAX_FRAMES:
            raise ValueError('CSV inválido o demasiado grande.')
        value = float(row[0])
        if not math.isfinite(value) or not 0.001 <= value <= 60000:
            raise ValueError('Frametime inválido.')
        values.append(value)
    if not values:
        raise ValueError('No hay fotogramas.')
    values.sort()
    n = len(values)
    total = math.fsum(values)
    def percentile(p):
        return values[max(0, math.ceil(n * p) - 1)]
    def low(p):
        k = max(1, math.ceil(n * p))
        return 1000 * k / math.fsum(values[-k:])
    return {'schema': 1, 'source': 'imported_per_frame_ms', 'frames': n,
            'duration_s': total / 1000, 'fps': 1000 * n / total,
            'one_percent_low': low(.01), 'p95_ms': percentile(.95),
            'p99_ms': percentile(.99), 'median_ms': percentile(.5)}


def main():
    try:
        if len(sys.argv) != 2:
            raise ValueError('Uso: nvmetrics-runtime benchmark archivo.csv')
        # Only a bounded regular local file; no FIFOs/devices or blocking reads.
        fd = os.open(Path(sys.argv[1]), os.O_RDONLY | os.O_NONBLOCK)
        with os.fdopen(fd, 'rb') as f:
            info = os.fstat(f.fileno())
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_BYTES:
                raise ValueError('El archivo debe ser regular y de hasta 16 MiB.')
            data = f.read(MAX_BYTES + 1)
        if len(data) > MAX_BYTES:
            raise ValueError('Archivo demasiado grande.')
        print(json.dumps(frame_report(data.decode('utf-8-sig')), allow_nan=False))
        return 0
    except (OSError, ValueError, csv.Error, UnicodeError, OverflowError) as exc:
        print(str(exc), file=sys.stderr)
        return 2
