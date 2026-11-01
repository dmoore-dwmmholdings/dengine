float easeBrand(float a,float b,float x){x=saturate((x-a)/(b-a));return x*x*(3-2*x);}
float3 rotateBrand(float3 q,float time){
 float angle=lerp(-.48,.14,easeBrand(.05,.67,time));
 float c=cos(angle),s=sin(angle);
 q=float3(c*q.x-s*q.z,q.y,s*q.x+c*q.z);
 return float3(q.x,.9928*q.y+.1197*q.z,-.1197*q.y+.9928*q.z);
}
float brandDistance(float3 position,BrandData p){
 float3 q=rotateBrand(position,p.time);
 float2 uv=float2(q.x*.5+.5,.5-q.y*.5);
 float2 box=max(abs(q.xy)-1,0);
 float2 rg=brandAtlas(float2(clamp(uv.x,.00097656,.99902344),clamp(uv.y*.6666667,.00065104,.666015625)),p).rg;
 float field=(dot(rg,float2(256,1))/257-.5)*.5+length(box);
 float2 shape=float2(field,abs(q.z)-.16);
 return min(max(shape.x,shape.y),0)+length(max(shape,0))-.025;
}
float3 brandNormal(float3 q,BrandData p){
 float e=.014;
 return normalize(float3(brandDistance(q+float3(e,0,0),p)-brandDistance(q-float3(e,0,0),p),
 brandDistance(q+float3(0,e,0),p)-brandDistance(q-float3(0,e,0),p),
 brandDistance(q+float3(0,0,e),p)-brandDistance(q-float3(0,0,e),p)));
}
float brandText(float2 screen,float2 center,float2 size,float band,BrandData p){
 float2 uv=(screen-center)/size+.5;
 if(uv.x<0||uv.y<0||uv.x>1||uv.y>1)return 0;
 return brandAtlas(float2(uv.x,(band+1-uv.y)/6),p).b;
}
float4 brandPixel(float2 uv,BrandData p){
 float2 screen=(uv-.5)*float2(p.width,p.height)/min(p.width,p.height);
 screen.y=-screen.y;
 float time=p.time;
 float ambient=exp(-dot((screen-float2(0,.1))*float2(1.2,1), (screen-float2(0,.1))*float2(1.2,1))*9);
 float3 color=float3(.0013,.0016,.0024)+float3(.001,.003,.007)*ambient*easeBrand(.03,.4,time);
 float camera=lerp(4.35,3.95,easeBrand(0,.72,time));
 float3 ro=float3(0,0,camera);
 float3 rd=normalize(float3(screen.x*3.1,(screen.y-.11)*3.1,-3));
 float travel=0;
 bool hit=false;
 [loop]for(int i=0;i<70;i++){
  float distance=brandDistance(ro+rd*travel,p);
  if(distance<.0007){hit=true;break;}
  travel+=max(distance*.8,.001);
  if(travel>7)break;
 }
 if(hit){
  float3 point=ro+rd*travel;
  float3 n=brandNormal(point,p);
  float3 view=-rd;
  float3 light=normalize(float3(lerp(-3.7,2.7,easeBrand(.08,.60,time)),2.4,4));
  float diffuse=max(dot(n,light),0);
  float spec=pow(max(dot(n,normalize(light+view)),0),72);
  float rim=pow(1-saturate(dot(n,view)),3);
  float reveal=easeBrand(.02,.37,time);
  float brush=.98+.02*sin(rotateBrand(point,time).y*1300);
  float sweep=exp(-pow((point.x+point.y*.35-lerp(-1.4,1.4,easeBrand(.08,.62,time)))*5,2));
  float3 metal=float3(.045,.065,.10)*(.14+diffuse*.85)*brush;
  metal+=float3(.91,.94,1)*spec*1.8;
  float3 reflected=reflect(-view,n);
  float studio=exp(-pow((reflected.x+reflected.y*.25-lerp(-.4,.40,easeBrand(.08,.62,time)))*3.2,2));
  metal+=float3(.27,.33,.43)*studio*.72;
  metal+=float3(.12,.36,.68)*(rim*.9+sweep*.13);
  float edgeLight=pow(saturate(1-abs(dot(n,normalize(float3(-.5,1.2,1)))-.91)/.12),3);
  metal+=float3(.76,.84,1)*edgeLight*.6;
  color=lerp(color,metal,reveal);
 }
 float word=brandText(screen,float2(0,-.32),float2(.61,.1525),4,p);
 float caption=brandText(screen,float2(0,-.222),float2(.40,.10),5,p);
 color=lerp(color,float3(.81,.84,.90),word*p.text_alpha);
 color=lerp(color,float3(.30,.36,.47),caption*p.text_alpha);
 float glint=exp(-pow((time-.43)/.045,2));
 color+=float3(.25,.48,.8)*exp(-abs(screen.y-.22)*650)*exp(-abs(screen.x-.16)*18)*glint*.40;
 color=(1-exp(-color*1.1))*p.alpha;
 return float4(color,1);
}
