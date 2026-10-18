import bindless;
import push_constants;
struct Rect {float4 bounds;float4 color;};
struct Params {uint rectangles;float width;float height;uint pad;};
struct Vertex {float4 position:SV_Position;nointerpolation float4 color:COLOR;};
[shader("vertex")] Vertex vs_main(uint vertex:SV_VertexID,uint instance:SV_InstanceID,uniform Params p) {
 const float2 corners[6]={float2(0,0),float2(1,0),float2(1,1),float2(0,0),float2(1,1),float2(0,1)};
 Rect r=de_robuffer<Rect>(p.rectangles)[instance];float scale=min(p.width/320,p.height/400);
 float2 xy=(float2(p.width,p.height)-float2(320,400)*scale)*.5+(r.bounds.xy+corners[vertex]*r.bounds.zw)*scale;
 Vertex o;o.position=float4(xy/float2(p.width,p.height)*float2(2,-2)+float2(-1,1),0,1);o.color=r.color;return o;
}
[shader("fragment")]float4 ps_main(Vertex i):SV_Target {
 // Palette tokens are sRGB; the swapchain applies sRGB encoding, so supply linear radiance.
 float3 c=i.color.rgb;return float4(select(c<=.04045,c/12.92,pow((c+.055)/1.055,2.4)),1);
}
