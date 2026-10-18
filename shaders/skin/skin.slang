import push_constants;
import bindless;
import "../scene/scene_types";
import "../visbuffer/vis_decode";

struct SkinParams { uint description, palette, output, previous; uint joints, count, first, reserved; };
float3 joint_transform(uint palette,uint joint,float3 p,bool point) {
    DeBytes b=de_bytes(palette); uint at=joint*48;
    float3 x=asfloat(b.load3(at)),y=asfloat(b.load3(at+12)),z=asfloat(b.load3(at+24));
    float3 t=point?asfloat(b.load3(at+36)):float3(0);
    return x*p.x+y*p.y+z*p.z+t;
}
uint4 skin_joints(DeGpuMesh mesh,uint vertex) {
    uint f=mesh.stream_format[1].z,at=mesh.stream_offset[1].z;
    DeBytes b=de_bytes(mesh.vertices);
    if(f==4u) { uint2 q=b.load2(at+vertex*8); return uint4(q.x&65535,q.x>>16,q.y&65535,q.y>>16); }
    uint q=b.load(at+vertex*4); return uint4(q&255,(q>>8)&255,(q>>16)&255,q>>24);
}
float4 skin_weights(DeGpuMesh mesh,uint vertex) {
    uint f=mesh.stream_format[1].w,at=mesh.stream_offset[1].w;
    DeBytes b=de_bytes(mesh.vertices);
    if(f==5u) { uint2 q=b.load2(at+vertex*8); return float4(q.x&65535,q.x>>16,q.y&65535,q.y>>16)/65535.0; }
    uint q=b.load(at+vertex*4); return float4(q&255,(q>>8)&255,(q>>16)&255,q>>24)/255.0;
}
[shader("compute")]
[numthreads(64,1,1)]
void cs_skin(uint3 id:SV_DispatchThreadID,uniform SkinParams pc) {
    uint v=id.x; if(v>=pc.count) return;
    DeGpuMesh mesh=de_robuffer<DeGpuMesh>(pc.description)[0];
    float3 p=de_vis_mesh_position(mesh,v),n=de_vis_mesh_normal(mesh,v);
    float4 tangent=de_vis_mesh_tangent(mesh,v);
    uint4 joints=skin_joints(mesh,v); float4 weights=skin_weights(mesh,v);
    float3 sp=float3(0),sn=float3(0),st=float3(0); float total=0;
    for(uint i=0;i<4;++i) {
        float weight=weights[i]; total+=weight;
        if(joints[i]<pc.joints) {
            sp+=joint_transform(pc.palette,joints[i],p,true)*weight;
            sn+=joint_transform(pc.palette,joints[i],n,false)*weight;
            st+=joint_transform(pc.palette,joints[i],tangent.xyz,false)*weight;
        } else { sp+=p*weight;sn+=n*weight;st+=tangent.xyz*weight; }
    }
    if(total<=0) {sp=p;sn=n;st=tangent.xyz;} else {sp/=total;sn/=total;st/=total;}
    sn=dot(sn,sn)>0?normalize(sn):n; st=dot(st,st)>0?normalize(st):tangent.xyz;
    DeRWBytes out=de_rwbytes(pc.output);
    out.store3(v*12,asuint(sp));
    out.store3(pc.count*12+v*12,asuint(sn));
    out.store4(pc.count*24+v*16,asuint(float4(st,tangent.w)));
    out.store2(pc.count*40+v*8,asuint(de_vis_mesh_uv0(mesh,v)));
    out.store2(pc.count*48+v*8,asuint(de_vis_mesh_uv1(mesh,v)));
    float4 color=saturate(de_vis_mesh_color0(mesh,v));
    uint4 c=uint4(color*255.0+0.5);
    out.store(pc.count*56+v*4,c.x|(c.y<<8)|(c.z<<16)|(c.w<<24));
    if(pc.first!=0) de_rwbytes(pc.previous).store3(v*12,asuint(sp));
}
