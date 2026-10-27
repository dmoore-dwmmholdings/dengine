"""Exact failing-before/fixed-after native image, light-list and compiler evidence."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng660-readback';out.mkdir(exist_ok=True)
assert not os.environ.get('DE_GOLDEN_CAPTURE_BAKES')
shader=root/'shaders/lighting/light_cull.slang';saved=shader.read_bytes();summary={}
compiler=shutil.which('dxc')
if not compiler:
 candidates=sorted((Path(os.environ.get('ProgramFiles(x86)','C:/Program Files (x86)'))/'Windows Kits/10/bin').glob('*/x64/dxc.exe'));assert candidates
 compiler=str(candidates[-1])
try:
 for label,source in [('before',subprocess.check_output(['git','show','fda93445:shaders/lighting/light_cull.slang'])),('after',saved)]:
  shader.write_bytes(source);stage=out/label;stage.mkdir();image=build/'golden/d3d12/base/shadows_local_off.png';image.unlink(missing_ok=True)
  env=dict(os.environ,DE_DENG660_PROBE='default',DE_DENG660_READBACK=str(stage.resolve()))
  with (stage/'ctest.log').open('w') as log:
   p=subprocess.run(['ctest','--test-dir',str(build),'-R',r'^golden\.d3d12\.shadows_local_off$','--parallel','1',
    '--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576',
    '--output-junit',str(stage/'results.xml')],env=env,stdout=log,stderr=subprocess.STDOUT)
  cases=E.parse(stage/'results.xml').findall('.//testcase');assert len(cases)==1;c=cases[0]
  assert c.find('skipped') is None and c.get('status')!='notrun'
  t=c.findtext('system-out') or '';assert 'd3d12: enhanced barriers' in t and 'Microsoft Basic Render Driver' in t and '[error]' not in t and '[fatal]' not in t
  assert image.is_file();shutil.copy2(image,stage/image.name);(stage/'light_cull.slang').write_bytes(source)
  with (stage/'lighting_cull.dxil.txt').open('w') as log:subprocess.run([compiler,'-dumpbin',str(stage/'lighting_cull.dxil')],stdout=log,stderr=subprocess.STDOUT,check=True)
  summary[label]={'exit':p.returncode,'failed':c.find('failure') is not None,'seconds':float(c.get('time')),
   'shader_sha256':hashlib.sha256(source).hexdigest(),'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),
   'dxil_sha256':hashlib.sha256((stage/'lighting_cull.dxil').read_bytes()).hexdigest()}
  (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,summary[label],flush=True)
finally:shader.write_bytes(saved)
assert summary['before']['failed'] and summary['before']['image_sha256']=='e5cf18725982816f2cff0868b3b331b77ea77cef8f6949853d333c9780c70393'
assert not summary['after']['failed'] and summary['after']['exit']==0
subprocess.run(['git','diff','--exit-code'],check=True)
