#include "common/rt_pipeline_metal.h"
struct Params { uint output,tlas,alpha; };
[[visible]] void miss0(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=20;}
[[visible]] void miss1(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=21;}
[[visible]] void closest0(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=10;}
[[visible]] void closest1(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=11;}
[[visible]] void closest2(thread DeRtPayload &p,thread const DeRtHit &,constant DeRtHeap &,constant uchar *) {p.words[0]=12;}
[[intersection(triangle,triangle_data,instancing)]]
bool alpha(uint primitive [[primitive_id]],float2 bary [[barycentric_coord]],
           ray_data DeRtPayload &payload [[payload]],constant DeRtHeap &heap [[buffer(0)]]) {
    float u=primitive==0?bary.x+bary.y:bary.x;
    return heap.textures[payload.words[2]].read(uint2(u<.5?0:1,0)).a>=.5;
}
kernel void raygen(constant DeRtHeap &heap [[buffer(0)]],constant Params &pc [[buffer(1)]],
                   DeRtIntersectionTable intersections [[buffer(2)]],
                   DeRtVisibleTable functions [[buffer(3)]],device const uint4 *sbt [[buffer(4)]],
                   uint3 tid [[thread_position_in_grid]]) {
    uint i=tid.x;
    float x=i<3?float(i)*2:(i<5?10:(i==5?1.75:2.25));
    ray r;r.origin=float3(x,0,1);r.direction=float3(0,0,-1);r.min_distance=.001;r.max_distance=10;
    DeRtPayload p={};p.words[2]=pc.alpha;
    de_rt_trace(heap,pc.tlas,r,255,i==4?1:0,p,intersections,functions,sbt,(constant uchar *)&pc);
    ((device uint *)heap.buffers[pc.output])[i]=p.words[0];
}
