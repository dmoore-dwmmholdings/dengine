"""Native matrix; retain negative controls and stop repeated identical GPU timeouts."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng660-clusters';out.mkdir(exist_ok=True)
backend='d3d12' if os.name=='nt' else 'vulkan';shader=root/'shaders/lighting/light_cull.slang';saved=shader.read_bytes();summary={}
def save(): (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
def run(label,names):
 stage=out/label;stage.mkdir()
 for n in names:
  if n.startswith('golden.'):(build/'golden'/backend/'base'/(n.split('.')[-1]+'.png')).unlink(missing_ok=True)
 with (stage/'ctest.log').open('w') as log:
  p=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$',
   '--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576',
   '--test-output-size-failed','1048576','--output-junit',str(stage/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
 cases=E.parse(stage/'results.xml').findall('.//testcase');assert len(cases)==len(names) and {c.get('name') for c in cases}==set(names)
 failures=[];timeouts=[];images={}
 for c in cases:
  assert c.find('skipped') is None and c.get('status')!='notrun'
  t=c.findtext('system-out') or '';n=c.get('name')
  if n.endswith('_'+backend) or n.startswith('golden.'):
   if backend=='d3d12':assert 'd3d12: enhanced barriers' in t and 'Microsoft Basic Render Driver' in t
   else:assert 'llvmpipe' in t.lower() or 'lavapipe' in t.lower()
  assert '[error]' not in t and '[fatal]' not in t
  if c.find('failure') is not None:
   failures.append(n)
   if float(c.get('time'))>=299:timeouts.append(n)
  if n.startswith('golden.'):
   image=build/'golden'/backend/'base'/(n.split('.')[-1]+'.png');assert image.is_file();shutil.copy2(image,stage/image.name)
   images[n]=hashlib.sha256(image.read_bytes()).hexdigest()
 summary[label]={'exit':p.returncode,'cases':len(cases),'failures':failures,'timeouts':timeouts,
  'shader_sha256':hashlib.sha256(shader.read_bytes()).hexdigest(),'images':images}
 save();print(label,summary[label],flush=True);return summary[label]
regression='render_clustered_lights.clustered_lights.small_mixed_light_sets_'+backend
if os.environ['PRESET']=='windows-clang-cl-debug':
 names=[regression]+['golden.d3d12.shadows_local_'+q for q in ['medium','high','off']]
 try:
  shader.write_bytes(subprocess.check_output(['git','show','fda93445:shaders/lighting/light_cull.slang']))
  before=run('original-shader-control',names);assert set(before['failures'])==set(names)
 finally:shader.write_bytes(saved)
discovered=json.loads(subprocess.check_output(['ctest','--test-dir',str(build),'--show-only=json-v1']))['tests']
families=['shadows_csm','shadows_local','shadows_pcss_contact','ao_gtao','reflections_ssr_probes','gi_probes_ssgi','refraction_ss']
goldens={s+'_'+q for s in families for q in ['medium','high','off']}|{'text_'+n for n in ['msdf_scales','outline_shadow','bitmap_pixel','world_text']}
names=[t['name'] for t in discovered if
 (t['name'].startswith('render_clustered_lights.') and (t['name'].endswith('_'+backend) or not t['name'].endswith(('_metal','_vulkan','_d3d12')))) or
 (t['name'].startswith('golden.'+backend+'.') and t['name'].split('.')[-1] in goldens)]
assert len(names)==44,len(names)
if backend=='d3d12':run('candidate',names)
else:
 extra=[n for n in names if n.endswith('_vulkan') and n!=regression]
 assert len(extra)==11
 first=run('candidate',[n for n in names if n not in extra])
 if regression in first['failures']:
  summary['incomplete_gpu_cases']=extra;save()
 else:
  for i,n in enumerate(extra):
   result=run('candidate-extra-'+str(i),[n])
   if result['timeouts']:
    summary['incomplete_gpu_cases']=extra[i+1:];save();break
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(bool(summary.get('incomplete_gpu_cases')) or any(v.get('failures') for v in summary.values() if isinstance(v,dict) and v is not summary.get('original-shader-control'))))
