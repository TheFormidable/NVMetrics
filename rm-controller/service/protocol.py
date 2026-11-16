"""Strict v1 newline-JSON protocol. Physical actions require server-side opt-in.
This file never executes hardware operations.
"""
from __future__ import annotations
import json
import math

PROTOCOL=1
MAX_REQUEST_BYTES=1024
ALLOWED={'ping','status','inspect','auto_observe','control_status','manual_begin','manual_raise','manual_lower','manual_heartbeat','manual_auto'}
SESSIONS={'manual_raise','manual_lower','manual_heartbeat','manual_auto'}

# Conservative *experimental* minimum for both physical tachometers; not a
# measured fan speed curve. A positive RM readback alone is never sufficient.
MANUAL_FLOOR=30
def rpm_floor(pct):return 1000

class ProtocolError(ValueError): pass

def decode_request(raw: bytes) -> dict:
    if not isinstance(raw, bytes) or len(raw)>MAX_REQUEST_BYTES or not raw.endswith(b'\n') or raw.count(b'\n')!=1:
        raise ProtocolError('invalid_frame')
    def unique(pairs):
        d={}
        for k,v in pairs:
            if k in d: raise ValueError('duplicate_json_key')
            d[k]=v
        return d
    try: o=json.loads(raw[:-1].decode('utf8','strict'),object_pairs_hook=unique)
    except (ValueError,UnicodeDecodeError): raise ProtocolError('invalid_json') from None
    if type(o) is not dict or type(o.get('v')) is not int or o['v']!=PROTOCOL:
        raise ProtocolError('invalid_fields_or_version')
    op=o.get('op')
    if type(op) is not str or op not in ALLOWED: raise ProtocolError('operation_not_allowed')
    allowed={'v','op'}
    if op in SESSIONS:
        allowed.add('session')
        s=o.get('session')
        if type(s) is not str or len(s)!=32 or any(c not in '0123456789abcdef' for c in s):
            raise ProtocolError('invalid_session')
    if op in ('manual_raise','manual_lower'):
        allowed.add('pct')
        pct=o.get('pct')
        if op=='manual_raise' and (type(pct) is not int or pct not in range(35,101,5)):
            raise ProtocolError('invalid_raise_target')
        if op=='manual_lower' and (type(pct) is not int or pct not in range(MANUAL_FLOOR,100,5)):
            raise ProtocolError('invalid_lower_target')
    if set(o)!=allowed: raise ProtocolError('invalid_fields_or_version')
    return o

def parse_agent(output: str, operation: str) -> dict:
    obj=None
    for line in reversed(output.splitlines()):
        try: candidate=json.loads(line)
        except ValueError: continue
        if type(candidate) is dict: obj=candidate;break
    if obj is None: raise ProtocolError('agent_not_json')
    if operation=='status':
        gpu,mem,rpm=obj.get('temp_gpu_c'),obj.get('temp_mem_c'),obj.get('fan_rpm')
        if not all(type(x) in (int,float) and math.isfinite(x) for x in (gpu,mem)):
            raise ProtocolError('temperature_not_fresh')
        if not (rpm==[] or (type(rpm) is list and len(rpm)>=2 and all(type(x) is int and x>=0 for x in rpm))):
            raise ProtocolError('rpm_not_available')
        if (obj.get('fan_live') or {}).get('temperature_fresh') is not True:
            raise ProtocolError('telemetry_not_fresh')
        power=obj.get('power_w');util=obj.get('util_gpu_pct')
        power=power if type(power) in (int,float) and math.isfinite(power) else None
        util=util if type(util) is int and 0<=util<=100 else None
        result={'gpu_c':gpu,'mem_c':mem,'rpm':rpm,'fresh':True,'power_w':power,'util_gpu_pct':util}
        for key,limit in (('pstate',15),('clock_gpu_mhz',10000)):
            value=obj.get(key)
            if type(value) is int and 0<=value<=limit:result[key]=value
        return result
    if operation=='inspect':
        if obj.get('operation')!='rm-snapshot' or obj.get('get_regular_status')!=0 or obj.get('get_default_status')!=0:
            raise ProtocolError('rm_get_failed')
        blocks={}
        for variant in ('regular','default'):
            b=obj.get(variant)
            if type(b) is not dict or b.get('layout_matches_two') is not True:
                raise ProtocolError('layout_unrecognized')
            recs=b.get('records')
            if type(recs) is not list or len(recs)!=2:raise ProtocolError('fan_count_unrecognized')
            pairs=[]
            for r in recs:
                if type(r) is not dict:raise ProtocolError('record_invalid')
                mode,q16=r.get('policy_word_candidate'),r.get('requested_q16_candidate')
                if type(mode) is not int or type(q16) is not int or mode not in (0,1) or not 0<=q16<=65536:
                    raise ProtocolError('rm_mode_invalid')
                pair={'mode':mode,'q16':q16,'percent':round(q16/65536*100,1)}
                lo,hi=r.get('rpm_min_candidate'),r.get('rpm_max_candidate')
                if type(lo) is int and type(hi) is int and 100<=lo<hi<=30000:
                    pair.update(rpm_min_candidate=lo,rpm_max_candidate=hi)
                pairs.append(pair)
            blocks[variant]=pairs
        auto=all(r['mode']==0 and r['q16']==0 for recs in blocks.values() for r in recs)
        manual=(all(r['mode']==1 and r['q16']>0 for r in blocks['regular']) and
                blocks['regular'][0]['q16']==blocks['regular'][1]['q16'] and
                all(r['mode']==0 and r['q16']==0 for r in blocks['default']))
        return {'mode':'auto' if auto else 'manual' if manual else 'unknown',**blocks}
    raise ProtocolError('unsupported_agent_operation')

def response(data:dict)->bytes:
    return (json.dumps({'v':PROTOCOL,**data},ensure_ascii=False,separators=(',',':'))+'\n').encode('utf8')
