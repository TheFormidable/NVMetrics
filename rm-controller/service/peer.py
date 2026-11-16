"""Kernel-attested peer identity. Never trust usernames or claimed UIDs in JSON."""
import ctypes
import os
import socket
import struct
import sys


def peer_uid(conn: socket.socket) -> int:
    if sys.platform == 'darwin':
        libc = ctypes.CDLL(None, use_errno=True)
        f = libc.getpeereid
        f.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
        f.restype = ctypes.c_int
        uid = ctypes.c_uint()
        gid = ctypes.c_uint()
        if f(conn.fileno(), ctypes.byref(uid), ctypes.byref(gid)) != 0:
            e = ctypes.get_errno()
            raise OSError(e, f'getpeereid failed: {os.strerror(e)}')
        return uid.value
    if sys.platform.startswith('linux'):
        raw = conn.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, struct.calcsize('3i'))
        return struct.unpack('3i', raw)[1]
    raise OSError('unsupported_peer_credential_platform')
