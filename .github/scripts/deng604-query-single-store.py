"""CI-only query-store control, original production code/validation/oracles retained."""
from pathlib import Path
import hashlib,json,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/linux-clang-debug';out=build/'deng604-query-single-store'
p=root/'tests/unit/render/test_ssr.c';saved=p.read_bytes();q=root/'shaders/ssr/ssr_resolve.slang';original=q.read_bytes()
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

def query_source():
 text=original.decode();head,tail=text.split('// Native regression oracle:',1)
 tail=tail.replace('    float3 geometric_normal = float3(0,0,1);','    float3 geometric_normal = float3(0,0,1);\n    bool plane_valid = true;',1)
 before='        if (!ssr_geometric_normal(pc, sc, pixel, depth, center, geometric_normal)) {\n            de_rwtex2d(q.output)[id.xy] = float4(0);\n            return;\n        }'
 assert tail.count(before)==1
 tail=tail.replace(before,'        plane_valid = ssr_geometric_normal(pc, sc, pixel, depth, center, geometric_normal);',1)
 before='    de_rwtex2d(q.output)[id.xy] = ssr_history_sample(pc,sc,q.color,uv,position,normal,geometric_normal,true);'
 after='    float4 value = float4(0);\n    if (plane_valid) value = ssr_history_sample(pc,sc,q.color,uv,position,normal,geometric_normal,true);\n    de_rwtex2d(q.output)[id.xy] = value;'
 assert tail.count(before)==1;tail=tail.replace(before,after,1)
 result=(head+'// Native regression oracle:'+tail).encode();assert result.split(b'// Native regression oracle:')[0]==original.split(b'// Native regression oracle:')[0]
 return result
if len(sys.argv)==3 and sys.argv[1]=='--emit':
 target=Path(sys.argv[2]);target.mkdir(parents=True,exist_ok=True);(target/'test_ssr.c').write_bytes(instrument(saved.decode()).encode());(target/'ssr_resolve.slang').write_bytes(query_source());sys.exit(0)
out.mkdir(exist_ok=True);summary={'product':'dac9650c','scope':'test-query control only; GPUAV/debuglayer/inputs/oracles/deadlines unchanged'}
try:
 test=instrument(saved.decode()).encode();shader=query_source();p.write_bytes(test);q.write_bytes(shader);(out/'test_ssr.c').write_bytes(test);(out/'ssr_resolve.slang').write_bytes(shader)
 summary['sources']={'original_test':hashlib.sha256(saved).hexdigest(),'test':hashlib.sha256(test).hexdigest(),'original_resolve':hashlib.sha256(original).hexdigest(),'resolve':hashlib.sha256(shader).hexdigest(),'unchanged_production_prefix':hashlib.sha256(original.split(b'// Native regression oracle:')[0]).hexdigest()}
 with (out/'build.log').open('w') as log:r=subprocess.run(['cmake','--build','--preset','linux-clang-debug','--target','test_render_ssr','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
 assert r.returncode==0,'build failed'
 names=['render_ssr.ssr.'+n+'_vulkan' for n in ['history_footprint','history_shading_normal_flat_plane','history_depth_plane_guards','history_hit_partial_coverage']]
 with (out/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
 cases=E.parse(out/'results.xml').findall('.//testcase');assert len(cases)==4 and {c.get('name') for c in cases}==set(names);errors=[]
 for c in cases:
  text=c.findtext('system-out') or '';assert c.find('skipped') is None and 'llvmpipe' in text.lower()
  if c.find('failure') is not None or c.get('status')!='run':errors.append(c.get('name'))
  if '[error]' in text or '[fatal]' in text:errors.append(c.get('name')+': error-level diagnostic')
 summary['tests']={'exit':r.returncode,'cases':4,'errors':errors};(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
finally:p.write_bytes(saved);q.write_bytes(original);subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(bool(r.returncode or errors)))
