import bindless;
import push_constants;
import text_common;
import "../lighting/lighting_common";
import "../materials/standard/standard";
struct TextPush {uint data,first,view_offset,lighting_view,lighting_entry;uint lights,dir_lights,grid,list,counters,ltc1,ltc2,ltc_sampler;uint world;};
struct TextVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;nointerpolation uint record:TEXCOORD1;float3 world:TEXCOORD2;};
float4 read4(TextPush pc,uint offset){return asfloat(de_bytes(pc.data).load4(offset));}
float4 transform_point(TextPush pc,uint offset,float4 p){return read4(pc,offset)*p.x+read4(pc,offset+16)*p.y+read4(pc,offset+32)*p.z+read4(pc,offset+48)*p.w;}
[shader("vertex")]
TextVertex vs_main(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uniform TextPush pc){uint b=pc.first+(instance*192);float2 corner=float2((vertex==1||vertex==2||vertex==4)?1:0,(vertex==2||vertex==4||vertex==5)?1:0);float4 rect=read4(pc,b),uv=read4(pc,b+16);TextVertex o;o.record=b;o.world=0;
 if(pc.world!=0){float3 origin=read4(pc,b+96).xyz,right=read4(pc,b+112).xyz,up=read4(pc,b+128).xyz;o.world=origin+right*(rect.x+corner.x*rect.z)+up*(rect.y+corner.y*rect.w);o.position=transform_point(pc,pc.view_offset,float4(o.world,1));corner.y=1-corner.y;}else{float4 viewport=read4(pc,pc.view_offset+144);float2 pos=rect.xy+corner*rect.zw;o.position=float4(pos.x*2/viewport.x-1,1-pos.y*2/viewport.y,0,1);}o.uv=lerp(uv.xy,uv.zw,corner);return o;}
float4 color_text(TextVertex v,TextPush pc){uint b=v.record;float4 clip=read4(pc,b+80);if(clip.z>0&&clip.w>0&&(v.position.x<clip.x||v.position.y<clip.y||v.position.x>=clip.x+clip.z||v.position.y>=clip.y+clip.w))discard;uint4 meta=de_bytes(pc.data).load4(b+144);float4 effects=read4(pc,b+160);float4 result=de_text_color(meta.x,meta.y,v.uv,read4(pc,b+16),read4(pc,b+32),read4(pc,b+48),read4(pc,b+64),effects.x,effects.y,effects.zw,meta.z);
 if(pc.world!=0&&(meta.w&2)!=0&&pc.lighting_view!=0){DeLightingView lv=de_robuffer<DeLightingView>(pc.lighting_view)[pc.lighting_entry];DeClusterLists cl;cl.lights=pc.lights;cl.dir_lights=pc.dir_lights;cl.grid=pc.grid;cl.list=pc.list;cl.counters=pc.counters;cl.ltc1=pc.ltc1;cl.ltc2=pc.ltc2;cl.sampler=pc.ltc_sampler;SurfaceData s=de_surface_default();float3 right=read4(pc,b+112).xyz,up=read4(pc,b+128).xyz;s.normal=normalize(cross(right,up));s.base_color=result.a>0?result.rgb/result.a:float3(0);s.roughness=1;s.specular=0;s.brdf_lut=lv.brdf_lut;float3 camera=read4(pc,pc.view_offset+128).xyz,view=normalize(camera-v.world);float depth=-transform_point(pc,pc.view_offset+64,float4(v.world,1)).z;uint cluster=de_cluster_index(lv,uint2(v.position.xy),depth);DeDirect d=evaluate_direct_clustered<StandardModel>(s,v.world,view,uint2(v.position.xy),lv,cl,cluster,false);float3 ind_d,ind_s;evaluate_indirect_split(s,v.world,view,uint2(v.position.xy),lv,ind_d,ind_s);result.rgb=(d.diffuse+d.specular+ind_d+ind_s)*lv.pre_exposure*result.a;}return result;}
[shader("fragment")]
float4 ps_main(TextVertex v,uniform TextPush pc):SV_Target{return color_text(v,pc);}
struct WorldOutput {float4 color:SV_Target;float depth:SV_Depth;};
[shader("fragment")]
WorldOutput ps_world(TextVertex v,uniform TextPush pc){WorldOutput o;o.color=color_text(v,pc);uint flags=de_bytes(pc.data).load(v.record+156);o.depth=(flags&4)!=0?v.position.z:1.0;return o;}
