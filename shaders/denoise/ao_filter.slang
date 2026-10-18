module ao_filter;
import bindless;
// Match the ray generator's closest reverse-Z representative within the low-quality cell.
public uint2 de_denoise_ao_representative(uint depth,uint2 pixel,uint scale,uint2 extent){
    uint2 best=min(pixel*scale,extent-1);float z=de_tex2d(depth).Load(int3(best,0)).x;
    for(uint y=0;y<scale;++y)for(uint x=0;x<scale;++x){uint2 q=min(pixel*scale+uint2(x,y),extent-1);float d=de_tex2d(depth).Load(int3(q,0)).x;if(d>z){z=d;best=q;}}
    return best;
}
public float de_denoise_ao_radius(float distance){return clamp(distance*2,1.0,4.0);}
