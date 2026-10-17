module contact_common;
import bindless;
public struct ContactLightRecord
{
    public uint2 entity;
    public uint kind;
    public uint channel;
    public float4 position_range;
    public float4 direction_radius;
};
public ContactLightRecord de_contact_record(uint metadata, uint index)
{
    // Header16 + record48; raw bytes avoid adding padding to the fixed 256-byte contract.
    DeBytes b = de_bytes(metadata);
    uint o = 16u + index * 48u;
    ContactLightRecord r;
    uint4 identity = b.load4(o);
    r.entity = identity.xy; r.kind = identity.z; r.channel = identity.w;
    r.position_range = asfloat(b.load4(o + 16u));
    r.direction_radius = asfloat(b.load4(o + 32u));
    return r;
}
public uint de_contact_find(uint metadata, uint2 entity)
{
    if (metadata == 0u) return 0u;
    uint count = min(de_bytes(metadata).load(8u), 4u);
    for (uint i = 0u; i < count; ++i)
        if (all(de_contact_record(metadata, 1u + i).entity == entity)) return i + 1u;
    return 0u;
}
