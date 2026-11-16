import json
from pathlib import Path
import tempfile
import unittest

from package_install import MARKER, tree_digest
from verify_package_payload import verify, render_postinstall, verify_envelope, verify_payload, compare_payload


class VerifyArchiveTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.scripts = Path(self.tmp.name) / 'Scripts'
        self.payload = self.scripts / 'payload'
        self.app = self.payload / 'NVMetrics.app'
        self.service = self.payload / 'service'
        self.runtime = self.app / 'Contents/Resources/GuardTools/Runtime/nvmetrics-runtime'
        self.agent = self.app / 'Contents/Resources/nvmetrics-agent'
        self.runtime.parent.mkdir(parents=True)
        self.service.mkdir()
        self.reference = Path(self.tmp.name) / 'reference'
        self.reference.write_bytes(b'#!/bin/bash\n')
        self.postinstall = self.scripts / 'postinstall'
        for file, contents in ((self.runtime, b'frozen runtime'), (self.agent, MARKER),
                               (self.postinstall, self.reference.read_bytes())):
            file.write_bytes(contents)
            file.chmod(0o755)
        (self.service / 'server.py').write_text('server')
        self.write_manifest()

    def write_manifest(self):
        (self.payload / 'integrity.json').write_text(json.dumps({
            'app': tree_digest(self.app), 'service': tree_digest(self.service)}))

    def test_valid_expansion(self):
        verify(self.scripts, self.reference)

    def test_modified_service(self):
        (self.service / 'server.py').write_text('changed')
        with self.assertRaisesRegex(RuntimeError, 'integrity mismatch'):
            verify(self.scripts, self.reference)

    def test_nonexecutable_runtime(self):
        self.runtime.chmod(0o644)
        with self.assertRaisesRegex(RuntimeError, 'non-executable'):
            verify(self.scripts, self.reference)

    def test_wrong_postinstall(self):
        self.postinstall.write_text('unexpected')
        with self.assertRaisesRegex(RuntimeError, 'postinstall content'):
            verify(self.scripts, self.reference)

    def test_wrong_agent_marker(self):
        self.agent.write_bytes(b'wrong revision')
        self.write_manifest()
        with self.assertRaisesRegex(RuntimeError, 'agent revision'):
            verify(self.scripts, self.reference)

    def test_envelope_roundtrip(self):
        import shutil
        archive = self.scripts / 'payload.zip'
        archive.write_bytes(b'PK\x03\x04 opaque signed payload')
        self.postinstall.write_bytes(render_postinstall(self.scripts))
        verify_envelope(self.scripts)
        expanded = Path(self.tmp.name) / 'expanded'
        expanded.mkdir()
        shutil.copy2(archive, expanded / archive.name)
        shutil.copy2(self.postinstall, expanded / 'postinstall')
        verify_envelope(expanded)
        verify_payload(self.payload)

    def test_modified_archive_rejected(self):
        archive = self.scripts / 'payload.zip'
        archive.write_bytes(b'original')
        self.postinstall.write_bytes(render_postinstall(self.scripts))
        archive.write_bytes(b'changed')
        with self.assertRaisesRegex(RuntimeError, 'archive SHA-256 mismatch'):
            verify_envelope(self.scripts)

    def test_missing_archive_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'old package must be rebuilt'):
            verify_envelope(self.scripts)

    def test_archive_symlink_rejected(self):
        (self.scripts / 'payload.zip').symlink_to(self.reference)
        with self.assertRaisesRegex(RuntimeError, 'missing regular payload.zip'):
            verify_envelope(self.scripts)

    def test_tree_comparison_includes_internal_symlink(self):
        import shutil
        (self.app / 'agent-link').symlink_to('Contents/Resources/nvmetrics-agent')
        copy = Path(self.tmp.name) / 'copied'
        shutil.copytree(self.payload, copy, symlinks=True)
        compare_payload(self.payload, copy)
        (copy / 'NVMetrics.app/agent-link').unlink()
        with self.assertRaisesRegex(RuntimeError, 'changed 1 tree entries'):
            compare_payload(self.payload, copy)
