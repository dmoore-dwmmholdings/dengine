"""CI-only original runtime qualification of Base-tier packed fixtures plus separately reviewed WARP references."""
from pathlib import Path
import hashlib,json,os,re,subprocess,sys,xml.etree.ElementTree as ET
root=Path.cwd();preset=os.environ['PRESET'];backend=os.environ['BACKEND'];build=root/'build'/preset;out=build/'deng659';out.mkdir(parents=True,exist_ok=True)
scenes=[f'{effect}_{quality}' for effect in ('ao_gtao','reflections_ssr_probes') for quality in ('off','medium','high')]
base_scenes=[f'{family}_{quality}' for family in ('shadows_csm','shadows_local','shadows_pcss_contact','ao_gtao','reflections_ssr_probes','gi_probes_ssgi','refraction_ss') for quality in ('off','medium','high')]
goldnames=['golden.'+backend+'.'+x for x in base_scenes]
text_scenes='text_msdf_scales text_outline_shadow text_bitmap_pixel text_world_text'.split()
ui_scenes='ui_rect_border ui_nine_slice ui_clip_nested ui_z_popup ui_text_labels ui_widgets_basic ui_widgets_advanced_text ui_widgets_advanced_controls ui_world_1m ui_world_20m ui_world_occluded ui_world_overlay ui_world_lit ui_world_origin_100km ui_pixel_four'.split()
textnames=['golden.'+backend+'.'+x for x in text_scenes+(ui_scenes if backend=='d3d12' else [])]
packs=re.findall(r'TEST(?:_F)?\(golden_assets, (\w+)',(root/'tests/golden/harness/test_cooked_assets.c').read_text())
native='hiz_trace_matches_linear_march ssr_mirror_floor_reference ssr_confidence_converges ssr_history_exposure_correct ssr_off_no_dispatch ssr_rough_converges_no_nan ssr_forward_preserves_fallback ssr_offscreen_confidence_zero ssr_pipelined_history ssr_resize_view_generation history_footprint simultaneous_projection_cut_resize'.split()
unitnames=['render_ssr.ssr.fixture_removal_capacity']+['render_ssr.ssr.'+n+'_d3d12' for n in native]
assert len(packs)==4 and len(unitnames)==13
summary={};failed=False

def run(args,path):
 with path.open('w') as log:p=subprocess.run(args,stdout=log,stderr=subprocess.STDOUT)
 return p.returncode

def family(label,names,kind):
 global failed
 stage=out/label;stage.mkdir(exist_ok=True)
 pattern='^('+'|'.join(map(re.escape,names))+')$'
 if kind=='golden':
  for scene in [n.split('.',2)[2] for n in names]:(build/'golden'/backend/'base'/(scene+'.png')).unlink(missing_ok=True)
 code=run(['ctest','--test-dir',str(build),'-R',pattern,'--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(stage/'results.xml')],stage/'ctest.log')
 cases=ET.parse(stage/'results.xml').findall('.//testcase');errors=[]
 if len(cases)!=len(names) or {c.get('name') for c in cases}!=set(names):errors.append('case set/count mismatch')
 for c in cases:
  name=c.get('name');text=c.findtext('system-out') or ''
  if c.get('status')!='run' or any(c.find(t) is not None for t in ('failure','skipped','error')):errors.append(name+': failed or skipped')
  if kind in ('unit','golden') and name!='render_ssr.ssr.fixture_removal_capacity':
   if backend=='d3d12':
    if 'd3d12: enhanced barriers' not in text or 'Microsoft Basic Render Driver' not in text:errors.append(name+': native enhanced/WARP markers missing')
   elif 'llvmpipe' not in text.lower() and 'lavapipe' not in text.lower():errors.append(name+': actual lavapipe marker missing')
   if '[error]' in text or '[fatal]' in text:errors.append(name+': error-level diagnostic')
  if kind=='golden':
   packed='mounted pack "goldenpacked:/' in text
   if packed!=(not preset.endswith('-debug')):errors.append(name+': incorrect SOURCE/PACKED mode')
 images={}
 if kind=='golden':
  for scene in [n.split('.',2)[2] for n in names]:
   p=build/'golden'/backend/'base'/(scene+'.png')
   if not p.is_file():errors.append(scene+': missing image');continue
   data=p.read_bytes();(stage/p.name).write_bytes(data);images[scene]=hashlib.sha256(data).hexdigest()
 summary[label]={'exit':code,'count':len(cases),'errors':errors,'images':images}
 failed|=bool(code or errors);(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
 print(label,json.dumps({k:v for k,v in summary[label].items() if k!='images'}),flush=True)

config=(build/'de_config_dump.txt').read_text()
assert ('DE_HAS_COOKER=1' in config)==preset.endswith('-debug'), 'runtime cooker policy changed'
if preset.endswith('-debug'):
 code=run(['cmake','--build','--preset',preset,'--target','test_render_ssr','--parallel','4'],out/'unit-build.log')
 if code:raise RuntimeError('original Debug SSR fixture build failed')
 family('unit',unitnames,'unit')
family('packs',['golden_assets.'+n for n in packs],'pack')
family('base-goldens',goldnames,'golden')
family('text-ui',textnames,'golden')
family('repeat-six',['golden.'+backend+'.'+n for n in scenes],'golden')
summary['repeated_image_identity']={n:n in summary['base-goldens']['images'] and summary['base-goldens']['images'][n]==summary['repeat-six']['images'].get(n) for n in scenes}
summary['refs']={p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (root/'tests/golden/refs'/backend/'base').glob('*.png')}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(failed))
