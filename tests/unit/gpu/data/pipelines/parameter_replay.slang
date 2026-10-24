import bindless;
import push_constants;
struct UploadText {uint source,destination,offset,pitch,x,y,width,height;};
[shader("compute")][numthreads(8,8,1)]
void cs_main(uint3 tid:SV_DispatchThreadID,uniform UploadText pc){if(tid.x>=pc.width||tid.y>=pc.height)return;uint rgba=de_bytes(pc.source).load(pc.offset+tid.y*pc.pitch+tid.x*4);de_rwtex2d(pc.destination)[uint2(pc.x,pc.y)+tid.xy]=float4(rgba&255,(rgba>>8)&255,(rgba>>16)&255,rgba>>24)/255.0;}
