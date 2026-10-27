"""CI-only current-main/candidate SSR diagnostics, retaining every original failure."""
from pathlib import Path
import hashlib,json,os,re,shutil,subprocess,sys,xml.etree.ElementTree as ET
root=Path.cwd();preset=os.environ['PRESET'];backend=os.environ['BACKEND'];build=root/'build'/preset;out=build/'deng604-depth-scenes';out.mkdir(parents=True,exist_ok=True)
base='81743f901357cfbea66f652866f647f29a1b8ad7'
shaders=['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang']
saved={f:(root/f).read_bytes() for f in shaders}
native='hiz_trace_matches_linear_march ssr_mirror_floor_reference ssr_confidence_converges ssr_history_exposure_correct ssr_off_no_dispatch ssr_rough_converges_no_nan ssr_forward_preserves_fallback ssr_offscreen_confidence_zero ssr_pipelined_history ssr_resize_view_generation history_footprint history_shading_normal_flat_plane history_depth_plane_guards simultaneous_projection_cut_resize'.split()
unitnames=['render_ssr.ssr.fixture_removal_capacity']+['render_ssr.ssr.'+n+'_'+backend for n in native]
scenes=[f'{effect}_{quality}' for effect in ('ao_gtao','reflections_ssr_probes') for quality in ('off','medium','high')]
goldnames=['golden.'+backend+'.'+n for n in scenes]
summary={'base':base,'candidate':'1d869e05','backend':backend,'stages':{}};failed=False

def command(args,path):
 with path.open('w') as log:p=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT)
 return p.returncode

def family(label,names):
 global failed
 directory=out/label;directory.mkdir(exist_ok=True)
 pattern='^('+'|'.join(re.escape(n) for n in names)+')$'
 code=command(['ctest','--test-dir',str(build),'-R',pattern,'--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(directory/'results.xml')],directory/'ctest.log')
 cases=ET.parse(directory/'results.xml').findall('.//testcase');errors=[]
 if len(cases)!=len(names) or {c.get('name') for c in cases}!=set(names):errors.append('case set mismatch')
 for case in cases:
  name=case.get('name');output=case.findtext('system-out') or ''
  if case.get('status')!='run' or any(case.find(t) is not None for t in ('failure','skipped','error')):errors.append(name+': not passing')
  if name!='render_ssr.ssr.fixture_removal_capacity':
   if backend=='d3d12':
    if 'd3d12: enhanced barriers' not in output or 'Microsoft Basic Render Driver' not in output:errors.append(name+': no actual enhanced WARP marker')
   elif 'llvmpipe' not in output.lower() and 'lavapipe' not in output.lower():errors.append(name+': no actual software Vulkan marker')
   if '[error]' in output or '[fatal]' in output:errors.append(name+': error-level diagnostic')
 result={'exit':code,'count':len(cases),'errors':errors,'images':{}}
 if names==goldnames:
  for scene in scenes:
   source=build/'golden'/backend/'base'/(scene+'.png')
   if not source.is_file():errors.append(scene+': missing actual image');continue
   shutil.copy2(source,directory/source.name)
   result['images'][scene]=hashlib.sha256(source.read_bytes()).hexdigest()
 result['sources']={f:hashlib.sha256((root/f).read_bytes()).hexdigest() for f in shaders}
 summary['stages'][label]=result;failed|=bool(code or errors)
 (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
 print(label,json.dumps({k:v for k,v in result.items() if k not in ('images','sources')}),flush=True)

try:
 for f in shaders:(root/f).write_bytes(subprocess.check_output(['git','show',base+':'+f]))
 code=command(['cmake','--build','--preset',preset,'--target','de_golden','--parallel','4'],out/'baseline-build.log')
 if code:raise RuntimeError('baseline build failed')
 family('baseline-goldens',goldnames)
finally:
 for f,data in saved.items():(root/f).write_bytes(data)
 assert all((root/f).read_bytes()==data for f,data in saved.items())
code=command(['cmake','--build','--preset',preset,'--target','de_golden','test_render_ssr','--parallel','4'],out/'candidate-build.log')
if code:raise RuntimeError('candidate build failed')
family('candidate-unit',unitnames)
family('candidate-goldens',goldnames)
for scene in scenes:
 a=summary['stages']['baseline-goldens']['images'].get(scene)
 b=summary['stages']['candidate-goldens']['images'].get(scene)
 if scene.startswith('ao_') or scene.endswith('_off'):
  if not a or a!=b:failed=True;summary.setdefault('unchanged_control_errors',[]).append(scene)
summary['unchanged_controls_match']=not summary.get('unchanged_control_errors')
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(failed))
