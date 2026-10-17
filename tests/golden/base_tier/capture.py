#!/usr/bin/env python3
"""Capture the DENG-343 matrix twice, require byte identity, retain all raw logs.
Missing first references are accepted only for capture; renderer/validation failures are not.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
SCENES=('shadows_csm','shadows_local','shadows_pcss_contact','ao_gtao','reflections_ssr_probes','gi_probes_ssgi','refraction_ss')
p=argparse.ArgumentParser();p.add_argument('--exe',required=True);p.add_argument('--backend',required=True);p.add_argument('--out',required=True);a=p.parse_args()
out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
results=[]
for scene in SCENES:
    for quality in ('medium','high','off'):
        name=scene+'_'+quality
        for repeat in (1,2):
            dest=out/('capture'+str(repeat)); log=out/(name+'-'+str(repeat)+'.log')
            cmd=[a.exe,'--backend',a.backend,'--scene',name,'--out',str(dest)]
            if a.backend=='d3d12':cmd+=['--warp']
            with log.open('w') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
            text=log.read_text();fails=[line for line in text.splitlines() if line.startswith('FAIL ')]
            expected_missing=r.returncode==1 and len(fails)==1 and ': no reference ' in fails[0]
            if r.returncode!=0 and not expected_missing:
                print(name,'capture failed; see',log,flush=True);sys.exit(1)
        images=[out/('capture'+str(n))/a.backend/'base'/(name+'.png') for n in (1,2)]
        same=images[0].read_bytes()==images[1].read_bytes()
        row=dict(scene=name,identical=same,sha256=hashlib.sha256(images[0].read_bytes()).hexdigest())
        results.append(row);print(json.dumps(row),flush=True)
        if not same:sys.exit('nonrepeatable '+name)
(out/'repeatability.json').write_text(json.dumps(results,indent=2)+'\n')
print('21 actual captures; 21 byte-identical repeats; no skips')
