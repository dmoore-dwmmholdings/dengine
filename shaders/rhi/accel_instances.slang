// DENG-42: public 60-byte handle records -> Vulkan/DXR native 64-byte instances.
// Address table entries are immutable per-recording {handle, reserved, address.lo, address.hi}.
#if defined(DE_ACCEL_FIXED_BINDINGS)
ByteAddressBuffer source_buffer : register(t0);
ByteAddressBuffer address_buffer : register(t1);
RWByteAddressBuffer dest_buffer : register(u0);
#else
import bindless;
import push_constants;
#endif
#if !defined(DE_ACCEL_FIXED_BINDINGS)
#define Load load
#define Store store
#endif
struct Params { uint input, output, addresses, count, address_count; };
[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID, uniform Params p)
{
    // Grid-stride traversal keeps dispatch dimensions within the portable 65535-group bound.
    uint stride = min((p.count + 63) / 64, 65535u) * 64;
    for (uint i = tid.x; i < p.count; i += stride) {
#if defined(DE_ACCEL_FIXED_BINDINGS)
    let src = source_buffer;
    let table = address_buffer;
    let dst = dest_buffer;
#else
    let src = de_bytes(p.input);
    let table = de_bytes(p.addresses);
    let dst = de_rwbytes(p.output);
    #endif
    uint handle = src.Load(i * 60 + 56);
    uint index = handle & 0xfffff;
    uint lo = 0, hi = 0;
    bool valid = false;
    if (index < p.address_count && handle != 0) {
        valid = table.Load(index * 16) == handle;
        if (valid) {
            lo = table.Load(index * 16 + 8);
            hi = table.Load(index * 16 + 12);
            valid = (lo | hi) != 0;
        }
    }
    for (uint j = 0; j < 14; ++j) {
        uint v = src.Load(i * 60 + j * 4);
        if (j == 12 && !valid) v &= 0xffffff;
        dst.Store(i * 64 + j * 4, v);
    }
    dst.Store(i * 64 + 56, valid ? lo : 0);
    dst.Store(i * 64 + 60, valid ? hi : 0);
    }
}
