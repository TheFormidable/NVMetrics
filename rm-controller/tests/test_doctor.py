import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import doctor
import runtime


class DoctorTests(unittest.TestCase):
    def test_report_is_read_only_and_redacts_identifiers(self):
        with tempfile.TemporaryDirectory() as d:
            agent = Path(d) / 'agent'
            agent.touch()
            replies = {
                ('once',): [{'temp_gpu_c': 45.2, 'temp_mem_c': None, 'power_w': 0,
                             'gpu_profile': {'gpu_id': 123, 'tach_slot_mask': 0b10001},
                             'health': {'xid_count': 2, 'bugchecks': 0, 'rm_asserts': 0}, 'token': 'secret'}],
                ('caps',): [{'verdict': 'usable', 'manual_authorized': True, 'registry_id': 'secret'}],
                ('eco', 'status'): [{'eco': 'on'}], ('policy', 'status'): [{'policy': 'idle'}],
            }
            called = []
            def read(path, args):
                self.assertEqual(path, agent)
                called.append(args)
                return replies[args]
            report = doctor.build_report(agent, read=read, ping=lambda: {'ok': True, 'writes_enabled': False, 'authorized_uid': 500, 'session': 'secret'})
            checks = {c['name']: c for c in report['checks']}
            self.assertEqual(tuple(called), doctor.COMMANDS)
            self.assertEqual(checks['Temperatura VRAM']['status'], 'unknown')
            self.assertEqual(checks['Potencia']['status'], 'ok')
            self.assertEqual(checks['Errores Xid']['status'], 'warn')
            self.assertIn('0, 4', checks['Tacómetros']['detail'])
            self.assertEqual(checks['Control Manual']['detail'], 'Escrituras deshabilitadas.')
            self.assertNotIn('secret', json.dumps(report))
            self.assertNotIn('authorized_uid', json.dumps(report))

    def test_disconnected_and_malformed_are_not_success(self):
        def no_peer():
            raise TimeoutError()
        result = doctor.build_report(Path('/missing'), read=lambda *_: [], ping=no_peer)
        checks = {c['name']: c['status'] for c in result['checks']}
        self.assertEqual(checks['RUSD'], 'error')
        self.assertEqual(checks['GPU Caps ABI'], 'unknown')
        self.assertEqual(checks['Servicio RM'], 'warn')
        self.assertEqual(checks['Control Manual'], 'unknown')

    def test_non_read_only_agent_command_is_refused(self):
        for command in (('eco', 'on'), ('policy', 'idle'), ('fan', 'rm-set'), ('boost', 'hold')):
            with self.assertRaises(ValueError):
                doctor.read_agent(Path('/agent'), command)

    def test_runtime_dispatches_doctor_without_root(self):
        with patch('sys.argv', ['runtime', 'doctor']), patch('doctor.main', return_value=0) as main:
            self.assertEqual(runtime.main(), 0)
            main.assert_called_once_with()
