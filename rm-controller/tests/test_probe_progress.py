import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock

from backend import ReadOnlyAgent
from protocol import ProtocolError


class ProbeProgressTests(unittest.TestCase):
    def test_slow_read_only_probe_issues_progress_and_parses(self):
        with tempfile.TemporaryDirectory() as folder:
            agent=Path(folder)/'fake-agent'
            agent.write_text('#!/bin/sh\nread request\nsleep 0.7\nprintf \'%s\\n\' \'{"control_request_id":1,"control_fresh":true,"control_sources_advanced":true,"temp_gpu_c":55.8,"temp_mem_c":78,"fan_rpm":[1176,1173],"fan_live":{"temperature_fresh":true},"power_w":47.81,"util_gpu_pct":1}\'\n')
            agent.chmod(0o700)
            progress=Mock()
            backend=ReadOnlyAgent(agent);self.addCleanup(backend.close)
            sample=backend.call('status',timeout=3.5,progress=progress)
            self.assertEqual(sample['power_w'],47.81)
            self.assertGreaterEqual(progress.call_count,2)

    def test_probe_timeout_still_fails_closed(self):
        with tempfile.TemporaryDirectory() as folder:
            agent=Path(folder)/'fake-agent'
            agent.write_text('#!/bin/sh\nexec sleep 2\n')
            agent.chmod(0o700)
            with self.assertRaisesRegex(ProtocolError,'agent_unavailable'):
                ReadOnlyAgent(agent).call('status',timeout=.1,progress=Mock())
