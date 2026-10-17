// DENG-43, R2 native intersection acceptance, compiled by the installed Metal toolchain.
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;
struct Heap {
    ulong textures[65536], storage[16384], samplers[2048], buffers[65536];
    instance_acceleration_structure accels[65536];
};
[[intersection(triangle, triangle_data, instancing)]]
bool de_metal_intersection(device atomic_uint *calls [[buffer(1)]]) {
    atomic_fetch_add_explicit(calls, 1, memory_order_relaxed);
    return true;
}
kernel void trace(constant Heap &heap [[buffer(0)]], constant packed_uint3 &pc [[buffer(1)]],
                  intersection_function_table<triangle_data, instancing> table [[buffer(2)]]) {
    device float *out = (device float *)heap.buffers[pc.y];
    intersector<triangle_data, instancing> query;
    query.assume_geometry_type(geometry_type::triangle);
    query.accept_any_intersection(false);
    ray r;
    r.origin = float3(3, 0, 0);
    r.direction = float3(0, 0, 1);
    r.min_distance = 0;
    r.max_distance = 100;
    auto hit = query.intersect(r, heap.accels[pc.x], 255, table);
    out[0] = hit.type == intersection_type::triangle ? hit.distance : -1;
    out[1] = float(hit.user_instance_id);
}
kernel void generate(device uint *dst [[buffer(2)]], constant uint *in [[buffer(3)]],
                     uint i [[thread_position_in_grid]]) {
    if (i < 15) dst[i] = in[i];
}
