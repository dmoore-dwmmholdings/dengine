"""One-time unchanged Windows family acceptance; excluded from product PR."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,xml.etree.ElementTree as ET
root=Path.cwd();build=root/'build'/os.environ['PRESET'];out=build/'deng652';out.mkdir(parents=True,exist_ok=True)
scenes='text_msdf_scales text_outline_shadow text_bitmap_pixel text_world_text ui_rect_border ui_nine_slice ui_clip_nested ui_z_popup ui_text_labels ui_widgets_basic ui_widgets_advanced_text ui_widgets_advanced_controls ui_world_1m ui_world_20m ui_world_occluded ui_world_overlay ui_world_lit ui_world_origin_100km ui_pixel_four'.split()
fonts=re.findall(r'TEST\(font, (\w+)\)',(root/'tests/unit/text/test_font_import.c').read_text())
packs=re.findall(r'TEST(?:_F)?\(golden_assets, (\w+)',(root/'tests/golden/harness/test_cooked_assets.c').read_text())
assert len(scenes)==19 and len(fonts)==10 and len(packs)==3
families={'cpu':['text_import.font.'+n for n in fonts]+['golden_assets.'+n for n in packs],'goldens':['golden.d3d12.'+n for n in scenes]}
summary={};failed=False
for family,names in families.items():
 pattern=r'^('+'|'.join(map(re.escape,names))+r')$';report=out/(family+'.xml');log=out/(family+'.log')
 args=['ctest','--test-dir',str(build),'-R',pattern,'--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(report)]
 with log.open('w') as f:p=subprocess.run(args,stdout=f,stderr=subprocess.STDOUT)
 cases=ET.parse(report).findall('.//testcase');errors=[]
 if len(cases)!=len(names) or {c.attrib['name'] for c in cases}!=set(names):errors.append('case count/set mismatch')
 for c in cases:
  name=c.attrib['name'];text=c.findtext('system-out') or ''
  if any(c.find(t) is not None for t in ('failure','error','skipped')) or c.get('status')=='notrun':errors.append(name+': failed/skipped')
  if family=='goldens':
   if 'd3d12: enhanced barriers' not in text or 'd3d12 on Microsoft Basic Render Driver' not in text:errors.append(name+': actual enhanced/WARP execution unobserved')
   if '[error]' in text or '[fatal]' in text:errors.append(name+': error-level diagnostics')
   packed='mounted pack \"goldenpacked:/' in text
   if packed != (not os.environ['PRESET'].endswith('-debug')):errors.append(name+': wrong SOURCE/PACKED runtime mode')
 summary[family]={'exit':p.returncode,'count':len(cases),'errors':errors};failed|=bool(p.returncode or errors)
 print(family,json.dumps(summary[family]),flush=True)
images={}
for scene in scenes:
 p=build/'golden/d3d12/base'/(scene+'.png')
 if not p.is_file():failed=True;images[scene]={'missing':True};continue
 images[scene]={'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'bytes':p.stat().st_size}
summary['images']=images
summary['refs']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'tests/golden/refs/d3d12/base').glob('*.png')}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(failed))
