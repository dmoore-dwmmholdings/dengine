"""Matched diagnostic: only list-handle nonuniform marking differs; never merge."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng660-uniform';out.mkdir(exist_ok=True)
shader=root/'shaders/lighting/light_cull.slang';original=shader.read_bytes();summary={}
marked=subprocess.check_output(['git','show','280c51ad:shaders/lighting/light_cull.slang'])
needle=b'ResourceDescriptorHeap[NonUniformResourceIndex(pc.list)]'
assert marked.count(needle)==1
uniform=marked.replace(needle,b'ResourceDescriptorHeap[pc.list]')
names=['golden.d3d12.shadows_local_off']+['render_clustered_lights.clustered_lights.'+n+'_d3d12' for n in ['small_mixed_light_sets','clusters_assignment_cpu_ref']]
try:
 for label,source in [('nonuniform',marked),('uniform',uniform)]:
  shader.write_bytes(source);stage=out/label;stage.mkdir();image=build/'golden/d3d12/base/shadows_local_off.png';image.unlink(missing_ok=True)
  with (stage/'ctest.log').open('w') as log:
   p=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$',
    '--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576',
    '--test-output-size-failed','1048576','--output-junit',str(stage/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
  cases=E.parse(stage/'results.xml').findall('.//testcase');assert len(cases)==3 and {c.get('name') for c in cases}==set(names)
  failures=[]
  for c in cases:
   assert c.find('skipped') is None and c.get('status')!='notrun'
   t=c.findtext('system-out') or '';assert 'd3d12: enhanced barriers' in t and 'Microsoft Basic Render Driver' in t
   assert '[error]' not in t and '[fatal]' not in t
   if c.find('failure') is not None:failures.append(c.get('name'))
  assert image.is_file();shutil.copy2(image,stage/image.name);(stage/'light_cull.slang').write_bytes(source)
  summary[label]={'exit':p.returncode,'cases':3,'failures':failures,'shader_sha256':hashlib.sha256(source).hexdigest(),'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest()}
  (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,summary[label],flush=True)
finally:shader.write_bytes(original)
assert summary['nonuniform']['image_sha256']=='e5cf18725982816f2cff0868b3b331b77ea77cef8f6949853d333c9780c70393'
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(any(v['failures'] for v in summary.values())))
