"""CI-only validation/control-flow crash attribution; no acceptance downgrade."""
from pathlib import Path
import hashlib,json,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/linux-clang-debug';out=build/'deng604-vulkan-controls'
p=root/'tests/unit/render/test_ssr.c';shader=root/'shaders/ssr/ssr_common.slang';saved=p.read_bytes();original=shader.read_bytes()
def phase(text):return '    fprintf(stderr, "DENG604 '+text+'\\n"); fflush(stderr);\n'
def instrument(text):
 a=text.index('static void history_footprint_range(');b=text.index('#define HISTORY_FOOTPRINT_TEST',a);t=text[a:b]
 t=t.replace('    open_gpu(f, backend);',phase('open_gpu.begin')+'    open_gpu(f, backend);\n'+phase('open_gpu.end'),1)
 t=t.replace('    de__engine_shader_code_free(&code);','    de__engine_shader_code_free(&code);\n'+phase('kernel.ready'),1)
 t=t.replace('        u8 *data =', '        fprintf(stderr, "DENG604 case%u prepare\\n", test); fflush(stderr);\n        u8 *data =',1)
 for before,label in [('        de_gpu_cmd *cmd =','record'),('        ASSERT_EQ(de_gpu_queue_submit','submit'),('        de_gpu_wait_idle(f->dev);','wait'),('        read_texture(f, images[2]','readback')]:
  assert t.count(before)==1,(before,t.count(before));t=t.replace(before,'        fprintf(stderr, "DENG604 case%u '+label+'\\n", test); fflush(stderr);\n'+before,1)
 t=t.replace('        for (u32 i = 0; i < 16; ++i) {','        fflush(stdout);\n        fprintf(stderr, "DENG604 case%u assertions\\n", test); fflush(stderr);\n        for (u32 i = 0; i < 16; ++i) {',1)
 t=t.rstrip();assert t.endswith('}');t=t[:-1]+phase('query_function.end')+'}\n'
 text=text[:a]+t+'\n'+text[b:]
 text=text.replace('static void sfx_teardown(sfx *f) {','static void sfx_teardown(sfx *f) {\n'+phase('teardown.begin'),1)
 text=text.replace('        de_gpu_device_destroy(f->dev);','        '+phase('device_destroy.begin').strip()+'\n        de_gpu_device_destroy(f->dev);\n'+phase('device_destroy.end'),1)
 return text

def sources():
 logged=instrument(saved.decode())
 needle='    if (!ssr_depth_axis(pc, sc, pixel, depth, position, int2(1,0), dx) ||\n        !ssr_depth_axis(pc, sc, pixel, depth, position, int2(0,1), dy)) return false;'
 replacement='    bool valid_x = ssr_depth_axis(pc, sc, pixel, depth, position, int2(1,0), dx);\n    bool valid_y = ssr_depth_axis(pc, sc, pixel, depth, position, int2(0,1), dy);\n    if (!valid_x || !valid_y) return false;'
 assert original.decode().count(needle)==1
 flag='.gpu_validation = backend == DE_GPU_VULKAN,';assert logged.count(flag)==1
 return {'original-gpuav-off':(logged.replace(flag,'.gpu_validation = false,').encode(),original),
         'explicit-axes-gpuav-on':(logged.encode(),original.decode().replace(needle,replacement).encode())}
if len(sys.argv)==3 and sys.argv[1]=='--emit':
 target=Path(sys.argv[2]);target.mkdir(parents=True,exist_ok=True)
 for label,(test,common) in sources().items():
  d=target/label;d.mkdir(exist_ok=True);(d/'test_ssr.c').write_bytes(test);(d/'ssr_common.slang').write_bytes(common)
 sys.exit(0)
out.mkdir(exist_ok=True);summary={'product':'1d869e05','stages':{},'scope':'GPUAV-off is attribution only; original inputs/oracles/deadlines retained'};failed=False
try:
 for label,(test,common) in sources().items():
  d=out/label;d.mkdir(exist_ok=True);p.write_bytes(test);shader.write_bytes(common)
  (d/'test_ssr.c').write_bytes(test);(d/'ssr_common.slang').write_bytes(common)
  with (d/'build.log').open('w') as log:r=subprocess.run(['cmake','--build','--preset','linux-clang-debug','--target','test_render_ssr','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
  assert r.returncode==0,'build failed: '+label
  name='render_ssr.ssr.history_depth_plane_guards_vulkan'
  with (d/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^'+re.escape(name)+'$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(d/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
  cases=E.parse(d/'results.xml').findall('.//testcase');assert len(cases)==1 and cases[0].get('name')==name and cases[0].find('skipped') is None
  c=cases[0];text=c.findtext('system-out') or '';assert 'llvmpipe' in text.lower() and '[error]' not in text and '[fatal]' not in text
  summary['stages'][label]={'exit':r.returncode,'status':c.get('status'),'failure':c.find('failure').attrib if c.find('failure') is not None else None,'test_sha256':hashlib.sha256(test).hexdigest(),'common_sha256':hashlib.sha256(common).hexdigest()}
  (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(label,json.dumps(summary['stages'][label]),flush=True);failed|=bool(r.returncode)
finally:
 p.write_bytes(saved);shader.write_bytes(original);subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(failed))
