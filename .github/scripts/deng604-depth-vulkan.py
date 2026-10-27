"""Bounded current-depth query qualification; no full-view timeout waiver."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng604-depth-vulkan';out.mkdir(exist_ok=True)
names=['render_ssr.ssr.'+n+'_vulkan' for n in ['hiz_trace_matches_linear_march','history_footprint','history_shading_normal_flat_plane','history_depth_plane_guards']]
with (out/'ctest.log').open('w') as log:
 r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
cases=E.parse(out/'results.xml').findall('.//testcase');errors=[]
if len(cases)!=len(names) or {c.get('name') for c in cases}!=set(names):errors.append('case set mismatch')
for c in cases:
 text=c.findtext('system-out') or ''
 if any(c.find(t) is not None for t in ('failure','skipped','error')) or c.get('status')!='run':errors.append(c.get('name')+': not passing')
 if 'llvmpipe' not in text.lower() and 'lavapipe' not in text.lower():errors.append(c.get('name')+': no software Vulkan marker')
 if '[error]' in text or '[fatal]' in text:errors.append(c.get('name')+': error-level diagnostic')
summary={'product':'1d869e05','exit':r.returncode,'cases':len(cases),'errors':errors,'source_sha256':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang','tests/unit/render/test_ssr.c']},'scope':'Queries and Hi-Z only; no full-view/native Metal/ANC motion or performance clearance'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(bool(r.returncode or errors)))
