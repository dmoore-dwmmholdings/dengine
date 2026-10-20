module history;
__exported import bindless;
__exported import "../visbuffer/gbuffer";
public struct DenoiseParams {
    public uint width,height,input,depth,normal,motion,material,hit;
    public uint previous,previous_moments,previous_length,previous_depth,previous_normal,previous_material;
    public uint output,moments,length,validity,variance,out_depth,out_normal,out_material;
    public uint reset,linear_depth,clamp_mode,step,scene,slot,visible,reserved;
    public float alpha_min,depth_relative,normal_cos,normal_power,luminance_scale,hit_scale,roughness_threshold,unused;
    public float4 inverse[4],previous_inverse[4];
    public float4 camera,origin_delta;
    public uint key,previous_key,out_key,zero_radius_passthrough;
    public uint position,previous_position,out_position,rgb_variance;
    public uint temporal_alpha,normal_world,channels,out_variance;
};
public DenoiseParams dn_load(uint buffer,uint entry){return de_robuffer<DenoiseParams>(buffer)[entry];}
public float2 denoise_previous_uv(float2 uv,float2 motion){return uv+float2(-.5,.5)*motion;}
public float3 dn_position(DenoiseParams p,int2 xy,float depth,bool previous){
    if(p.position!=0)return de_tex2d(previous?p.previous_position:p.position).Load(int3(xy,0)).xyz+(previous?p.origin_delta.xyz:float3(0));
    if(p.linear_depth!=0)return float3(float2(xy),depth);
    float2 uv=(float2(xy)+.5)/float2(p.width,p.height);float4 h=float4(uv*float2(2,-2)+float2(-1,1),depth,1);
    float4 world;if(previous)world=float4(dot(p.previous_inverse[0],h),dot(p.previous_inverse[1],h),dot(p.previous_inverse[2],h),dot(p.previous_inverse[3],h));
    else world=float4(dot(p.inverse[0],h),dot(p.inverse[1],h),dot(p.inverse[2],h),dot(p.inverse[3],h));
    return world.xyz/(abs(world.w)>1e-20?world.w:1e-20)+(previous?p.origin_delta.xyz:float3(0));
}
public DeGbSurface dn_surface(DenoiseParams p,uint normal,int2 xy){float4 v=de_tex2d(normal).Load(int3(xy,0));if(p.normal_world!=0){DeGbSurface s=(DeGbSurface)0;s.normal=dot(v.xyz,v.xyz)>1e-12?normalize(v.xyz):float3(0,0,1);s.roughness=v.w;return s;}return gb_decode(float4(0),v,float4(0));}
public float dn_luma(float3 c){return dot(c,float3(.2126,.7152,.0722));}
// Visibility and distance are independent scalars, not red/green radiance.
public bool dn_scalar_signal(DenoiseParams p){return p.channels==1||p.channels==2;}
public float dn_signal_luma(DenoiseParams p,float3 c){return dn_scalar_signal(p)?c.x:dn_luma(c);}
public float3 dn_ycocg(float3 c){return float3(dot(c,float3(.25,.5,.25)),.5*(c.r-c.b),-.25*c.r+.5*c.g-.25*c.b);}
public float3 dn_rgb(float3 c){return float3(c.x+c.y-c.z,c.x+c.z,c.x-c.y-c.z);}
