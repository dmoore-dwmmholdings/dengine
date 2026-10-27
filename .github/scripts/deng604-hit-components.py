"""CI-only factorial SSR history diagnostics; no reference/threshold updates."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/windows-clang-cl-debug';out=build/'deng604-hit-components'
files=['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang']
base={f:subprocess.check_output(['git','show','81743f90:'+f]).decode() for f in files}
candidate={f:(root/f).read_text() for f in files}
common,resolve=files
def variants():
 result={'main':base.copy(),'candidate':candidate.copy()}
 original=candidate[resolve]
 start=original.index('    return ssr_history_sample(pc, sc, pc.history_color,')
 end=original.index('\n}',start)
 tail=original[start:end]
 for label in ['unit-coverage','unfiltered-radiance','continuous-anchor']:
  r=original
  if label=='continuous-anchor':
   replacement=tail.replace('position, normal, geometric_normal, true)', 'ssr_position(sc, uv, depth), normal, geometric_normal, true)')
   assert replacement!=tail
  else:
   replacement=tail.replace('return ssr_history_sample(', 'float4 history = ssr_history_sample(',1)
   if label=='unit-coverage':replacement+='\n    if (history.a > 0) history.a = 1;'
   else:replacement+='\n    if (history.a > 0) history.rgb = ssr_history_load(pc.history_color, ssr_previous_uv(uv,motion), uint2(pc.width,pc.height)).rgb;'
   replacement+='\n    return history;'
  r=r[:start]+replacement+r[end:]
  result[label]={common:candidate[common],resolve:r}
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
 out.mkdir(exist_ok=True);summary={'product':'dac9650c','base':'81743f90','stages':{}};failed=False
 goldnames=['golden.d3d12.reflections_ssr_probes_'+q for q in ('medium','high')]
 oracle='render_ssr.ssr.history_hit_partial_coverage_d3d12'
 try:
  for label,sources in allvariants.items():
   directory=out/label;directory.mkdir(exist_ok=True)
   for f,text in sources.items():
    (root/f).write_bytes(text.encode());(directory/Path(f).name).write_bytes(text.encode())
   names=goldnames+([] if label=='main' else [oracle])
   with (directory/'ctest.log').open('w') as log:
    r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(directory/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
   cases=E.parse(directory/'results.xml').findall('.//testcase');errors=[]
   assert len(cases)==len(names) and {c.get('name') for c in cases}==set(names)
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
   summary['stages'][label]={'exit':r.returncode,'cases':len(cases),'failures':errors,'images':images,'sources':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files}}
   failed|=bool(r.returncode or errors)
   (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,json.dumps(summary['stages'][label]),flush=True)
 finally:
  for f,text in candidate.items():(root/f).write_bytes(text.encode())
  subprocess.run(['git','diff','--exit-code'],check=True)
 # Any original golden failures remain red diagnostics, never acceptance.
 sys.exit(int(failed))
if __name__=='__main__':main()
