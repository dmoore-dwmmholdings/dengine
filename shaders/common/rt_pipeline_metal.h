#ifndef DE_RT_PIPELINE_METAL_H
#define DE_RT_PIPELINE_METAL_H
// S17 native Metal RT ABI. Heap prefix matches bindless.slang; tail is backend-private.
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace metal::raytracing;
struct DeRtHeap {
    array<texture2d<float>, 65536> textures;
    array<texture2d<float, access::read_write>, 16384> storage;
    array<sampler, 2048> samplers;
    array<device uchar *, 65536> buffers;
    array<instance_acceleration_structure, 65536> accels;
    array<device const uint *, 65536> instances;
};
struct DeRtPayload {
    uint words[16];
};
struct DeRtHit {
    float distance;
    uint custom_index, instance, geometry, primitive;
    float2 barycentrics;
    float4 origin, direction;
};
using DeRtFunction = void(thread DeRtPayload &, thread const DeRtHit &, constant DeRtHeap &,
                          constant uchar *);
using DeRtVisibleTable = visible_function_table<DeRtFunction>;
using DeRtIntersectionTable = intersection_function_table<triangle_data, instancing>;
// Required fallback export for groups without any-hit. Opaque geometry skips any-hit normally.
[[intersection(triangle, triangle_data, instancing)]]
bool de_rt_accept() {
    return true;
}
// Each SBT record is uint4. Record0 stores miss/hit counts and the hit section byte offset.
inline void de_rt_trace(constant DeRtHeap &heap, uint tlas, ray r, uint mask, uint miss,
                        thread DeRtPayload &payload, DeRtIntersectionTable intersections,
                        DeRtVisibleTable functions, device const uint4 *sbt, constant uchar *push) {
    DeRtHit out = {};
    out.distance = -1;
    out.custom_index = 0xffffffffu;
    out.origin = float4(r.origin, r.min_distance);
    out.direction = float4(r.direction, r.max_distance);
    if (r.max_distance > r.min_distance && dot(r.direction, r.direction) > 0) {
        intersector<triangle_data, instancing> query;
        query.assume_geometry_type(geometry_type::triangle);
        auto h = query.intersect(r, heap.accels[tlas], mask, intersections, payload);
        if (h.type == intersection_type::triangle) {
            out.distance = h.distance;
            out.custom_index = h.user_instance_id;
            out.instance = h.instance_id;
            out.geometry = h.geometry_id;
            out.primitive = h.primitive_id;
            out.barycentrics = h.triangle_barycentric_coord;
            uint group = heap.instances[tlas][h.instance_id * 18 + 14] + h.geometry_id;
            if (group < sbt[0].y)
                functions[sbt[sbt[0].w / 16 + group].x](payload, out, heap, push);
            return;
        }
    }
    if (miss < sbt[0].x)
        functions[sbt[1 + miss].x](payload, out, heap, push);
}
#endif
