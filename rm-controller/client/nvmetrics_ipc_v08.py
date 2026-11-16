#!/usr/bin/env python3
"""NVMetrics GUI/CLI IPC v0.8 client. Server must be root and is started separately.
Usage: nvmetrics_ipc_v08.py OP [SESSION] [PCT]. Returns one JSON line.
"""
from __future__ import annotations
import ctypes
import json
import os
import socket
import sys

SOCKET='/Library/PrivilegedHelperTools/org.nvmetrics.rmguard/control.sock'
OPS={'ping','status','inspect','auto_observe','control_status','manual_begin','manual_raise','manual_lower','manual_heartbeat','manual_auto'}

def root_peer(conn):
    if sys.platform=='darwin':
        f=ctypes.CDLL(None,use_errno=True).getpeereid
        f.argtypes=[ctypes.c_int,ctypes.POINTER(ctypes.c_uint),ctypes.POINTER(ctypes.c_uint)]
        f.restype=ctypes.c_int
        uid=ctypes.c_uint();gid=ctypes.c_uint()
        if f(conn.fileno(),ctypes.byref(uid),ctypes.byref(gid))!=0:raise OSError('getpeereid failed')
        return uid.value==0
    if sys.platform.startswith('linux'):
        import struct
        return struct.unpack('3i',conn.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))[1]==0
    return False

def request(op,session=None,pct=None,path=SOCKET,verify_root=True):
    if op not in OPS:raise ValueError('operation_not_allowed')
    body={'v':1,'op':op}
    if op in {'manual_raise','manual_lower','manual_heartbeat','manual_auto'}:
        if type(session) is not str or len(session)!=32:raise ValueError('invalid_session')
        body['session']=session
    if op in ('manual_raise','manual_lower'):
        allowed=range(35,101,5) if op=='manual_raise' else range(30,100,5)
        if type(pct) is not int or pct not in allowed:raise ValueError('invalid_adjust_target')
        body['pct']=pct
    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as s:
        # Physical steps contain bounded native SET/readbacks and tach probes.
        # Give responses time to arrive; the independent guard/lease deadlines
        # are unchanged and still terminate unsafe or abandoned operations.
        s.settimeout(65 if op=='manual_lower' else 35 if op=='manual_begin' else 12);s.connect(path)
        if verify_root and not root_peer(s):raise PermissionError('server_is_not_root')
        s.sendall((json.dumps(body,separators=(',',':'))+'\n').encode('utf8'))
        data=b''
        while b'\n' not in data and len(data)<65536:
            b=s.recv(4096)
            if not b:break
            data+=b
        if not data.endswith(b'\n') or len(data)>65536 or data.count(b'\n')!=1:
            raise ValueError('invalid_response_frame')
        r=json.loads(data)
        if type(r) is not dict or r.get('v')!=1:raise ValueError('invalid_response_version')
        return r

def main():
    if len(sys.argv)<2 or sys.argv[1] not in OPS:
        print('usage: nvmetrics_ipc_v08.py '+ '|'.join(sorted(OPS))+' [SESSION] [PCT]',file=sys.stderr);return 2
    op=sys.argv[1];expect=4 if op in ('manual_raise','manual_lower') else 3 if op in {'manual_auto','manual_heartbeat'} else 2
    if len(sys.argv)!=expect:
        print('invalid arguments',file=sys.stderr);return 2
    try:
        session=sys.argv[2] if expect>=3 else None
        pct=int(sys.argv[3]) if expect==4 else None
        result=request(op,session,pct)
    except (OSError,ValueError,PermissionError,TypeError) as e:
        result={'v':1,'ok':False,'error':str(e)}
    print(json.dumps(result,ensure_ascii=False,separators=(',',':')))
    return 0 if result.get('ok') is True else 3

if __name__=='__main__':raise SystemExit(main())
