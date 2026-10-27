"""Exact held checkpoint, uninstrumented fixture, native backend-specific acceptance."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];backend=os.environ['BACKEND'];out=build/'deng604-query-checkpoint';out.mkdir(exist_ok=True)
query='history_footprint history_shading_normal_flat_plane history_depth_plane_guards history_hit_partial_coverage'.split()
full='hiz_trace_matches_linear_march ssr_mirror_floor_reference ssr_confidence_converges ssr_history_exposure_correct ssr_off_no_dispatch ssr_rough_converges_no_nan ssr_forward_preserves_fallback ssr_offscreen_confidence_zero ssr_pipelined_history ssr_resize_view_generation simultaneous_projection_cut_resize'.split()
names=['render_ssr.ssr.'+n+'_'+backend for n in query+(full if backend=='d3d12' else [])]
if backend=='d3d12':names+=['render_ssr.ssr.fixture_removal_capacity']
with (out/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
cases=E.parse(out/'results.xml').findall('.//testcase');assert len(cases)==len(names) and {c.get('name') for c in cases}==set(names);errors=[]
for c in cases:
 text=c.findtext('system-out') or ''
 if c.get('status')!='run' or any(c.find(tag) is not None for tag in ['failure','skipped','error']):errors.append(c.get('name'))
 if c.get('name')!='render_ssr.ssr.fixture_removal_capacity':
  if backend=='d3d12':assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text
  else:assert 'llvmpipe' in text.lower()
  if '[error]' in text or '[fatal]' in text:errors.append(c.get('name')+': error-level diagnostic')
summary={'product':'153c6675','backend':backend,'exit':r.returncode,'cases':len(cases),'errors':errors,'sources':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang','tests/unit/render/test_ssr.c']},'scope':'Uninstrumented fixture; no Vulkan full-view/ANC motion/native Metal/performance clearance'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary));subprocess.run(['git','diff','--exit-code'],check=True);sys.exit(int(bool(r.returncode or errors)))
