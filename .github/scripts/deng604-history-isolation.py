"""CI-only factorial SSR history diagnostics; no reference/threshold updates."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/windows-clang-cl-debug';out=build/'deng604-history-isolation'
files=['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang']
base={f:subprocess.check_output(['git','show','81743f90:'+f]).decode() for f in files}
candidate={f:(root/f).read_text() for f in files}
common,resolve=files
legacy=base[common][base[common].index('public float4 ssr_history_sample('):base[common].index('public float3 ssr_response(')]
legacy=legacy.replace('public float4 ssr_history_sample(', 'public float4 ssr_history_sample_legacy(')
def variants():
 result={'main':base.copy(),'candidate':candidate.copy()}
 for hit,receiver,label in [(True,False,'legacy-hit'),(False,True,'legacy-receiver'),(True,True,'legacy-both')]:
  c=candidate[common].replace('public float3 ssr_response(',legacy+'public float3 ssr_response(',1)
  r=candidate[resolve]
  if hit:
   start=r.index('                // History is pre-exposed;');end=r.index('                if (!(history.a > 0))',start)
   a=base[resolve].index('                float2 prev = ssr_previous_uv(hit.xy');b=base[resolve].index('                if (!(history.a > 0))',a)
   block=base[resolve][a:b].replace('ssr_history_sample(', 'ssr_history_sample_legacy(')
   r=r[:start]+block+r[end:]
  if receiver:
   start=r.index('            if ((pc.history_valid & 2u)');end=r.index('                if (old.a > 0',start)
   a=base[resolve].index('            if ((pc.history_valid & 2u)');b=base[resolve].index('                if (old.a > 0',a)
   block=base[resolve][a:b].replace('ssr_history_sample(', 'ssr_history_sample_legacy(')
   r=r[:start]+block+r[end:]
  result[label]={common:c,resolve:r}
 return result

def main():
 allvariants=variants()
 # Offline preparation emits exact variant sources only, never starts a GPU executable.
 if len(sys.argv)==3 and sys.argv[1]=='--emit':
  target=Path(sys.argv[2]);target.mkdir(parents=True,exist_ok=True)
  for label,sources in allvariants.items():
   d=target/label;d.mkdir(exist_ok=True)
   for f,text in sources.items():(d/Path(f).name).write_bytes(text.encode())
  return
 out.mkdir(exist_ok=True);summary={'product':'1d869e05','base':'81743f90','stages':{}};failed=False
 names=['golden.d3d12.reflections_ssr_probes_'+q for q in ('medium','high')]
 try:
  for label,sources in allvariants.items():
   directory=out/label;directory.mkdir(exist_ok=True)
   for f,text in sources.items():
    (root/f).write_bytes(text.encode());(directory/Path(f).name).write_bytes(text.encode())
   with (directory/'ctest.log').open('w') as log:
    r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(directory/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
   cases=E.parse(directory/'results.xml').findall('.//testcase');errors=[]
   assert len(cases)==2 and {c.get('name') for c in cases}==set(names)
   for c in cases:
    text=c.findtext('system-out') or ''
    assert c.find('skipped') is None and c.find('error') is None and c.get('status') in ('run','fail')
    assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text
    assert '[error]' not in text and '[fatal]' not in text
    if c.find('failure') is not None:errors.append(c.get('name'))
   images={}
   for quality in ('medium','high'):
    name='reflections_ssr_probes_'+quality+'.png';p=build/'golden/d3d12/base'/name
    assert p.is_file();shutil.copy2(p,directory/name);images[name]=hashlib.sha256(p.read_bytes()).hexdigest()
   summary['stages'][label]={'exit':r.returncode,'failures':errors,'images':images,'sources':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}}
   failed|=bool(r.returncode or errors)
   (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,json.dumps(summary['stages'][label]),flush=True)
 finally:
  for f,text in candidate.items():(root/f).write_bytes(text.encode())
  subprocess.run(['git','diff','--exit-code'],check=True)
 # Any original golden failures remain red diagnostics, never acceptance.
 sys.exit(int(failed))
if __name__=='__main__':main()
