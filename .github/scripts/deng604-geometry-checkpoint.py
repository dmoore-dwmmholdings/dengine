"""Exact held checkpoint, uninstrumented fixture, native backend-specific acceptance."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];backend=os.environ['BACKEND'];out=build/'deng604-geometry-checkpoint';out.mkdir(exist_ok=True)
query='history_footprint history_shading_normal_flat_plane history_depth_plane_guards history_hit_partial_coverage history_neighbor_plane_continuity history_verified_geometry'.split()
full='hiz_trace_matches_linear_march ssr_mirror_floor_reference ssr_confidence_converges ssr_history_exposure_correct ssr_off_no_dispatch ssr_rough_converges_no_nan ssr_forward_preserves_fallback ssr_offscreen_confidence_zero ssr_pipelined_history ssr_resize_view_generation simultaneous_projection_cut_resize'.split()
names=['render_ssr.ssr.'+n+'_'+backend for n in query+(full if backend=='d3d12' else [])]
cpu=['render_resolve.resolve.layout_match','render_resolve.resolve.gbuffer_encode_decode_roundtrip']
if backend=='d3d12':
 names+=['render_ssr.ssr.fixture_removal_capacity']
 names+=['render_resolve.resolve.'+n+'_d3d12' for n in 'positions_uvs_cpu_ref positions_uvs_orthographic_cpu_ref positions_uvs_wide_oblique_cpu_ref unlit_model_dispatch game_model_dispatch geometry_normal_provenance motion_vectors_static_zero motion_vectors_moving'.split()]+cpu
 # Same new regression fixtures with the exact pre-fix shader implementations.
 shader_files=['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang','shaders/visbuffer/gbuffer.slang','shaders/visbuffer/resolve_model.slang']
 saved={f:(root/f).read_bytes() for f in shader_files}
 before=out/'before';before.mkdir(exist_ok=True)
 wanted={'render_ssr.ssr.history_verified_geometry_d3d12','render_resolve.resolve.geometry_normal_provenance_d3d12'}
 try:
  for f in shader_files:
   data=subprocess.check_output(['git','show','18ad5001:'+f]);(root/f).write_bytes(data)
   p=before/f;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
  with (before/'build.log').open('w') as log:b=subprocess.run(['cmake','--build','--preset',os.environ['PRESET'],'--target','test_render_ssr','test_render_resolve','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
  assert b.returncode==0,'before build failed'
  with (before/'ctest.log').open('w') as log:b=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,sorted(wanted)))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(before/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
  before_cases=E.parse(before/'results.xml').findall('.//testcase');assert {c.get('name') for c in before_cases}==wanted
  for c in before_cases:
   text=c.findtext('system-out') or '';assert c.find('failure') is not None and c.find('skipped') is None and 'Microsoft Basic Render Driver' in text and 'd3d12: enhanced barriers' in text
   assert '[error]' not in text and '[fatal]' not in text
  assert b.returncode!=0
  (before/'audit.json').write_text(json.dumps({'shader_product':'18ad5001','cases':2,'expected_failures':sorted(wanted),'skip':0,'sources':{f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in shader_files}},indent=2)+'\n')
 finally:
  for f,data in saved.items():(root/f).write_bytes(data)
 with (out/'after-build.log').open('w') as log:b=subprocess.run(['cmake','--build','--preset',os.environ['PRESET'],'--target','test_render_ssr','test_render_resolve','de_golden','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
 assert b.returncode==0,'after build failed'

with (out/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
cases=E.parse(out/'results.xml').findall('.//testcase');assert len(cases)==len(names) and {c.get('name') for c in cases}==set(names);errors=[]
for c in cases:
 text=c.findtext('system-out') or ''
 if c.get('status')!='run' or any(c.find(tag) is not None for tag in ['failure','skipped','error']):errors.append(c.get('name'))
 if c.get('name')!='render_ssr.ssr.fixture_removal_capacity' and c.get('name') not in cpu:
  if backend=='d3d12':assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text
  else:assert 'llvmpipe' in text.lower()
  if '[error]' in text or '[fatal]' in text:errors.append(c.get('name')+': error-level diagnostic')
summary={'product':'8c53f288','backend':backend,'exit':r.returncode,'cases':len(cases),'errors':errors,'sources':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang','shaders/visbuffer/gbuffer.slang','shaders/visbuffer/resolve_model.slang','src/render/passes/vis/gbuffer.h','src/render/passes/vis/resolve.c','tests/unit/render/test_ssr.c','tests/unit/render/test_resolve.c']},'scope':'Uninstrumented fixture; no Vulkan full-view/ANC motion/native Metal/performance clearance'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary),flush=True)
failed=bool(r.returncode or errors)
if backend=='d3d12':
 import shutil
 scenes=[f'{effect}_{quality}' for effect in ['ao_gtao','reflections_ssr_probes'] for quality in ['off','medium','high']]
 goldnames=['golden.d3d12.'+n for n in scenes]
 with (out/'goldens.log').open('w') as log:g=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,goldnames))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'goldens.xml')],stdout=log,stderr=subprocess.STDOUT)
 gcases=E.parse(out/'goldens.xml').findall('.//testcase');assert len(gcases)==6 and {c.get('name') for c in gcases}==set(goldnames)
 bad=[];images={}
 for c in gcases:
  text=c.findtext('system-out') or '';assert c.find('skipped') is None and 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text and '[error]' not in text and '[fatal]' not in text
  if c.find('failure') is not None or c.get('status')!='run':bad.append(c.get('name'))
 for scene in scenes:
  p=build/'golden/d3d12/base'/(scene+'.png');assert p.is_file();shutil.copy2(p,out/p.name);images[scene]=hashlib.sha256(p.read_bytes()).hexdigest()
 summary['original_goldens']={'exit':g.returncode,'cases':6,'failures':bad,'images':images};failed|=bool(g.returncode or bad)
 (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary['original_goldens']),flush=True)
subprocess.run(['git','diff','--exit-code'],check=True);sys.exit(int(failed))
