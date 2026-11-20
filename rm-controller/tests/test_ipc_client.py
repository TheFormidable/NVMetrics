import socket
import threading
import time
import unittest
from unittest.mock import patch

from nvmetrics_ipc_v08 import request


class IPCClientTests(unittest.TestCase):
    def exchange(self, writer, timeout=1):
        # Connected local socket pair: exercise real send/recv deadlines without
        # binding a filesystem socket or starting any installed service.
        with_socket, peer = socket.socketpair()
        class Connected:
            def __enter__(self): return self
            def __exit__(self, *_): with_socket.close()
            def connect(self, _): pass
            def __getattr__(self, name): return getattr(with_socket, name)
        with with_socket, peer:
            def worker():
                try:
                    peer.recv(1024)
                    writer(peer)
                except OSError:
                    pass
            thread = threading.Thread(target=worker)
            thread.start()
            try:
                with patch('nvmetrics_ipc_v08.socket.socket', return_value=Connected()):
                    return request('ping', path='/unused', verify_root=False, timeout=timeout)
            finally:
                thread.join(2)

    def test_valid_frame(self):
        reply = self.exchange(lambda c: c.sendall(b'{"v":1,"ok":true}\n'))
        self.assertTrue(reply['ok'])

    def test_bool_version_is_not_integer(self):
        with self.assertRaisesRegex(ValueError, 'invalid_response_version'):
            self.exchange(lambda c: c.sendall(b'{"v":true,"ok":true}\n'))

    def test_slow_response_cannot_renew_deadline(self):
        def trickle(conn):
            for _ in range(20):
                conn.sendall(b'x')
                time.sleep(.035)
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.exchange(trickle, timeout=.15)
        self.assertLess(time.monotonic() - started, .55)
