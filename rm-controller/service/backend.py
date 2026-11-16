"""Agent read-only adapter. No RM SET, no shell, no user-supplied executable paths."""
from __future__ import annotations
import json
import os
import select
import pathlib
import subprocess
import time
from protocol import ProtocolError, parse_agent


class ReadOnlyAgent:
    def __init__(self, agent: pathlib.Path):
        self.agent = str(agent)
        self.reader = None
        self.request_id = 0

    def close(self):
        reader,self.reader=self.reader,None
        if reader is None:return
        if reader.stdin:
            try:reader.stdin.close()
            except OSError:pass
        try:reader.wait(timeout=.5)
        except subprocess.TimeoutExpired:
            reader.kill();reader.wait(timeout=1)
        if reader.stdout:reader.stdout.close()

    def _session_sample(self,timeout,progress):
        """One fresh response per request; never serve a buffered/cached sample.
        Only used during a guarded physical transaction/session. No --control,
        boost or fan SET command exists on this reader's stdin protocol.
        """
        try:
            if self.reader is None:
                self.reader=subprocess.Popen([self.agent,'fan','sensor-read'],
                    stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0,
                    env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','HOME':'/var/root','LANG':'C','PYTHONNOUSERSITE':'1'})
            p=self.reader
            if p.poll() is not None:raise ProtocolError('sensor_reader_exited')
            self.request_id+=1
            p.stdin.write(('SAMPLE '+str(self.request_id)+'\n').encode('ascii'))
            p.stdin.flush()
            data=b'';deadline=time.monotonic()+min(6,timeout)
            while b'\n' not in data:
                progress()
                remaining=deadline-time.monotonic()
                if remaining<=0:raise ProtocolError('agent_unavailable')
                ready,_,_=select.select([p.stdout],[],[],min(.25,remaining))
                if not ready:continue
                chunk=os.read(p.stdout.fileno(),4096)
                if not chunk:raise ProtocolError('sensor_reader_exited')
                data+=chunk
                if len(data)>65536:raise ProtocolError('agent_failed_or_oversized')
            if not data.endswith(b'\n') or data.count(b'\n')!=1:raise ProtocolError('sensor_reader_invalid_frame')
            try:obj=json.loads(data)
            except (ValueError,UnicodeError):raise ProtocolError('sensor_reader_invalid_json') from None
            if type(obj) is not dict or type(obj.get('control_request_id')) is not int or obj['control_request_id']!=self.request_id:
                raise ProtocolError('sensor_reader_response_mismatch')
            if obj.get('control_fresh') is not True or obj.get('control_sources_advanced') is not True:
                raise ProtocolError('telemetry_not_fresh')
            return parse_agent(data.decode('utf8'),'status')
        except (OSError,ProtocolError) as exc:
            self.close()
            if isinstance(exc,ProtocolError):raise
            raise ProtocolError('agent_unavailable') from None
        except BaseException:
            self.close();raise

    def call(self, operation: str, timeout: float = 6, progress=None) -> dict:
        if operation=='status' and progress is not None:
            return self._session_sample(timeout,progress)
        cmd = {'status': 'status', 'inspect': 'rm-inspect'}.get(operation)
        if cmd is None:
            raise ProtocolError('operation_not_allowed')
        try:
            deadline=time.monotonic()+min(6,timeout)
            with subprocess.Popen([self.agent,'fan',cmd],stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,
                                  env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','HOME':'/var/root',
                                       'LANG':'C','PYTHONNOUSERSITE':'1'}) as p:
                try:
                    while True:
                        remaining=deadline-time.monotonic()
                        if remaining<=0:raise subprocess.TimeoutExpired(cmd,timeout)
                        if progress is not None:progress()
                        try:
                            stdout,_=p.communicate(timeout=min(remaining,.5) if progress else remaining)
                            break
                        except subprocess.TimeoutExpired:
                            if time.monotonic()>=deadline:raise
                except BaseException:
                    p.kill();p.communicate();raise
        except (subprocess.TimeoutExpired, OSError):
            raise ProtocolError('agent_unavailable') from None
        if p.returncode != 0 or len(stdout) > 65536:
            raise ProtocolError('agent_failed_or_oversized')
        return parse_agent(stdout, operation)
