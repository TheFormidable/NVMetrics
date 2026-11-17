import os
import socket
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import server
import physical


class AuditHardeningTests(unittest.TestCase):
    def test_slow_trickle_cannot_renew_deadline(self):
        a,b=socket.socketpair()
        def trickle():
            try:
                for _ in range(12):
                    b.sendall(b'x');time.sleep(.04)
            except OSError:pass
        thread=threading.Thread(target=trickle)
        with a,b:
            thread.start();started=time.monotonic()
            with self.assertRaises(TimeoutError):server.read_frame(a,.15)
            self.assertLess(time.monotonic()-started,.4)
        thread.join()

    def test_complete_frame(self):
        a,b=socket.socketpair()
        with a,b:
            b.sendall(b'{}\n')
            self.assertEqual(server.read_frame(a),b'{}\n')

    def test_audit_rotates_and_preserves_permissions(self):
        with tempfile.TemporaryDirectory() as temp,patch.object(physical,'ROOT',Path(temp)):
            path=Path(temp)/'controller-events.jsonl'
            fd=os.open(path,os.O_WRONLY|os.O_CREAT,0o600)
            os.ftruncate(fd,4*1024*1024);os.close(fd)
            physical.Physical(Path('/fake'))._audit('test')
            self.assertEqual(path.stat().st_mode&0o777,0o600)
            self.assertIn('"event":"test"',path.read_text())
            self.assertEqual(path.with_suffix('.jsonl.1').stat().st_size,4*1024*1024)

    def test_absolute_manual_expiry_independent_of_client(self):
        p=physical.Physical(Path('/fake'));p.token='token';p.start_time=time.monotonic()-3601
        with patch.object(p,'_poll_child'),patch.object(p,'stop') as stop:
            p.check_client_lease();stop.assert_called_once_with('hard_deadline')
