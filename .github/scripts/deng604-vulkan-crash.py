"""Diagnostic-only flushed phases and exact-case GDB trace, validation unchanged."""
from pathlib import Path
import hashlib,json,os,signal,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/linux-clang-debug';out=build/'deng604-vulkan-crash'
p=root/'tests/unit/render/test_ssr.c';saved=p.read_bytes()
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
if len(sys.argv)==3 and sys.argv[1]=='--emit':Path(sys.argv[2]).write_bytes(instrument(saved.decode()).encode());sys.exit(0)
out.mkdir(exist_ok=True);summary={'product':'1d869e05','validation':'unchanged debug_layer and gpu_validation true for Vulkan'}
try:
 changed=instrument(saved.decode()).encode();p.write_bytes(changed);(out/'instrumented-test_ssr.c').write_bytes(changed)
 summary['sources']={'original':hashlib.sha256(saved).hexdigest(),'instrumented':hashlib.sha256(changed).hexdigest()}
 with (out/'build.log').open('w') as log:r=subprocess.run(['cmake','--build','--preset','linux-clang-debug','--target','test_render_ssr','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
 assert r.returncode==0,'instrumented build failed'
 name='render_ssr.ssr.history_depth_plane_guards_vulkan'
 with (out/'ctest.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^'+name.replace('.','\\.')+'$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'results.xml')],stdout=log,stderr=subprocess.STDOUT)
 cases=E.parse(out/'results.xml').findall('.//testcase');assert len(cases)==1 and cases[0].get('name')==name and cases[0].find('skipped') is None
 summary['ctest']={'exit':r.returncode,'status':cases[0].get('status'),'failure':cases[0].find('failure').attrib if cases[0].find('failure') is not None else None}
 binary=build/'tests/unit/render/test_render_ssr'
 args=['gdb','--batch','--return-child-result','-ex','set pagination off','-ex','set confirm off','-ex','run','-ex','thread apply all bt 24','-ex','info sharedlibrary','--args',str(binary),'--filter=ssr.history_depth_plane_guards_vulkan']
 with (out/'gdb.log').open('w') as log:
  child=subprocess.Popen(args,cwd=binary.parent,stdout=log,stderr=subprocess.STDOUT,start_new_session=True);timed_out=False
  try:child.wait(timeout=300)
  except subprocess.TimeoutExpired:
   timed_out=True;os.killpg(child.pid,signal.SIGINT)
   try:child.wait(timeout=15)
   except subprocess.TimeoutExpired:os.killpg(child.pid,signal.SIGKILL);child.wait()
 summary['gdb']={'exit':child.returncode,'deadline_seconds':300,'deadline_triggered':timed_out,'command':args}
 (out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
finally:
 p.write_bytes(saved);subprocess.run(['git','diff','--exit-code'],check=True)
# A diagnostic run does not convert a repeated crash into qualification.
sys.exit(int(bool(summary['ctest']['exit'] or summary['gdb']['exit'] or summary['gdb']['deadline_triggered'])))
