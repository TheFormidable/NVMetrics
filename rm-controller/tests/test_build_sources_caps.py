import tempfile
import unittest
from pathlib import Path

import build_sources


class GPUCapsSourceTests(unittest.TestCase):
    def test_old_driver_is_rejected_for_candidate_but_available_for_tests(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)
            for name in ('build/build_nvk.sh', 'nvk/ogkm-nvos-620.patch', 'nvk/ogkm-warnings.patch'):
                f = path / name
                f.parent.mkdir(parents=True, exist_ok=True)
                f.touch()
            self.assertIsNone(build_sources.source_error('driver', path))
            self.assertIn('GPU Caps ABI missing', build_sources.source_error('driver-caps', path))
            header = path / 'kexts/NVRM/nvrm_gpu_capabilities_abi.h'
            header.parent.mkdir(parents=True)
            header.touch()
            self.assertIsNone(build_sources.source_error('driver-caps', path))
