"""Read-only benchmark; no fan/boost/Eco writes. Pass the candidate agent path."""
import json,statistics,sys,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'service'))
from backend import ReadOnlyAgent
agent=ReadOnlyAgent(Path(sys.argv[1]))
try:
 old=[];new=[]
 for _ in range(3):
  t=time.monotonic();agent.call('status');old.append(time.monotonic()-t)
 t=time.monotonic();agent.call('status',progress=lambda:None);warm=time.monotonic()-t
 for _ in range(12):
  time.sleep(.45)
  t=time.monotonic();s=agent.call('status',progress=lambda:None);new.append(time.monotonic()-t)
 result={'one_shot_seconds':old,'reader_warmup_seconds':warm,'reader_seconds':new,
         'one_shot_median':statistics.median(old),'reader_median':statistics.median(new),'last_sample':s,
         'hardware_writes':False}
 print(json.dumps(result,indent=2))
finally:agent.close()
