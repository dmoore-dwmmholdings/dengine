"""Diagnostic controls only. Never merge this workflow or shader mutations."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng660-heap-controls';out.mkdir(exist_ok=True)
shader=root/'shaders/lighting/light_cull.slang';original=shader.read_bytes();source=original.decode();summary={}
no_sort=source.replace('for (uint end = k; end > 1; --end)', 'for (uint end = 0; end > 1; --end)')
assert no_sort!=source
pattern=r'(de_buffer<uint>\(pc\.list\)\[[^;]+?\]\s*=\s*[^;]+;)'
barrier,count=re.subn(pattern,r'\1\n        DeviceMemoryBarrier();',source)
assert count==5,count
names=['golden.d3d12.shadows_local_off','render_clustered_lights.clustered_lights.small_mixed_light_sets_d3d12']
try:
 for label,text in [('original',source),('no_sort',no_sort),('barrier',barrier),('repeat_original',source)]:
  shader.write_text(text);stage=out/label;stage.mkdir()
  image=build/'golden/d3d12/base/shadows_local_off.png';image.unlink(missing_ok=True)
  with (stage/'ctest.log').open('w') as log:
   p=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$',
    '--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576',
    '--test-output-size-failed','1048576','--output-junit',str(stage/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
  cases=E.parse(stage/'results.xml').findall('.//testcase');assert {c.get('name') for c in cases}==set(names) and len(cases)==2
  failures=[]
  for c in cases:
   assert c.find('skipped') is None and c.get('status')!='notrun'
   t=c.findtext('system-out') or '';assert 'd3d12: enhanced barriers' in t and 'Microsoft Basic Render Driver' in t
   assert '[error]' not in t and '[fatal]' not in t
   if c.find('failure') is not None:failures.append(c.get('name'))
  assert image.is_file();shutil.copy2(image,stage/image.name);(stage/'light_cull.slang').write_bytes(shader.read_bytes())
  summary[label]={'exit':p.returncode,'failures':failures,'cases':2,'shader_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest()}
  (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,summary[label],flush=True)
finally:shader.write_bytes(original)
assert summary['original']['image_sha256']==summary['repeat_original']['image_sha256']=='e5cf18725982816f2cff0868b3b331b77ea77cef8f6949853d333c9780c70393'
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(any(v['failures'] for v in summary.values())))
