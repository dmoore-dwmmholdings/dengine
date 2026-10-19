import bindless;
import push_constants;
import ui_common;
struct UiPush {uint data,first;float width,height,white;};
struct UiVertex {float4 position:SV_Position;float2 uv:TEXCOORD0;float2 local:TEXCOORD1;nointerpolation uint index:TEXCOORD2;};
[shader("vertex")]
UiVertex vs_main(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uniform UiPush p){
 float2 c=vertex==0?float2(0,0):vertex==1?float2(1,0):vertex==2?float2(0,1):vertex==3?float2(0,1):vertex==4?float2(1,0):float2(1,1);
 uint index=p.first+instance;UiQuad q=de_robuffer<UiQuad>(p.data)[index];float2 pos=q.rect.xy+c*q.rect.zw;
 UiVertex v;v.position=float4(pos.x/p.width*2-1,1-pos.y/p.height*2,0,1);v.uv=lerp(q.uv.xy,q.uv.zw,c);v.local=c*q.rect.zw;v.index=index;return v;
}
[shader("fragment")]
float4 ps_main(UiVertex v,uniform UiPush p):SV_Target{
 UiQuad q=de_robuffer<UiQuad>(p.data)[v.index];
 float4 color=de_ui_sample(q,v.uv,v.local);color.rgb*=p.white;return color;
}
