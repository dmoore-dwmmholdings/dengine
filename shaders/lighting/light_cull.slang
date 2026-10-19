// shaders/lighting/light_cull.slang: the clustered light lists of one view and frame (SPEC §12.5
// step 5; DENG-303). Engine shader `lighting_cull`; src/render/lighting/clusters.c runs, on the
// graphics queue:
//
//   cs_light_clear  64 threads: the first clear_bytes bytes of `counters` to 0 (the light counters;
//                   also the deferred pass's tile counters)
//   cs_light_prep   64 threads, one per GPU-scene light slot, then one per instance slot:
//                   - a live directional light -> the directional list (DE_LIGHTING_MAX_DIRECTIONAL
//                     kept, the rest counted)
//                   - a live point, spot, rect or disk light -> the local list, in shading units
//                     (lights.slang), with its derived range and its view-space culling sphere
//                   - an instance whose submesh-0 material's emissive luminance x its bounds'
//                     surface area exceeds emissive_threshold -> a proxy point light (an
//                     isotropic sphere emitter fitted to the bounds: intensity L pi R^2, source
//                     radius R; at most proxy_capacity per frame, the rest counted)
//   cs_light_count  64 threads, one per cluster: the local lights touching the cluster's bounds
//                   (sphere vs AABB; spots also cone vs the AABB's sphere; area lights sphere
//                   around the emitter), clamped to max_per_cluster (the excess counted)
//   cs_light_scan   one 1024-thread group: the exclusive prefix sum of the counts -> each
//                   cluster's offset; entries past the list capacity are cut and counted
//   cs_light_fill   64 threads, one per cluster: the same tests again, writing the light indices
//                   at the cluster's offset (in local-list order)
//
// Atomic compaction may reorder local lights. Given the same local set, fill selects and orders
// complete records consistently, including when a cluster exceeds its bounded list budget.
import bindless;
import push_constants;
import "../shadows/atlas_sample";
import "../shadows/contact_common";
import "../lighting/lighting_common";

/// Threads of cs_light_scan.
public static const uint DE_LIGHT_SCAN_GROUP = 1024;

/// Push constants of every kernel (48 bytes; clusters.c cl_cull_params).
struct CullParams
{
    uint view;       // DeLightingView ring (de_robuffer)
    uint entry;      // this frame's entry
    uint scene;      // DeSceneConstants table
    uint view_slot;  // the view's entry in it
    uint bounds;     // cluster AABBs (raw, cluster_build.slang)
    uint lights;     // DeClusterLight[local_capacity] (structured)
    uint dir_lights; // DeClusterLight[DE_LIGHTING_MAX_DIRECTIONAL] (structured)
    uint grid;       // uint2 per cluster (structured)
    uint list;       // uint per entry (structured)
    uint counters;   // raw DE_LIGHTING_COUNTER_*
    uint clear_bytes; // cs_light_clear: bytes of `counters` to zero (a multiple of 4, <= 256)
    uint reserved1;
    uint contact_records;
    uint3 padding;
};

static const float3 LUMA = float3(0.2126, 0.7152, 0.0722);

float max3(float3 v)
{
    return max(v.x, max(v.y, v.z));
}

float3 to_view(DeSceneConstants sc, float3 p)
{
    float4 h = float4(p, 1.0);
    return float3(dot(sc.view[0], h), dot(sc.view[1], h), dot(sc.view[2], h));
}

float3 dir_to_view(DeSceneConstants sc, float3 d)
{
    return float3(dot(sc.view[0].xyz, d), dot(sc.view[1].xyz, d), dot(sc.view[2].xyz, d));
}

void push_local(CullParams pc, DeLightingView lv, DeClusterLight l)
{
    uint i = de_rwbytes(pc.counters).atomic_add(DE_LIGHTING_COUNTER_LOCAL, 1u);
    if (i < lv.local_capacity)
        de_buffer<DeClusterLight>(pc.lights)[i] = l;
}

/// The cluster light of GPU-scene light `g` (not directional).
DeClusterLight local_light(DeGpuLight g, DeSceneConstants sc, DeLightingView lv, uint atlas, uint contact)
{
    DeClusterLight l;
    uint kind = g.kind;
    float3 pos = float3(g.transform[0].w, g.transform[1].w, g.transform[2].w);
    float3 zc = float3(g.transform[0].z, g.transform[1].z, g.transform[2].z);
    float3 xc = float3(g.transform[0].x, g.transform[1].x, g.transform[2].x);
    float3 dir = normalize(-zc);
    float3 ax = normalize(xc - dir * dot(xc, dir));
    float power = g.color.w;
    float outer = clamp(g.params.z, 1e-3, DE_LIGHT_PI);
    float inner = clamp(g.params.y, 0.0, outer);
    float intensity = power;
    float half_x = 0.0, half_y = 0.0, extent = 0.0;
    if (kind == DE_LIGHT_KIND_POINT)
        intensity = de_point_candela(power);
    else if (kind == DE_LIGHT_KIND_SPOT)
        intensity = de_spot_candela(power, outer);
    else if (kind == DE_LIGHT_KIND_RECT)
    {
        half_x = 0.5 * g.area.x;
        half_y = 0.5 * g.area.y;
        extent = sqrt(half_x * half_x + half_y * half_y);
    }
    else // disk: radius in half_x (and half_y, unused)
    {
        half_x = 0.5 * g.area.x;
        half_y = half_x;
        extent = half_x;
    }
    float3 color = g.color.rgb * intensity;
    float range = g.params.x;
    if (!(range > 0.0))
    {
        // area lights: the intensity of the emitter seen face on, L x area
        float i_max = max3(color);
        if (kind == DE_LIGHT_KIND_RECT)
            i_max *= 4.0 * half_x * half_y;
        else if (kind == DE_LIGHT_KIND_DISK)
            i_max *= DE_LIGHT_PI * half_x * half_x;
        range = de_light_derived_range(i_max, lv.cutoff);
    }
    float cos_o = cos(outer), cos_i = cos(inner);
    l.position = float4(pos, range);
    l.direction = float4(dir, max(g.params.w, 0.0));
    l.color = float4(color, cos_o);
    l.shape = float4(1.0 / max(cos_i - cos_o, 1e-4), half_x, half_y, asfloat(kind));
    uint flags = (g.flags & DE_GPU_LIGHT_CAST_SHADOWS) != 0u ? DE_CLUSTER_LIGHT_SHADOWS : 0u;
    // Bits 8..15 are an immutable per-view face-record index, resolved before atomic compaction.
    // Shared GPU-scene records and the 96-byte cluster ABI remain unchanged.
    flags |= atlas_find(atlas, uint2(g.entity_lo,g.entity_hi)) << 8u;
    flags |= de_contact_find(contact, uint2(g.entity_lo,g.entity_hi)) << 16u;
    l.axis = float4(kind == DE_LIGHT_KIND_SPOT ? dir_to_view(sc, dir) : ax, asfloat(flags));
    l.bound = float4(to_view(sc, pos), range + extent);
    return l;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_light_clear(uint3 id : SV_DispatchThreadID, uniform CullParams pc)
{
    uint o = id.x * 4u;
    if (o < pc.clear_bytes)
        de_rwbytes(pc.counters).store(o, 0u);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_light_prep(uint3 id : SV_DispatchThreadID, uniform CullParams pc)
{
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    DeSceneConstants sc = de_robuffer<DeSceneConstants>(pc.scene)[pc.view_slot];
    uint t = id.x;
    if (t < lv.light_slots)
    {
        if (sc.lights == DE_BINDLESS_INVALID || t >= sc.light_count)
            return;
        DeGpuLight g = de_robuffer<DeGpuLight>(sc.lights)[t];
        if ((g.flags & DE_GPU_LIGHT_LIVE) == 0u || !(g.color.w > 0.0))
            return;
        if (g.kind == DE_LIGHT_KIND_DIRECTIONAL)
        {
            uint i = de_rwbytes(pc.counters).atomic_add(DE_LIGHTING_COUNTER_DIRECTIONAL, 1u);
            if (i >= DE_LIGHTING_MAX_DIRECTIONAL)
                return;
            float3 zc = float3(g.transform[0].z, g.transform[1].z, g.transform[2].z);
            DeClusterLight l;
            l.position = float4(0.0, 0.0, 0.0, 0.0);
            l.direction = float4(normalize(-zc), max(g.params.w, 0.0));
            l.color = float4(g.color.rgb * g.color.w, -1.0);
            l.shape = float4(0.0, 0.0, 0.0, asfloat(DE_LIGHT_KIND_DIRECTIONAL));
            l.axis = float4(0.0, 0.0, 0.0, asfloat((g.flags & DE_GPU_LIGHT_CAST_SHADOWS)!=0u?DE_CLUSTER_LIGHT_SHADOWS:0u));
            l.bound = float4(asfloat(g.entity_lo), asfloat(g.entity_hi), 0.0, 1e30);
            de_buffer<DeClusterLight>(pc.dir_lights)[i] = l;
            return;
        }
        if (g.kind > DE_LIGHT_KIND_DISK)
            return;
        push_local(pc, lv, local_light(g, sc, lv, pc.reserved1, pc.contact_records));
        return;
    }
    // Emissive proxies.
    uint s = t - lv.light_slots;
    if (s >= lv.instance_slots || s >= sc.instance_count || !(lv.emissive_threshold > 0.0) ||
        sc.materials == DE_BINDLESS_INVALID)
        return;
    DeGpuInstance inst = de_robuffer<DeGpuInstance>(sc.instances)[s];
    if ((inst.flags & DE_GPU_INSTANCE_LIVE) == 0u || (inst.flags & DE_GPU_INSTANCE_HAS_BOUNDS) == 0u)
        return;
    uint slot = inst.materials[0].x;
    if (slot == DE_GPU_NONE || slot >= sc.material_count)
        return;
    DeGpuMaterial m = de_robuffer<DeGpuMaterial>(sc.materials)[slot];
    if ((m.flags & DE_GPU_MATERIAL_READY) == 0u)
        return;
    float3 radiance = m.emissive.rgb * m.emissive.w; // nits
    float luminance = dot(radiance, LUMA);
    float3 e = inst.aabb_max.xyz - inst.aabb_min.xyz;
    float area = 2.0 * (e.x * e.y + e.y * e.z + e.z * e.x);
    if (!(luminance * area > lv.emissive_threshold))
        return;
    uint p = de_rwbytes(pc.counters).atomic_add(DE_LIGHTING_COUNTER_PROXIES, 1u);
    if (p >= lv.proxy_capacity)
        return;
    float radius = max(inst.sphere.w, 1e-3);
    float3 intensity = radiance * (DE_LIGHT_PI * radius * radius); // cd: a sphere of radiance L
    float range = de_light_derived_range(max3(intensity), lv.cutoff);
    DeClusterLight l;
    l.position = float4(inst.sphere.xyz, range);
    l.direction = float4(0.0, 0.0, -1.0, radius);
    l.color = float4(intensity, -1.0);
    l.shape = float4(1.0, 0.0, 0.0, asfloat(DE_LIGHT_KIND_POINT));
    l.axis = float4(1.0, 0.0, 0.0, asfloat(DE_CLUSTER_LIGHT_PROXY));
    l.bound = float4(to_view(sc, inst.sphere.xyz), range);
    push_local(pc, lv, l);
}

/// True when local light `l` may light a point of the cluster with view-space bounds lo..hi.
public bool de_light_touches(DeClusterLight l, float3 lo, float3 hi)
{
    float3 c = l.bound.xyz;
    float r = l.bound.w;
    float3 q = clamp(c, lo, hi) - c;
    if (dot(q, q) > r * r)
        return false;
    if (de_light_kind(l) != DE_LIGHT_KIND_SPOT)
        return true;
    // Cone vs the AABB's bounding sphere (conservative; Wronski 2016).
    float3 sc = 0.5 * (lo + hi);
    float sr = length(0.5 * (hi - lo));
    float3 v = sc - c;
    float vlen2 = dot(v, v);
    float v1 = dot(v, l.axis.xyz);
    float cos_o = l.color.w;
    float sin_o = sqrt(saturate(1.0 - cos_o * cos_o));
    float dist = cos_o * sqrt(max(vlen2 - v1 * v1, 0.0)) - v1 * sin_o;
    bool angle_cull = dist > sr;
    bool front_cull = v1 > sr + r;
    bool back_cull = v1 < -sr;
    return !(angle_cull || front_cull || back_cull);
}

void cluster_bounds(CullParams pc, uint c, out float3 lo, out float3 hi)
{
    DeBytes b = de_bytes(pc.bounds);
    lo = asfloat(b.load3(c * 32u));
    hi = asfloat(b.load3(c * 32u + 16u));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_light_count(uint3 id : SV_DispatchThreadID, uniform CullParams pc)
{
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    uint c = id.x;
    if (c >= lv.cluster_count)
        return;
    float3 lo, hi;
    cluster_bounds(pc, c, lo, hi);
    // counters: read-write in this pass (the overflow atomic), so read through de_rwbytes
    uint n = min(de_rwbytes(pc.counters).load(DE_LIGHTING_COUNTER_LOCAL), lv.local_capacity);
    uint count = 0;
    for (uint i = 0; i < n; ++i)
        if (de_light_touches(de_robuffer<DeClusterLight>(pc.lights)[i], lo, hi))
            ++count;
    uint kept = min(count, lv.max_per_cluster);
    if (count > kept)
        de_rwbytes(pc.counters).atomic_add(DE_LIGHTING_COUNTER_OVERFLOW, count - kept);
    de_buffer<uint2>(pc.grid)[c] = uint2(0u, kept);
}

groupshared uint g_scan[DE_LIGHT_SCAN_GROUP];

[shader("compute")]
[numthreads(1024, 1, 1)]
void cs_light_scan(uint3 tid : SV_GroupThreadID, uniform CullParams pc)
{
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    uint t = tid.x;
    uint n = lv.cluster_count;
    uint chunk = (n + DE_LIGHT_SCAN_GROUP - 1u) / DE_LIGHT_SCAN_GROUP;
    uint first = t * chunk;
    uint sum = 0;
    for (uint i = 0; i < chunk; ++i)
        if (first + i < n)
            sum += de_buffer<uint2>(pc.grid)[first + i].y;
    g_scan[t] = sum;
    GroupMemoryBarrierWithGroupSync();
    // Hillis-Steele inclusive scan of the chunk sums.
    for (uint d = 1u; d < DE_LIGHT_SCAN_GROUP; d <<= 1)
    {
        uint v = t >= d ? g_scan[t - d] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_scan[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint offset = g_scan[t] - sum; // exclusive
    uint dropped = 0;
    for (uint i = 0; i < chunk; ++i)
    {
        uint c = first + i;
        if (c >= n)
            break;
        uint count = de_buffer<uint2>(pc.grid)[c].y;
        uint kept = offset >= lv.list_capacity ? 0u : min(count, lv.list_capacity - offset);
        dropped += count - kept;
        de_buffer<uint2>(pc.grid)[c] = uint2(min(offset, lv.list_capacity), kept);
        offset += count;
    }
    if (dropped > 0u)
        de_rwbytes(pc.counters).atomic_add(DE_LIGHTING_COUNTER_DROPPED, dropped);
    if (t == DE_LIGHT_SCAN_GROUP - 1u)
        de_rwbytes(pc.counters).store(DE_LIGHTING_COUNTER_ENTRIES, g_scan[t]);
}

// Distribute a capped set spatially rather than always favoring one coordinate half-space.
// This is a stable selection key, not an importance estimate.
uint light_position_key(float3 position)
{
    uint3 p = asuint(position);
    uint h = ((2166136261u ^ p.x) * 16777619u ^ p.y) * 16777619u;
    h = (h ^ p.z) * 16777619u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    return h ^ (h >> 16);
}

// A total order independent of compacted indices. Resolve spatial-key ties by the complete
// shading/culling payload, including signed zero. Equal records are interchangeable.
bool light_less(CullParams pc, uint a, uint b)
{
    DeClusterLight x = de_robuffer<DeClusterLight>(pc.lights)[a];
    DeClusterLight y = de_robuffer<DeClusterLight>(pc.lights)[b];
    uint xkey = light_position_key(x.position.xyz), ykey = light_position_key(y.position.xyz);
    if (xkey != ykey)
        return xkey < ykey;
    uint4 xx[6] = {asuint(x.position), asuint(x.direction), asuint(x.color),
                   asuint(x.shape), asuint(x.axis), asuint(x.bound)};
    uint4 yy[6] = {asuint(y.position), asuint(y.direction), asuint(y.color),
                   asuint(y.shape), asuint(y.axis), asuint(y.bound)};
    for (uint field = 0; field < 6; ++field)
        for (uint lane = 0; lane < 4; ++lane)
            if (xx[field][lane] != yy[field][lane])
                return xx[field][lane] < yy[field][lane];
    return false;
}

// Each cluster owns a disjoint list range. Use that range as a bounded max heap, without
// another allocation or an O(lights * cap) insertion sort.
void light_heap_down(CullParams pc, uint base, uint count, uint at, uint value)
{
    while (at * 2 + 1 < count) {
        uint child = at * 2 + 1;
        if (child + 1 < count &&
            light_less(pc, de_buffer<uint>(pc.list)[base + child],
                           de_buffer<uint>(pc.list)[base + child + 1]))
            ++child;
        uint next = de_buffer<uint>(pc.list)[base + child];
        if (!light_less(pc, value, next))
            break;
        de_buffer<uint>(pc.list)[base + at] = next;
        at = child;
    }
    de_buffer<uint>(pc.list)[base + at] = value;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_light_fill(uint3 id : SV_DispatchThreadID, uniform CullParams pc)
{
    DeLightingView lv = de_robuffer<DeLightingView>(pc.view)[pc.entry];
    uint c = id.x;
    if (c >= lv.cluster_count)
        return;
    uint2 cell = de_robuffer<uint2>(pc.grid)[c];
    if (cell.y == 0u)
        return;
    float3 lo, hi;
    cluster_bounds(pc, c, lo, hi);
    uint n = min(de_bytes(pc.counters).load(DE_LIGHTING_COUNTER_LOCAL), lv.local_capacity);
    uint k = 0;
    for (uint i = 0; i < n; ++i) {
        if (!de_light_touches(de_robuffer<DeClusterLight>(pc.lights)[i], lo, hi))
            continue;
        if (k < cell.y) {
            uint at = k++;
            while (at > 0) {
                uint parent = (at - 1) / 2;
                uint next = de_buffer<uint>(pc.list)[cell.x + parent];
                if (!light_less(pc, next, i))
                    break;
                de_buffer<uint>(pc.list)[cell.x + at] = next;
                at = parent;
            }
            de_buffer<uint>(pc.list)[cell.x + at] = i;
        } else if (light_less(pc, i, de_buffer<uint>(pc.list)[cell.x])) {
            light_heap_down(pc, cell.x, k, 0, i);
        }
    }
    // Heap sort produces the same summation order even when the cluster is not capped.
    for (uint end = k; end > 1; --end) {
        uint largest = de_buffer<uint>(pc.list)[cell.x];
        uint value = de_buffer<uint>(pc.list)[cell.x + end - 1];
        de_buffer<uint>(pc.list)[cell.x + end - 1] = largest;
        light_heap_down(pc, cell.x, end - 1, 0, value);
    }
}

// DENG-340: immutable probe records begin with world position/radius. Froxel bounds use the
// same S14 grid as lights. Lists retain stable priority/entity order and never truncate below
// the renderer's explicit 64-probe capacity.
struct ProbeCullParams { uint bounds; uint probes; uint list; uint count; uint clusters; uint scene; uint slot; uint entry; uint bounds_offset; uint reserved0; uint reserved1; uint reserved2; };
[shader("compute")] [numthreads(64,1,1)]
void cs_probe_cluster_list(uint3 id:SV_DispatchThreadID,uniform ProbeCullParams pc) {
    if(id.x>=pc.clusters)return;
    DeSceneConstants sc=de_robuffer<DeSceneConstants>(pc.scene)[pc.slot];
    float3 lo=asfloat(de_bytes(pc.bounds).load4(pc.bounds_offset+id.x*32)).xyz;
    float3 hi=asfloat(de_bytes(pc.bounds).load4(pc.bounds_offset+id.x*32+16)).xyz;
    uint count=0;
    for(uint i=0;i<pc.count;++i) {
        float4 sphere=asfloat(de_bytes(pc.probes).load4((pc.entry*64+i)*80));
        float3 center=float3(dot(sc.view[0],float4(sphere.xyz,1)),dot(sc.view[1],float4(sphere.xyz,1)),dot(sc.view[2],float4(sphere.xyz,1)));
        float3 d=center-clamp(center,lo,hi);
        if(dot(d,d)<=sphere.w*sphere.w) de_rwbytes(pc.list).store((id.x*65+1+count++)*4,i);
    }
    de_rwbytes(pc.list).store(id.x*65*4,count);
}
