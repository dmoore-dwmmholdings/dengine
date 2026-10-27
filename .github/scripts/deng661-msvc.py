"""Actual MSVC fail-first compiler proof and unchanged tooltip CPU oracles."""
from pathlib import Path
import hashlib,json,re,subprocess,sys,xml.etree.ElementTree as E
root=Path.cwd();build=root/'build/windows-msvc-debug';out=build/'deng661-msvc';out.mkdir(exist_ok=True)
p=root/'tests/unit/ui/test_dropdown_tabs_tooltip.c';saved=p.read_bytes();summary={}
def compile(label):
 with (out/(label+'.log')).open('w') as log:r=subprocess.run(['cmake','--build','--preset','windows-msvc-debug','--target','test_ui_layout','--parallel','4'],stdout=log,stderr=subprocess.STDOUT)
 return r.returncode
try:
 p.write_bytes(subprocess.check_output(['git','show','81743f90:tests/unit/ui/test_dropdown_tabs_tooltip.c']))
 code=compile('before-build');text=(out/'before-build.log').read_text(errors='replace')
 lines=set(re.findall(r'test_dropdown_tabs_tooltip.c\((\d+)(?:,\d+)?\).*warning C4244',text))
 summary['before']={'exit':code,'C4244_lines':sorted(lines),'source_sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
 assert code!=0 and lines=={'297','298','318','319'},summary
finally:p.write_bytes(saved)
code=compile('after-build');summary['after']={'exit':code,'source_sha256':hashlib.sha256(saved).hexdigest()}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');assert code==0,summary
names=['ui_layout.ui_advanced.'+n for n in re.findall(r'TEST_F\(ui_advanced,\s*(\w+)',saved.decode())]
with (out/'tests.log').open('w') as log:r=subprocess.run(['ctest','--test-dir',str(build),'-R','^('+'|'.join(map(re.escape,names))+')$','--parallel','1','--no-tests=error','--output-on-failure','--test-output-size-passed','1048576','--test-output-size-failed','1048576','--output-junit',str(out/'tests.xml')],stdout=log,stderr=subprocess.STDOUT)
cases=E.parse(out/'tests.xml').findall('.//testcase');errors=[]
if len(cases)!=len(names) or {c.get('name') for c in cases}!=set(names):errors.append('case set mismatch')
for c in cases:
 if c.get('status')!='run' or any(c.find(tag) is not None for tag in ['failure','skipped','error']):errors.append(c.get('name'))
summary['tests']={'exit':r.returncode,'cases':len(cases),'expected':names,'errors':errors}
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps(summary))
subprocess.run(['git','diff','--exit-code'],check=True)
sys.exit(int(bool(r.returncode or errors)))
