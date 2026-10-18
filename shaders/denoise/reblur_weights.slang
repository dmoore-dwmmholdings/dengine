module reblur_weights;
public float3 reblur_floor_factor(float3 factor){return max(factor,float3(.001));}
public float reblur_luminance(float3 c){return dot(c,float3(.2126,.7152,.0722));}
public float2 reblur_poisson(uint i){const float2 disk[8]={float2(-.326,-.406),float2(-.840,-.074),float2(-.696,.457),float2(-.203,.621),float2(.962,-.195),float2(.473,-.480),float2(.519,.767),float2(.185,-.893)};return disk[i&7u];}
public bool reblur_geometry(float3 a,float ra,float3 b,float rb,float3 delta){return dot(a,b)>=.9&&abs(ra-rb)<=.15&&abs(dot(delta,a))<.02;}
public float3 reblur_firefly(float3 value,float mean,float second,float count){float sigma=sqrt(max(second/max(count,1)-mean*mean,0)),limit=mean+3*sigma,l=reblur_luminance(value);return count>2&&l>limit?value*(limit/max(l,1e-20)):value;}
