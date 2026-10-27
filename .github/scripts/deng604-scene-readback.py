"""CI-only matched original/instrumented scene SSR buffer acquisition."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/windows-clang-cl-debug';out=build/'deng604-scene-readback';out.mkdir(exist_ok=True)
files=['src/render/passes/ssr/ssr.c','src/render/passes/ssr/scene_history.c','src/render/passes/ssr/ssr_internal.h','tests/golden/base_tier/base_tier_scenes.c']
saved={f:(root/f).read_bytes() for f in files};original={f:subprocess.check_output(['git','show','dac9650c:'+f]) for f in files}
shaders=['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang'];candidate={f:(root/f).read_bytes() for f in shaders};main={f:subprocess.check_output(['git','show','81743f90:'+f]) for f in shaders}
summary={'product':'dac9650c','base':'81743f90','stages':{}};failed=False
try:
 for label,shader,instrumented in [('main-control',main,False),('main-capture',main,True),('candidate-control',candidate,False),('candidate-capture',candidate,True)]:
  d=out/label;d.mkdir(exist_ok=True)
  for f,data in (saved if instrumented else original).items():(root/f).write_bytes(data)
  for f,data in shader.items():(root/f).write_bytes(data)
  with (d/'build.log').open('w') as log:r=subprocess.run(['cmake','--build','--preset','windows-clang-cl-debug','--target','de_golden','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
  assert r.returncode==0,label+' build failed'
  env=dict(os.environ);env.pop('DE_DENG604_READBACK',None)
  if instrumented:env['DE_DENG604_READBACK']=str(d.resolve())
  name='golden.d3d12.reflections_ssr_probes_medium'
  with (d/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^'+re.escape(name)+'$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(d/'results.xml')],env=env,stdout=log,stderr=subprocess.STDOUT)
  cases=E.parse(d/'results.xml').findall('.//testcase');assert len(cases)==1 and cases[0].get('name')==name and cases[0].find('skipped') is None
  c=cases[0];text=c.findtext('system-out') or '';assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text
  assert '[error]' not in text and '[fatal]' not in text
  image=build/'golden/d3d12/base/reflections_ssr_probes_medium.png';assert image.is_file();shutil.copy2(image,d/'image.png')
  if instrumented:
   meta=json.loads((d/'metadata.json').read_text());assert meta['frame']==95 and len(meta['row_pitches'])==11
   names=['depth','gbuffer1','motion','hits','rays','history_color','history_surface','previous_specular','previous_moments','output','write_moments']
   for i,n in enumerate(names):assert (d/(n+'.bin')).stat().st_size==meta['row_pitches'][i]*(meta['trace_height'] if i in (3,4) else meta['height'])
   assert (d/'params.bin').stat().st_size==128 and (d/'scene.bin').stat().st_size>0
  summary['stages'][label]={'exit':r.returncode,'failure':c.find('failure').attrib if c.find('failure') is not None else None,'image_sha256':hashlib.sha256(image.read_bytes()).hexdigest(),'sources':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in files+shaders},'buffers':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in d.glob('*.bin')}}
  failed|=bool(r.returncode);(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,json.dumps(summary['stages'][label]),flush=True)
 for label in ['main','candidate']:
  matches=(out/(label+'-control')/'image.png').read_bytes()==(out/(label+'-capture')/'image.png').read_bytes()
  summary[label+'_capture_image_exact']=matches;failed|=not matches
 (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
finally:
 for f,data in saved.items():(root/f).write_bytes(data)
 for f,data in candidate.items():(root/f).write_bytes(data)
 subprocess.run(['git','diff','--exit-code'],check=True)
# Original candidate golden failures remain red diagnostics, not qualification.
sys.exit(int(failed))
