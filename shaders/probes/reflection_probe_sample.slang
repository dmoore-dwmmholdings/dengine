module reflection_probe_sample;
import bindless;
import "../scene/scene_types";
import "../visbuffer/gbuffer";
import "../materials/material_interface";
import "../lighting/ibl";
import "../brdf/energy";
import "../materials/standard/standard_base";
public struct ProbeGpu { float4 position; float4 extent; float4 rotation; float4 blend; uint4 info; };
struct ProbeSampleParams {
    uint probes; uint entry; uint list; uint scene;
    uint slot; uint depth; uint gb0; uint gb1;
    uint gb2; uint output; uint width; uint height;
    uint tile; uint tiles_x; uint tiles_y; uint slices;
    float near_z; float log_scale; float log_bias; uint sampler;
    uint lut; uint ao; uint reserved0; uint reserved1;
};
public float3 de_probe_rotate(float4 q,float3 v) {return v+2*cross(q.xyz,cross(q.xyz,v)+q.w*v);}
public float influence(ProbeGpu p,float3 local) {
    float d=p.info.x==1?p.extent.x-length(local):min(p.extent.x-abs(local.x),min(p.extent.y-abs(local.y),p.extent.z-abs(local.z)));
    return d<=0?0:p.blend.x<=0?1:saturate(d/p.blend.x);
}
public float3 parallax(ProbeGpu p,float3 local,float3 direction) {
    if(p.info.y==0)return direction;
    float t=1e30;
    if(p.info.x==1) {
        float a=dot(direction,direction),b=dot(local,direction),c=dot(local,local)-p.extent.x*p.extent.x;
        t=(-b+sqrt(max(b*b-a*c,0)))/max(a,1e-10);
    } else {
        for(uint k=0;k<3;++k)if(abs(direction[k])>1e-8) {
            float v=((direction[k]>0?p.extent[k]:-p.extent[k])-local[k])/direction[k];
            if(v>=0)t=min(t,v);
        }
    }
    return local+direction*t;
}
public float3 sample_probe(ProbeGpu p,float3 dir,float rough,uint sampler) {
    float lod=rough*float(p.info.w-1);uint lo=uint(floor(lod)),hi=min(lo+1,p.info.w-1);
    float2 st;uint face=de_ibl_cube_face(dir,st);float3 values[2];
    for(uint k=0;k<2;++k) {
        uint m=k==0?lo:hi;float n=max(p.blend.y/exp2(float(m)),1.0);
        float2 uv=clamp(st*.5+.5,.5/n,1-.5/n);
        values[k]=de_tex2d(p.info.z).SampleLevel(de_sampler(sampler),float2(uv.x,(float(face)+uv.y)/6),float(m)).rgb;
    }
    return lerp(values[0],values[1],frac(lod))*p.extent.w;
}
/// Raw, unexposed local radiance and coverage. Material response is applied by shared IBL.
public float4 de_probe_radiance(uint records,uint list,uint entry,uint sampler,uint cluster,
                               float3 position,float3 direction,float roughness) {
    if(records==0||list==0)return float4(0,0,0,0);
    uint count=de_bytes(list).load(cluster*65*4),used=0;
    float remaining=1;float3 sum=0;
    for(uint k=0;k<count&&used<4;++k) {
        uint index=de_bytes(list).load((cluster*65+1+k)*4);
        ProbeGpu p=de_robuffer<ProbeGpu>(records)[entry*64+index];
        float4 inverse=float4(-p.rotation.xyz,p.rotation.w);
        float3 local=de_probe_rotate(inverse,position-p.position.xyz);
        float w=influence(p,local)*remaining;
        if(w<=0)continue;
        float3 corrected=parallax(p,local,de_probe_rotate(inverse,direction));
        sum+=sample_probe(p,de_probe_rotate(p.rotation,corrected),roughness,sampler)*w;
        remaining-=w;++used;
    }
    float weight=1-remaining;
    return float4(weight>0?sum/weight:float3(0,0,0),weight);
}
[shader("compute")] [numthreads(8,8,1)]
void cs_reflection_probe_sample(uint3 id:SV_DispatchThreadID,uniform ProbeSampleParams pc) {
    if(id.x>=pc.width||id.y>=pc.height)return;
    int3 pixel=int3(id.xy,0);float z=de_tex2d(pc.depth).Load(pixel).r;
    float4 result=0;
    if(z>0) {
        DeSceneConstants sc=de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
        float4 clip=float4((float(id.x)+.5)/float(pc.width)*2-1,1-(float(id.y)+.5)/float(pc.height)*2,z,1);
        float4 wp=float4(dot(sc.inv_view_proj[0],clip),dot(sc.inv_view_proj[1],clip),dot(sc.inv_view_proj[2],clip),dot(sc.inv_view_proj[3],clip));
        float3 pos=wp.xyz/wp.w;
        DeGbSurface g=gb_decode(de_tex2d(pc.gb0).Load(pixel),de_tex2d(pc.gb1).Load(pixel),de_tex2d(pc.gb2).Load(pixel));
        float3 v=normalize(sc.camera_position.xyz-pos),n=g.normal,refl=reflect(-v,n);
        float3 dir=de_ibl_dominant_dir(n,refl,g.roughness);
        float depth=-dot(sc.view[2],float4(pos,1));
        uint slice=uint(clamp(floor(log(max(depth,pc.near_z))*pc.log_scale+pc.log_bias),0,float(pc.slices-1)));
        uint cluster=(slice*pc.tiles_y+min(id.y/pc.tile,pc.tiles_y-1))*pc.tiles_x+min(id.x/pc.tile,pc.tiles_x-1);
        uint count=de_bytes(pc.list).load(cluster*65*4),used=0;
        float remaining=1;float3 sum=0;
        for(uint k=0;k<count&&used<4;++k) {
            uint index=de_bytes(pc.list).load((cluster*65+1+k)*4);
            ProbeGpu p=de_robuffer<ProbeGpu>(pc.probes)[pc.entry*64+index];
            float4 inverse=float4(-p.rotation.xyz,p.rotation.w);
            float3 local=de_probe_rotate(inverse,pos-p.position.xyz);
            float w=influence(p,local)*remaining;
            if(w<=0)continue;
            float3 corrected=parallax(p,local,de_probe_rotate(inverse,dir));
            // Capture cubes use world axes; local parallax is transformed back to world.
            sum+=sample_probe(p,de_probe_rotate(p.rotation,corrected),g.roughness,pc.sampler)*w;
            remaining-=w;++used;
        }
        float weight=1-remaining;
        if(weight>0) {
            SurfaceData s=de_surface_from_gb(g);StandardFresnel fr=de_standard_fresnel(s);
            float nov=max(dot(n,v),1e-4);EnergyTerms energy=de_energy_lookup(pc.lut,nov,g.roughness);
            float3 avg;float3 albedo=de_energy_specular_albedo(energy,fr.f0,fr.f90,true,avg);
            float ao=g.ao*(pc.ao!=0?de_tex2d(pc.ao).Load(pixel).r:1);
            float horizon=min(1+dot(refl,n),1.0);
            result=float4(sum/weight*albedo*de_ibl_specular_occlusion(nov,ao,g.roughness)*horizon*horizon,weight);
        }
    }
    de_rwtex2d(pc.output)[id.xy]=result;
}

// Acceptance harness: the same sampling function used by deferred and forward, evaluated at
// supplied reference-camera positions/directions. No CPU approximation of GPU lookup.
struct ProbeReferenceParams {uint records;uint list;uint entry;uint sampler;uint queries;uint output;uint count;uint roughness_bits;};
[shader("compute")] [numthreads(64,1,1)]
void cs_probe_reference(uint3 id:SV_DispatchThreadID,uniform ProbeReferenceParams pc) {
    if(id.x>=pc.count)return;
    float3 position=de_robuffer<float4>(pc.queries)[id.x*2].xyz;
    float3 direction=de_robuffer<float4>(pc.queries)[id.x*2+1].xyz;
    de_buffer<float4>(pc.output)[id.x]=de_probe_radiance(pc.records,pc.list,pc.entry,pc.sampler,0,
        position,direction,asfloat(pc.roughness_bits));
}
