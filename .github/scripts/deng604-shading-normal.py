"""Diagnostic: unchanged flat geometry must accept matching normal-mapped shading."""
from pathlib import Path
import json,hashlib,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/windows-clang-cl-debug';out=build/'deng604-shading-normal';out.mkdir(exist_ok=True)
p=root/'shaders/ssr/ssr_common.slang';saved=p.read_bytes();summary={}
name='render_ssr.ssr.history_shading_normal_flat_plane_d3d12'
original='hiz_trace_matches_linear_march ssr_mirror_floor_reference ssr_confidence_converges ssr_history_exposure_correct ssr_off_no_dispatch ssr_rough_converges_no_nan ssr_forward_preserves_fallback ssr_offscreen_confidence_zero ssr_pipelined_history ssr_resize_view_generation history_footprint simultaneous_projection_cut_resize'.split()
names=['render_ssr.ssr.fixture_removal_capacity']+['render_ssr.ssr.'+n+'_d3d12' for n in original]+[name]
def run(label,names):
 import re
 with (out/(label+'.log')).open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/(label+'.xml'))],stdout=log,stderr=subprocess.STDOUT)
 cases=E.parse(out/(label+'.xml')).findall('.//testcase');assert len(cases)==len(names) and {c.get('name') for c in cases}==set(names)
 failures=[]
 for c in cases:
  assert c.find('skipped') is None and c.get('status')!='notrun'
  text=c.findtext('system-out') or ''
  if c.get('name')!='render_ssr.ssr.fixture_removal_capacity':assert 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text
  if c.find('failure') is not None:failures.append(c.get('name'))
 summary[label]={'exit':r.returncode,'cases':len(cases),'failures':failures,'common_shader_sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
 (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,summary[label],flush=True)
try:
 p.write_bytes(subprocess.check_output(['git','show','b82abe648e73b9641a4ac85407fe34a996a2a742:shaders/ssr/ssr_common.slang']))
 run('original-sampler-control',[name])
finally:p.write_bytes(saved)
run('held-candidate',names)
subprocess.run(['git','diff','--exit-code'],check=True)
# A red candidate is a diagnostic result, never a qualification pass.
sys.exit(int(any(v['exit'] or v['failures'] for v in summary.values())))
