"""Native acceptance, preserving known failures rather than accepting new references."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng660-clusters';out.mkdir(exist_ok=True)
backend='d3d12' if os.name=='nt' else 'vulkan'
shader=root/'shaders/lighting/light_cull.slang';saved=shader.read_bytes();summary={}
def run(label,names):
 stage=out/label;stage.mkdir()
 with (stage/'ctest.log').open('w') as log:
  p=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$',
   '--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576',
   '--test-output-size-failed','1048576','--output-junit',str(stage/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
 cases=E.parse(stage/'results.xml').findall('.//testcase');assert len(cases)==len(names) and {c.get('name') for c in cases}==set(names)
 failures=[]
 for c in cases:
  assert c.find('skipped') is None and c.get('status')!='notrun'
  text=c.findtext('system-out') or ''
  if c.get('name').endswith('_'+backend) or c.get('name').startswith('golden.'):
   if backend=='d3d12':assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text
   else:assert 'llvmpipe' in text.lower() or 'lavapipe' in text.lower()
  assert '[error]' not in text and '[fatal]' not in text
  if c.find('failure') is not None:failures.append(c.get('name'))
 for image in (build/'golden'/backend/'base').glob('*.png'):shutil.copy2(image,stage/image.name)
 summary[label]={'exit':p.returncode,'cases':len(cases),'failures':failures,
  'shader_sha256':hashlib.sha256(shader.read_bytes()).hexdigest()}
 (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,summary[label],flush=True)
if False: # Exact original-shader/new-test control retained from40540593932
 try:
  shader.write_bytes(subprocess.check_output(['git','show','fda93445056a2835571598b5f674b46029196843:shaders/lighting/light_cull.slang']))
  run('original-shader-control',['render_clustered_lights.clustered_lights.small_mixed_light_sets_d3d12'])
 finally:shader.write_bytes(saved)
 assert summary['original-shader-control']['failures']==['render_clustered_lights.clustered_lights.small_mixed_light_sets_d3d12']
discovered=json.loads(subprocess.check_output(['ctest','--test-dir',str(build),'--show-only=json-v1']))['tests']
names=[t['name'] for t in discovered if
 (t['name'].startswith('render_clustered_lights.') and (t['name'].endswith('_'+backend) or not t['name'].endswith(('_metal','_vulkan','_d3d12')))) or
 (t['name'].startswith('golden.'+backend+'.') and t['name'].split('.')[-1] in
 ['shadows_csm_medium','shadows_csm_high','shadows_csm_off','shadows_local_medium','shadows_local_high','shadows_local_off',
  'ao_gtao_medium','ao_gtao_high','ao_gtao_off','reflections_ssr_probes_medium','reflections_ssr_probes_high','reflections_ssr_probes_off',
  'text_msdf_scales','text_outline_shadow','text_bitmap_pixel','text_world_text'])]
names=['render_clustered_lights.clustered_lights.'+n+'_d3d12' for n in ['small_mixed_light_sets','clusters_assignment_cpu_ref']]+['golden.d3d12.shadows_local_off']
assert len(names)==3
run('candidate',names)
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(bool(summary['candidate']['failures'])))
