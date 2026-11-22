"""Resolve developer inputs without changing either source tree."""
import argparse
import hashlib
import os
from pathlib import Path
import sys

EXPECTED = {
    'app/Sources/FanGuardUI.swift': '2df1d09ba2cde435c8f8358bf349ed1e61fcc875ded093e6be295c898fa166c8',
    'agent/fan_guard_core_v04.h': '185a585a4485a747d673ae697b46015c08141d16127bfa67bdbfc01146963810',
    'agent/fan_rm_guard_v04.h': '85229f84547393c59469571f3dcecc98185d0cecfdde06f4bf3dcd53f2e36744',
}


def source_error(kind, path):
    if kind == 'repo':
        if not os.access(path / 'app/build_app.sh', os.X_OK) or not (path / 'app/build_app.sh').is_file():
            return 'missing executable app/build_app.sh (the overlay ZIP alone is not the integrated checkout)'
        for name, expected in EXPECTED.items():
            try:
                actual = hashlib.sha256((path / name).read_bytes()).hexdigest()
            except OSError:
                return f'missing or unreadable {name}'
            if actual != expected:
                return f'not exact v0.13.4 source: {name}'
    elif not path.is_dir():
        return 'driver source directory not found'
    else:
        for name in ('build/build_nvk.sh', 'nvk/ogkm-nvos-620.patch', 'nvk/ogkm-warnings.patch'):
            if not (path / name).is_file():
                return f'missing driver source file: {name}'
        if kind == 'driver-caps' and not (path / 'kexts/NVRM/nvrm_gpu_capabilities_abi.h').is_file():
            return 'GPU Caps ABI missing: use the compatible Phase2B checkout (~/gpu-caps-phase2B) as the second Build.command argument'
    return None


def resolve(kind, explicit):
    path = Path(explicit).expanduser().resolve()
    error = source_error(kind, path)
    if error:
        raise ValueError(f'{path}: {error}')
    return path


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('kind', choices=('repo', 'driver', 'driver-caps'))
    parser.add_argument('path')
    args = parser.parse_args()
    try:
        print(resolve(args.kind, args.path))
    except (OSError, ValueError) as exc:
        print(f'Source preflight: {exc}', file=sys.stderr)
        sys.exit(2)
