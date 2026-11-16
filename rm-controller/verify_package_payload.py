"""Validate an expanded, locally built package without executing its contents."""
import json
import hashlib
import os
from pathlib import Path
import sys

from package_install import MARKER, tree_digest, tree_entries

REFERENCE = Path(__file__).resolve().parent / 'packaging/postinstall'


def archive_digest(archive):
    if not archive.is_file() or archive.is_symlink():
        raise RuntimeError('missing regular payload.zip (old package must be rebuilt)')
    with archive.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def render_postinstall(scripts, reference=REFERENCE):
    template = reference.read_bytes()
    marker = b'@PAYLOAD_SHA256@'
    if template.count(marker) != 1:
        raise RuntimeError('invalid postinstall template')
    return template.replace(marker, archive_digest(scripts / 'payload.zip').encode('ascii'))


def verify_envelope(scripts, reference=REFERENCE):
    postinstall = scripts / 'postinstall'
    if scripts.is_symlink() or postinstall.is_symlink():
        raise RuntimeError('unexpected Scripts/postinstall symlink')
    if not postinstall.is_file() or not os.access(postinstall, os.X_OK):
        raise RuntimeError('missing or non-executable postinstall')
    if postinstall.read_bytes() != render_postinstall(scripts, reference):
        raise RuntimeError('postinstall content or archive SHA-256 mismatch')


def verify(scripts, reference):
    payload = scripts / 'payload'
    postinstall = scripts / 'postinstall'
    manifest = payload / 'integrity.json'
    for path in (scripts, payload, postinstall, manifest):
        if path.is_symlink():
            raise RuntimeError(f'unexpected symlink: {path.name}')
    if not postinstall.is_file() or not os.access(postinstall, os.X_OK):
        raise RuntimeError('missing or non-executable postinstall')
    if postinstall.read_bytes() != reference.read_bytes():
        raise RuntimeError('unexpected postinstall content')
    verify_payload(payload)


def verify_payload(payload):
    manifest = payload / 'integrity.json'
    if payload.is_symlink() or manifest.is_symlink():
        raise RuntimeError('unexpected payload or integrity.json symlink')
    app = payload / 'NVMetrics.app'
    service = payload / 'service'
    expected = json.loads(manifest.read_text())
    actual = {'app': tree_digest(app), 'service': tree_digest(service)}
    if expected != actual:
        differences = ', '.join(f'{k}: expected {expected.get(k)}, actual {v}'
                                for k, v in actual.items() if expected.get(k) != v)
        raise RuntimeError('package integrity mismatch: ' + (differences or 'unexpected manifest keys'))
    resources = app / 'Contents/Resources'
    for executable in (resources / 'nvmetrics-agent', resources / 'GuardTools/Runtime/nvmetrics-runtime'):
        if not executable.is_file() or executable.is_symlink() or not os.access(executable, os.X_OK):
            raise RuntimeError(f'missing or non-executable: {executable.name}')
    if MARKER not in (resources / 'nvmetrics-agent').read_bytes():
        raise RuntimeError('unexpected native agent revision')


def compare_payload(original, extracted):
    old = dict(tree_entries(original))
    new = dict(tree_entries(extracted))
    differences = [(p, 'added' if p not in old else 'missing' if p not in new else 'changed')
                   for p in sorted(old.keys() | new.keys()) if old.get(p) != new.get(p)]
    if differences:
        for path, change in differences[:30]:
            print(f'{change}: {path}', file=sys.stderr)
        raise RuntimeError(f'native ZIP round-trip changed {len(differences)} tree entries')


if __name__ == '__main__':
    try:
        command, path, *rest = sys.argv[1:]
        path = Path(path)
        if command == 'render':
            script = path / 'postinstall'
            script.write_bytes(render_postinstall(path))
            script.chmod(0o755)
        elif command == 'envelope':
            verify_envelope(path)
            print('PASS: postinstall and payload.zip SHA-256')
        elif command == 'payload':
            verify_payload(path)
            print('PASS: native agent, frozen runtime, app and service digests')
        elif command == 'compare':
            compare_payload(path, Path(rest[0]))
            print('PASS: every payload tree entry matches original')
        else:
            raise RuntimeError('unknown verifier operation')
    except (OSError, ValueError, RuntimeError) as exc:
        sys.exit(f'Package verification FAILED: {exc}')
