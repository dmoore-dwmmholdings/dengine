// DENG-431: fixed acceptance inputs call the real shared production path with bound resources.
import bindless;
import "lighting/lighting_common";
import "brdf/sheen";
import "ssr/ssr_common";
struct IblCase { float4 p0, p1, p2, p3, p4, p5, p6; };
struct IblParams { uint cases, output, count, lut, config, reserved, first, padding; };
[shader("compute")] [numthreads(64,1,1)]
void cs_ibl_layers(uint3 id:SV_DispatchThreadID, uniform IblParams pc) {
    uint index=id.x+pc.first;
    if(index>=pc.count)return;
    IblCase c;
    c.p0=de_robuffer<float4>(pc.cases)[index*7];c.p1=de_robuffer<float4>(pc.cases)[index*7+1];
    c.p2=de_robuffer<float4>(pc.cases)[index*7+2];c.p3=de_robuffer<float4>(pc.cases)[index*7+3];
    c.p4=de_robuffer<float4>(pc.cases)[index*7+4];c.p5=de_robuffer<float4>(pc.cases)[index*7+5];
    c.p6=de_robuffer<float4>(pc.cases)[index*7+6];
    uint mode=uint(c.p4.w);
    uint4 a=de_robuffer<uint4>(pc.config)[0], b=de_robuffer<uint4>(pc.config)[1];
    uint4 gb_indices=de_robuffer<uint4>(pc.config)[2];
    SsrParams producerParams=(SsrParams)0;producerParams.gb0=gb_indices.y;
    producerParams.gb1=gb_indices.z;producerParams.gb2=gb_indices.w;
    SurfaceData producer=ssr_surface(producerParams,uint2(index,0));
    SurfaceData s=producer;

    s.roughness=c.p0.x; s.metallic=c.p0.y; s.specular=c.p2.a;
    s.specular_color=float3(1); s.ior=1.5; s.brdf_lut=pc.lut;
    s.clearcoat=c.p1.x; s.clearcoat_roughness=c.p1.y; s.clearcoat_normal=c.p4.xyz;
    s.sheen_color=c.p2.rgb; s.sheen_roughness=c.p1.z;
    s.iridescence=c.p3.x; s.iridescence_ior=c.p3.y;
    s.iridescence_thickness=c.p3.z; s.features=uint(c.p3.w);
    float mu=c.p0.w; float3 v=float3(sqrt(max(1-mu*mu,0)),0,mu);
    DeLightingView lv=(DeLightingView)0;
    lv.brdf_lut=pc.lut; lv.tiles_x=1;lv.tiles_y=1;lv.slices=1;lv.tile_px=64;
    lv.near=.1;lv.log_scale=1;
    lv.ibl.ambient=float4(1);lv.ibl.rotation=float4(1,0,2,2);
    lv.ibl.sh[0].a=1;lv.ibl.face_size=4;lv.ibl.strip=a.x;lv.ibl.sampler=a.y;
    if((mode&1)!=0)lv.ibl.flags=DE_IBL_ENVIRONMENT;
    if((mode&2)!=0) {
        lv.inputs.probe_records=a.z;lv.inputs.probe_list=a.w;
        lv.inputs.probe_sampler=a.y;lv.inputs.probe_view_z=float4(0,0,-1,0);
    }
    if((mode&4)!=0)lv.inputs.reserved1=b.x;
    if((mode&8)!=0)lv.inputs.reserved0|=DE_INPUT_AO_GTAO;
    if((mode&72)!=0)lv.inputs.ao=b.y;
    if((mode&16)!=0) {
        lv.inputs.indirect_specular=(mode&128)!=0?gb_indices.x:b.z;
        if((mode&256)==0)lv.inputs.reserved0|=DE_INPUT_SPECULAR_OPAQUE;
    }
    if((mode&32)!=0) {
        lv.inputs.indirect_diffuse=(mode&512)!=0?de_robuffer<uint4>(pc.config)[3].x:b.w;
        if((mode&256)==0)lv.inputs.reserved0|=DE_INPUT_DIFFUSE_OPAQUE;
    }
    float3 d,sp;
    evaluate_indirect_split(s,float3(0),v,uint2(0),lv,d,sp);
    de_buffer<float4>(pc.output)[index*8]=float4(d,0);
    de_buffer<float4>(pc.output)[index*8+1]=float4(sp,0);
    de_buffer<float4>(pc.output)[index*8+2]=float4(ssr_response(producer,v,pc.lut),0);
    de_buffer<float4>(pc.output)[index*8+3]=float4(de_ibl_radiance(lv.ibl,de_ibl_reflection_direction(s,v),s.roughness),0);
    de_buffer<float4>(pc.output)[index*8+4]=float4(de_ibl_radiance(lv.ibl,de_ibl_coat_direction(s,v),s.clearcoat_roughness),0);
    de_buffer<float4>(pc.output)[index*8+5]=float4(de_ibl_radiance(lv.ibl,de_ibl_sheen_direction(s,v),de_sheen_roughness(s.sheen_roughness)),0);
    de_buffer<float4>(pc.output)[index*8+6]=float4(de_sheen_albedo(pc.lut,mu,s.sheen_roughness),0,0,0);
    // The SSGI filter's actual thin-G-buffer/unit-ambient diffuse producer contract.
    DeIbl unit = (DeIbl)0;
    unit.ambient = float4(1);
    float3 producerDiffuse, producerSpecular;
    de_ibl_evaluate(producer,v,unit,pc.lut,1,producerDiffuse,producerSpecular);
    de_buffer<float4>(pc.output)[index*8+7]=float4(producerDiffuse,0);
}
