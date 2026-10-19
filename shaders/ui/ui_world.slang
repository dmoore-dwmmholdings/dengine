import bindless;
import ui_common;
import "../scene/scene_types";
import "../lighting/lighting_common";
import "../materials/standard/standard";
struct WorldQuad {UiQuad quad;float4 clip;};
struct WorldRoot {float4 rows[3];float4 panel;};
struct WorldPush {uint scene,slot,lighting,entry;DeClusterLists clusters;uint data,first,roots,root;};
struct WorldVertex {
 float4 position:SV_Position;
 float2 uv:TEXCOORD0;float2 local:TEXCOORD1;float2 panel:TEXCOORD2;
 float3 world:TEXCOORD3;nointerpolation uint index:TEXCOORD4;
};
[shader("vertex")]
WorldVertex vs_world(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uniform WorldPush p){
 float2 c=vertex==0?float2(0,0):vertex==1?float2(1,0):vertex==2?float2(0,1):vertex==3?float2(0,1):vertex==4?float2(1,0):float2(1,1);
 uint index=p.first+instance;WorldQuad q=de_robuffer<WorldQuad>(p.data)[index];WorldRoot root=de_robuffer<WorldRoot>(p.roots)[p.root];
 float2 pixel=q.quad.rect.xy+c*q.quad.rect.zw;
 float4 local=float4(pixel.x/root.panel.z-root.panel.x*.5,root.panel.y*.5-pixel.y/root.panel.z,0,1);
 WorldVertex v;v.world=float3(dot(root.rows[0],local),dot(root.rows[1],local),dot(root.rows[2],local));
 DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];float4 world=float4(v.world,1);
 v.position=float4(dot(sc.view_proj[0],world),dot(sc.view_proj[1],world),dot(sc.view_proj[2],world),dot(sc.view_proj[3],world));
 v.uv=lerp(q.quad.uv.xy,q.quad.uv.zw,c);v.local=c*q.quad.rect.zw;v.panel=pixel;v.index=index;return v;
}
[shader("fragment")]
float4 ps_world(WorldVertex v,uniform WorldPush p):SV_Target{
 WorldQuad q=de_robuffer<WorldQuad>(p.data)[v.index];
 if(any(v.panel<q.clip.xy)||any(v.panel>=q.clip.xy+q.clip.zw)) discard;
 float4 color=de_ui_sample(q.quad,v.uv,v.local);
 if(color.a<=0) discard;
 WorldRoot root=de_robuffer<WorldRoot>(p.roots)[p.root];
 DeLightingView lv=de_robuffer<DeLightingView>(p.lighting)[p.entry];
 if(root.panel.w!=0){
  // Opaque screen-space estimates are not measurements of this forward receiver.
  lv.inputs.ao=0;lv.inputs.rt_shadow_visibility=0;lv.inputs.rt_shadow_keys=0;
  lv.inputs.restir_diffuse=0;lv.inputs.restir_specular=0;
  if((lv.inputs.reserved0&DE_INPUT_CONTACT_METADATA)!=0){lv.inputs.contact_shadows=0;lv.inputs.reserved0&=~DE_INPUT_CONTACT_METADATA;}
  if((lv.inputs.reserved0&DE_INPUT_SPECULAR_OPAQUE)!=0)lv.inputs.indirect_specular=0;
  if((lv.inputs.reserved0&DE_INPUT_DIFFUSE_OPAQUE)!=0)lv.inputs.indirect_diffuse=0;
  lv.inputs.reserved0&=~DE_INPUT_AO_GTAO;
  DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];
  float3 view=normalize(sc.camera_position.xyz-v.world);
  float3 x=float3(root.rows[0].x,root.rows[1].x,root.rows[2].x),y=float3(root.rows[0].y,root.rows[1].y,root.rows[2].y);
  float3 normal=normalize(cross(x,y));if(dot(normal,view)<0)normal=-normal;
  SurfaceData s=de_surface_default();s.base_color=color.rgb/max(color.a,1e-6);s.normal=normal;s.specular=0;s.brdf_lut=lv.brdf_lut;
  uint2 pixel=uint2(v.position.xy);float depth=-dot(sc.view[2],float4(v.world,1));uint cluster=de_cluster_index(lv,pixel,depth);
  DeDirect direct=evaluate_direct_clustered<StandardModel>(s,v.world,view,pixel,lv,p.clusters,cluster,false);
  float3 indirect_diffuse,indirect_specular;
  evaluate_indirect_split(s,v.world,view,pixel,lv,indirect_diffuse,indirect_specular);
  color.rgb=(direct.diffuse+direct.specular+indirect_diffuse+indirect_specular)*color.a;
 }
 color.rgb*=lv.pre_exposure;return color;
}
