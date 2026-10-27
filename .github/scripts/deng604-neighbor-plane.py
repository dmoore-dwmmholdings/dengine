"""Fail-first query regression on exact product source; no shader overrides."""
from pathlib import Path
import hashlib,json,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/windows-clang-cl-debug';out=build/'deng604-neighbor-plane';out.mkdir(exist_ok=True)
names=['render_ssr.ssr.'+n+'_d3d12' for n in ['history_footprint','history_shading_normal_flat_plane','history_depth_plane_guards','history_hit_partial_coverage','history_neighbor_plane_continuity']]
with (out/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
cases=E.parse(out/'results.xml').findall('.//testcase');assert len(cases)==5 and {c.get('name') for c in cases}==set(names);failed=[]
for c in cases:
 text=c.findtext('system-out') or '';assert c.find('skipped') is None and 'd3d12: enhanced barriers' in text and 'Microsoft Basic Render Driver' in text and '[error]' not in text and '[fatal]' not in text
 if c.find('failure') is not None or c.get('status')!='run':failed.append(c.get('name'))
summary={'product':'4e5cbfa7','exit':r.returncode,'cases':5,'failures':failed,'sources':{n:hashlib.sha256((root/n).read_bytes()).hexdigest() for n in ['shaders/ssr/ssr_common.slang','shaders/ssr/ssr_resolve.slang','tests/unit/render/test_ssr.c']},'scope':'Fail-first actual query regression; candidate remains held'}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary));subprocess.run(['git','diff','--exit-code'],check=True);sys.exit(int(bool(r.returncode or failed)))
