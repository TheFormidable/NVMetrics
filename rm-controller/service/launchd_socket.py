"""macOS launchd socket activation only (no network).

launchd owns the UNIX socket lifetime; the privileged service must neither
create nor unlink the endpoint while launched on demand.
"""
from __future__ import annotations
import ctypes
import errno
import os
import socket
import sys

def validate_listener(listener, path):
    """Validate the inherited endpoint and return any already queued client.

    Darwin can reject SO_ACCEPTCONN on AF_UNIX with ENOPROTOOPT. In that
    case, a nonblocking accept is the listening-state check: EAGAIN means
    an empty listen queue; a successful accept must be retained for dispatch.
    Never call listen() to repair an invalid descriptor.
    """
    if (listener.family != socket.AF_UNIX or
            listener.getsockopt(socket.SOL_SOCKET, socket.SO_TYPE) != socket.SOCK_STREAM or
            listener.getsockname() != str(path)):
        raise RuntimeError('launchd_listener_identity_failed')
    opt = getattr(socket, 'SO_ACCEPTCONN', None)
    if opt is not None:
        try:
            listening = listener.getsockopt(socket.SOL_SOCKET, opt)
        except OSError as exc:
            if sys.platform != 'darwin' or exc.errno != errno.ENOPROTOOPT:
                raise
        else:
            if listening != 1:
                raise RuntimeError('launchd_listener_not_listening')
            return None
    elif sys.platform != 'darwin':
        raise RuntimeError('launchd_listener_validation_unavailable')
    timeout = listener.gettimeout()
    listener.setblocking(False)
    try:
        try:
            client, _ = listener.accept()
        except BlockingIOError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                raise
            return None
        return client
    finally:
        listener.settimeout(timeout)

def activate_socket(name: str):
    if sys.platform != 'darwin':raise RuntimeError('launchd_only_on_macos')
    libc=ctypes.CDLL(None,use_errno=True)
    fn=libc.launch_activate_socket
    fn.argtypes=[ctypes.c_char_p,ctypes.POINTER(ctypes.POINTER(ctypes.c_int)),
                 ctypes.POINTER(ctypes.c_size_t)]
    fn.restype=ctypes.c_int
    ptr=ctypes.POINTER(ctypes.c_int)()
    count=ctypes.c_size_t()
    rc=fn(name.encode('ascii'),ctypes.byref(ptr),ctypes.byref(count))
    if rc:raise OSError(rc,os.strerror(rc))
    try:
        if count.value!=1:raise RuntimeError('expected_one_launchd_socket')
        fd=int(ptr[0])
        return socket.socket(fileno=fd)
    finally:
        libc.free.argtypes=[ctypes.c_void_p]
        libc.free.restype=None
        libc.free(ptr)
