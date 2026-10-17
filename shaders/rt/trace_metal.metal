// Canonical Metal batch-query adapter. Shared material/geometry logic is Slang-generated below.
#include <metal_stdlib>
#include <metal_raytracing>
#include "rt_opacity_metal.generated"
using namespace metal::raytracing;
struct TraceParams { uint context, rays, hits, count, flags; };
struct RtPayload { DeRtContext_0 query; };
struct RtRay { float4 origin, direction; };
struct RtHit { float t; uint instance, primitive; float u, v; uint lod, reserved0, reserved1; };
[[intersection(triangle, triangle_data, instancing)]]
bool de_metal_intersection(uint instance [[user_instance_id]], uint primitive [[primitive_id]],
                          float2 bary [[barycentric_coord]], ray_data RtPayload &payload [[payload]],
                          constant DeBindlessHeap_default_0 &heap [[buffer(0)]]) {
    KernelContext_0 context={};
    context.de_heap_0=&heap;
    DeRtContext_0 query=payload.query;
    return de_rt_opacity_0(&query,instance,primitive,bary,&context);
}
kernel void cs_trace(constant DeBindlessHeap_default_0 &heap [[buffer(0)]],
                     constant TraceParams &pc [[buffer(1)]],
                     intersection_function_table<triangle_data,instancing> table [[buffer(2)]],
                     uint3 tid [[thread_position_in_grid]]) {
    uint i=tid.x+tid.y*(65535u*64u);
    if(i>=pc.count)return;
    device const DeRtContext_0 *contexts=(device const DeRtContext_0 *)heap.buffers_0[pc.context];
    device const RtRay *rays=(device const RtRay *)heap.buffers_0[pc.rays];
    device RtHit *hits=(device RtHit *)heap.buffers_0[pc.hits];
    RtPayload payload={contexts[0]};
    payload.query.flags_4=pc.flags;
    ray r;r.origin=rays[i].origin.xyz;r.direction=rays[i].direction.xyz;
    r.min_distance=rays[i].origin.w;r.max_distance=rays[i].direction.w;
    intersector<triangle_data,instancing> query;
    query.assume_geometry_type(geometry_type::triangle);
    query.accept_any_intersection((pc.flags&2u)!=0u);
    auto hit=query.intersect(r,heap.accels_0.data_3[payload.query.tlas_0],255,table,payload);
    RtHit out={};out.t=-1;out.instance=0xffffffffu;
    if(hit.type==intersection_type::triangle) {
        out.t=hit.distance;out.instance=hit.user_instance_id;out.primitive=hit.primitive_id;
        out.u=hit.triangle_barycentric_coord.x;out.v=hit.triangle_barycentric_coord.y;
        device const uint *lods=(device const uint *)heap.buffers_0[payload.query.lods_1];
        out.lod=lods[out.instance];
    }
    hits[i]=out;
}
