#!/usr/bin/env python3
"""Run actual backend captures, bit-exact repeat check, and real-effect negative control.
No reference or threshold is modified during normal verification.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
from PIL import Image
NAMES = ['rt_shadows_pole','rt_restir_many_lights','rt_ao_corner','rt_reflections_mirror_rough','rt_gi_cornell','rt_refraction_glass','rt_foliage_shadow']
p = argparse.ArgumentParser()
p.add_argument('--exe', required=True)
p.add_argument('--backend',default='metal',choices=['metal','vulkan','d3d12'])
p.add_argument('--out',required=True)
p.add_argument('--refs',required=True)
p.add_argument('--capture',action='store_true',help='Explicit local reference regeneration; refused by harness in CI')
a=p.parse_args()
out=Path(a.out).resolve();out.mkdir(parents=True,exist_ok=True)
results=[]
def run(name,folder,update=False,negative=False):
 env=dict(os.environ)
 if negative:env['DE_RT_GOLDEN_DISABLE_EFFECT']=negative
 cmd=[a.exe,'--backend',a.backend,'--scene',name,'--out',str(out/folder),'--refs',a.refs]
 if update:cmd+=['--update-refs']
 result=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 (out/f'{folder}-{name}.log').write_text(result.stdout)
 row=dict(scene=name,phase=folder,command=cmd,returncode=result.returncode,negative=bool(negative),comparison_failure=result.returncode==1 and bool(re.search(r'FAIL\s+.*: mean ',result.stdout)))
 results.append(row)
 print(f'{folder} {name}: {result.returncode}',flush=True)
 return row
for name in NAMES:
 for base in [False,True]:
  scene=name+('_base' if base else '')
  run(scene,'capture' if a.capture else 'normal',a.capture)
  run(scene,'repeat')
  tier='base' if base else 'rt'
  lhs=out/('capture' if a.capture else 'normal')/a.backend/tier/(scene+'.png')
  rhs=out/'repeat'/a.backend/tier/(scene+'.png')
  row=results[-1]
  if lhs.exists() and rhs.exists():
   l=Image.open(lhs).convert('RGBA').tobytes();r=Image.open(rhs).convert('RGBA').tobytes()
   row.update(pixel_sha256=hashlib.sha256(l).hexdigest(),repeat_sha256=hashlib.sha256(r).hexdigest(),different_bytes=sum(x!=y for x,y in zip(l,r)),identical=l==r)
  else:row['identical']=False
for scene,effect in [('rt_shadows_pole','shadows'),('rt_gi_cornell','gi'),('rt_refraction_glass','refraction')]:
 run(scene,'negative',negative=effect)
 restored=run(scene,'restored')
 first=out/('capture' if a.capture else 'normal')/a.backend/'rt'/(scene+'.png')
 last=out/'restored'/a.backend/'rt'/(scene+'.png')
 restored['restoration_identical']=first.exists() and last.exists() and Image.open(first).convert('RGBA').tobytes()==Image.open(last).convert('RGBA').tobytes()
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
failed=any(x['returncode'] not in (0,77) for x in results if x['phase']!='negative')
failed|=any(not x['identical'] for x in results if x['phase']=='repeat')
failed|=any(not x['comparison_failure'] for x in results if x['phase']=='negative')
failed|=any(not x['restoration_identical'] for x in results if x['phase']=='restored')
raise SystemExit(1 if failed else 0)
