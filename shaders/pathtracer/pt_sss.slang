// DENG-73: RGB random-walk coefficients share the raster Burley mean-free-path mapping.
module pt_sss;
import "../sss/burley_profile";
import pt_volume;
// One entry decision and two inward-direction coordinates per path event.
// Reserve a high dimension range, separate from camera, light, fog and flight draws.
// At most 64*(1024+5) events use fewer than 200k dimensions in this range.
public uint3 pt_sss_entry_dimensions(uint event) {
    uint base=0x40000000u+event*3u;
    return uint3(base,base+1u,base+2u);
}
public struct PtSssCoefficients {public float3 sigma_s,sigma_a;};
public PtSssCoefficients pt_sss_coefficients(float3 radius,float3 albedo) {
    albedo=clamp(albedo,float3(0),float3(.999999));
    float3 d=max(de_burley_d3(radius,albedo),float3(1e-8));
    // Invert the semi-infinite diffusion albedo fit (van de Hulst): A = (1-sqrt(1-a))/(1+sqrt(1-a)).
    float3 a=1-pow((1-albedo)/(1+albedo),float3(2));
    float3 sigma_t=1/(d*sqrt(max(3*(1-a),float3(1e-12))));
    PtSssCoefficients c;c.sigma_s=sigma_t*a;c.sigma_a=sigma_t*(1-a);return c;
}
public struct PtSssState {public uint active,instance,steps;public float3 sigma_s,sigma_a;};
public struct PtSssFlight {public float distance;public float3 weight;public uint scattered;};
public PtSssFlight pt_sss_flight(PtSssState state,float boundary,float2 u) {
    float3 sigma=state.sigma_s+state.sigma_a;
    uint channel=min(uint(u.x*3),2u);float d=-log(max(1-u.y,1e-20))/max(sigma[channel],1e-20);
    PtSssFlight f;f.distance=min(d,boundary);f.scattered=d<boundary?1:0;
    float3 tr=exp(-sigma*f.distance);
    // Hero-like RGB channel mixture, with the same full mixture PDF for every channel.
    float pdf=f.scattered!=0?dot(sigma*tr,float3(1.0/3)):dot(tr,float3(1.0/3));
    f.weight=pdf>0?tr*(f.scattered!=0?state.sigma_s:float3(1))/pdf:float3(0);return f;
}
