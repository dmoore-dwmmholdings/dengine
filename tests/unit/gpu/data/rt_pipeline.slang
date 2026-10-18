// Native Vulkan/DXR counterpart to the explicit Metal function-table ABI test.
import bindless;
import push_constants;
struct Params { uint output, tlas; };
struct Payload { uint value; };
[shader("raygeneration")]
void raygen(uniform Params pc) {
    RayDesc ray;ray.Origin=float3(0,0,1);ray.Direction=float3(0,0,-1);
    ray.TMin=.001;ray.TMax=10;Payload p={};
    TraceRay(de_accel(pc.tlas),RAY_FLAG_NONE,255,0,1,0,ray,p);
    de_buffer<uint>(pc.output)[DispatchRaysIndex().x]=p.value;
}
[shader("miss")]
void miss(inout Payload p) {p.value=20;}
[shader("closesthit")]
void closest(inout Payload p,BuiltInTriangleIntersectionAttributes attributes) {p.value=10;}
[shader("anyhit")]
void any(inout Payload p,BuiltInTriangleIntersectionAttributes attributes) {
    if(attributes.barycentrics.x<.25)IgnoreHit();
}
