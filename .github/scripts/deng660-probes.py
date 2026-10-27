"""Diagnostic-only raster/culling ablations; never a shipped feature-disable fix."""
from pathlib import Path
import json,hashlib,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng660-probes';out.mkdir(exist_ok=True)
assert not os.environ.get('DE_GOLDEN_CAPTURE_BAKES')
shader=root/'shaders/lighting/light_cull.slang';traced=shader.read_bytes();original=subprocess.check_output(['git','show','fda93445:shaders/lighting/light_cull.slang'])
summary={};failed=False
try:
 for label,mode in [('default','default'),('readback','default'),('repeat-default','default')]:
  shader.write_bytes(traced if label=='readback' else original)
  stage=out/label;stage.mkdir();image=build/'golden/d3d12/base/shadows_local_off.png';image.unlink(missing_ok=True)
  env=dict(os.environ,DE_DENG660_PROBE=mode)
  if label=='readback':env['DE_DENG660_READBACK']=str(stage.resolve())
  with (stage/'ctest.log').open('w') as log:p=subprocess.run(['ctest','--test-dir',str(build),'-R',r'^golden\.d3d12\.shadows_local_off$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(stage/'results.xml')],env=env,stdout=log,stderr=subprocess.STDOUT)
  cases=E.parse(stage/'results.xml').findall('.//testcase');assert len(cases)==1;c=cases[0];text=c.findtext('system-out') or ''
  assert c.get('name')=='golden.d3d12.shadows_local_off' and c.get('status')!='notrun' and c.find('skipped') is None
  assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text and '[error]' not in text and '[fatal]' not in text
  marker=re.search(r'DENG660_PROBE mode='+re.escape(mode)+r' raster=(mesh|compute|none) occlusion=(\d) triangles=(\d) cones=(\d) instances_off=(\d)',text);assert marker and marker.group(1)!='none'
  ambient=re.search(r'ambient=([-0-9.e+]+)',text);assert ambient and float(ambient.group(1))==(1.0 if mode=='ambient' else 0.0)
  assert image.is_file();shutil.copy2(image,stage/image.name)
  (stage/'light_cull.slang').write_bytes(shader.read_bytes())
  result={'shader_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),'exit':p.returncode,'failed':c.find('failure') is not None,'seconds':float(c.get('time')),'actual_state':marker.group(0),'actual_ambient':float(ambient.group(1)),'sha256':hashlib.sha256(image.read_bytes()).hexdigest()}
  summary[label]=result;failed|=bool(p.returncode or result['failed']);print(label,result,flush=True)
  (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
finally:shader.write_bytes(traced)
old='e5cf18725982816f2cff0868b3b331b77ea77cef8f6949853d333c9780c70393'
summary['control']={'reproduces_original_image':summary['default']['sha256']==old,'default_repeats_exactly':summary['default']['sha256']==summary['repeat-default']['sha256']}
summary['control']['instrumentation_preserves_image']=summary['readback']['sha256']==old
stage=out/'readback'
for name in ['lights','counters','grid','list','bounds','scene','depth','gbuffer1','view']:
 assert (stage/(name+'.bin')).is_file() and (stage/(name+'.bin')).stat().st_size>0
assert json.loads((stage/'metadata.json').read_text())['frame']==95
failed|=not all(summary['control'].values());(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
subprocess.run(['git','diff','--exit-code'],check=True);sys.exit(int(failed))
