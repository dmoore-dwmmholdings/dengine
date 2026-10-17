#!/usr/bin/env python3
"""DENG-343 deterministic CC0 procedural production-renderer scenes; Python stdlib only."""
import importlib.util
import math
import sys
sys.dont_write_bytecode = True
from pathlib import Path
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('gallery', HERE.parents[1] / 'materials/assets/gen_assets.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
g.HERE = str(HERE)

def obj(sc, pos, scale, material='white', mesh='box', rot=(0.,0.,0.,1.)):
    mr = g.mesh_renderer('mesh/'+mesh+'#'+mesh, 'mat/'+material)
    mr[1][-1] = ('cast_shadows', True)
    sc.entity([g.xf(pos, rot, scale), mr])

def light(sc, kind, pos, intensity, color=(1.,.96,.9), radius=.05, rot=(0.,0.,0.,1.)):
    fields=[('kind', 'DE_LIGHT_'+kind),('color',list(color)),('intensity',intensity),('range',30.),('cast_shadows',True),('source_radius',radius)]
    if kind=='SPOT': fields += [('spot_inner',.35),('spot_outer',.65)]
    if kind=='RECT': fields += [('area_size',[2.,1.])]
    sc.entity([g.xf(pos,rot),('DeLight',fields)])

def room(sc, floor='floor'):
    obj(sc,(0.,2.,-.15),(12.,16.,.3),floor)
    obj(sc,(0.,7.,3.),(12.,.3,6.),'white')
    obj(sc,(-6.,2.,3.),(.3,10.,6.),'red')
    obj(sc,(6.,2.,3.),(.3,10.,6.),'green')

def main():
    for name, data in [('box',g.mesh_box()),('sphere',g.mesh_sphere()),('card',g.mesh_card()),('cylinder',g.mesh_cylinder())]: g.out('meshes/'+name+'.gltf',data)
    g.out('textures/checker.png',g.tex_checker());g.out('env/sky.exr',g.env_sky())
    mats={'white':dict(base_color=[.65,.65,.65],roughness=.65), 'red':dict(base_color=[.7,.05,.03],roughness=.7),'green':dict(base_color=[.05,.6,.12],roughness=.7),'floor':dict(base_color=[.55,.55,.55],base_color_tex='tex/checker',roughness=.6),'mirror':dict(base_color=[.8,.8,.8],metallic=1.,roughness=.09),'glass':dict(base_color=[.7,.92,1.],transmission=1.,ior=1.5,roughness=.05,thickness=.25,attenuation_color=[.4,.8,.95],attenuation_distance=2.),'emit':dict(base_color=[1.,1.,1.],emissive=[1.,.7,.3],emissive_strength=12.)}
    for kv in mats.values():kv.setdefault('metallic',0.)
    for n,kv in mats.items():g.out('materials/'+n+'.mat.toml',g.mat_file(n,kv))
    scenes={}
    for name in ['shadows_csm','shadows_local','shadows_pcss_contact','ao_gtao','reflections_ssr_probes','gi_probes_ssgi','refraction_ss']:
        sc=g.Scene('Base-tier '+name+' (DENG-343)')
        g.camera(sc,(0.,-13.,7.),20.,50.,11. if name=='shadows_csm' else 10. if name in ('shadows_pcss_contact','refraction_ss') else 3.)
        if name != 'shadows_csm': room(sc, 'mirror' if name=='reflections_ssr_probes' else 'floor')
        if name=='shadows_csm':
            # Long range rows exercise cascades beyond the near room.
            obj(sc,(0.,65.,-.2),(30.,140.,.4),'floor')
            for y in [0.,8.,20.,40.,75.,110.]:
                for x in [-3.,3.]:obj(sc,(x,y,2.),(.6,.6,4.))
            light(sc,'DIRECTIONAL',(0.,0.,8.),5000.,radius=.008,rot=g.quat_axis((1.,0.,0.),.55))
        elif name=='shadows_local':
            # Open slatted cage: point inside, alternating lit stripes visible on floor/wall.
            for x in [-2.,2.]:
                for y in [0.,.6,1.2,1.8,2.4,3.]:obj(sc,(x,y,1.4),(.12,.12,2.8))
            for x in [-1.8,-1.2,-.6,0.,.6,1.2,1.8]:obj(sc,(x,3.,1.4),(.12,.12,2.8))
            obj(sc,(0.,1.5,2.9),(4.2,3.2,.12))
            light(sc,'POINT',(0.,1.5,1.8),1800.,(1.,.6,.25),.04)
            light(sc,'SPOT',(-4.,0.,5.),1500.,(.3,.5,1.),.08)
            light(sc,'RECT',(4.,2.,4.),80.,(.4,1.,.6),.15)
        elif name=='shadows_pcss_contact':
            for x,h in [(-3.,.03),(-1.,.3),(1.,.8),(3.,1.5)]:
                obj(sc,(x,1.,h+.12),(1.2,1.2,.24));obj(sc,(x,3.,1.5),(.12,.12,3.))
            light(sc,'DIRECTIONAL',(0.,0.,8.),3000.,radius=.06,rot=g.quat_axis((1.,0.,0.),.75))
        elif name=='ao_gtao':
            for x in [-2.,0.,2.]:obj(sc,(x,2.,.6),(1.6,1.6,1.2))
            for y in [-1.,-.7,-.4]:obj(sc,(-3.,y,.2),(2.,.1,.4))
            g.environment(sc,'env/sky',.8)
        elif name=='reflections_ssr_probes':
            obj(sc,(-2.,2.,1.3),(1.5,1.5,2.6),'red');obj(sc,(2.,4.,1.),(2.,2.,2.),'green')
            obj(sc,(0.,5.,3.),(2.,.2,1.),'emit')
            light(sc,'POINT',(0.,0.,5.),1800.)
            sc.entity([g.xf((0.,2.,2.)),('DeReflectionProbe',[('extent',[5.5,5.,3.]),('resolution',64),('box_projection',True),('update','DE_PROBE_ONCE')])],eid=0x500)
        elif name=='gi_probes_ssgi':
            obj(sc,(-2.,2.,1.),(2.,2.,2.));obj(sc,(2.,4.,1.5),(2.,2.,3.))
            obj(sc,(0.,2.,5.5),(3.,3.,.15),'emit')
            light(sc,'RECT',(0.,2.,5.),120.)
            sc.entity([g.xf((0.,2.,2.5)),('DeIrradianceVolume',[('extent',[4.,4.,2.]),('counts',[2,2,2]),('spacing',4.),('bounces',1),('blend_distance',1.)])],eid=0x600)
        else:
            for x in [-2.,0.,2.]:obj(sc,(x,1.,1.6),(1.5,.25,2.8),'glass')
            obj(sc,(0.,4.,2.),(9.,.2,4.),'floor')
            light(sc,'DIRECTIONAL',(0.,0.,8.),3000.,radius=.01,rot=g.quat_axis((1.,0.,0.),.5))
            g.environment(sc,'env/sky',.4)
        scenes[name]=sc
        g.out('scenes/'+name+'_capture.scene.toml',sc.text())
        bake = 'reflection.deprobe' if name == 'reflections_ssr_probes' else 'irradiance.degi'
        if name in ('reflections_ssr_probes', 'gi_probes_ssgi') and (HERE/'bakes'/bake).exists():
            sc.lines.append('bake = "bake/'+bake+'"')
        g.out('scenes/'+name+'.scene.toml',sc.text())
    manifest=['[project]','asset_root = "."','','[defaults.model]','generate_lods = false','','[assets]']
    for n in ['box','sphere','card','cylinder']:manifest.append(f'"mesh/{n}" = {{type="model", source="meshes/{n}.gltf"}}')
    manifest += ['"tex/checker" = {type="texture",source="textures/checker.png"}','"env/sky" = {type="texture",source="env/sky.exr",usage="environment",env_size=32,env_samples=256,compression="none"}']
    for n in mats:manifest.append(f'"mat/{n}" = {{type="material",source="materials/{n}.mat.toml"}}')
    for bake in ['reflection.deprobe','irradiance.degi']:
        if (HERE/'bakes'/bake).exists(): manifest.append(f'"bake/{bake}" = {{type="blob",source="bakes/{bake}"}}')
    for n in scenes:manifest.append(f'"scenes/{n}_capture" = {{type="scene",source="scenes/{n}_capture.scene.toml"}}')
    for n in scenes:manifest.append(f'"scenes/{n}" = {{type="scene",source="scenes/{n}.scene.toml"}}')
    g.out('assets.toml', ('\n'.join(manifest)+'\n').encode())
if __name__=='__main__':main()
