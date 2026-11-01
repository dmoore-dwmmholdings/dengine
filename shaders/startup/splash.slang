// DENG-703: an asset-free, resolution-independent brand reveal. Analytic
// strokes use pixel derivatives, so the mark stays clean at every DPI.
import push_constants;
struct SplashPush { float width, height, alpha, reveal, settle, text_alpha, background, pad; };
struct SplashVertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
[shader("vertex")]
SplashVertex vs_main(uint id : SV_VertexID, uniform SplashPush p) {
    float2 uv = float2(float((id << 1) & 2u), float(id & 2u));
    SplashVertex v; v.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1); v.uv = uv; return v;
}
float stroke(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    return length(p - a - ab * saturate(dot(p - a, ab) / dot(ab, ab)));
}
float letterD(float2 p) {
    float d = stroke(p, float2(-.35, -.5), float2(-.35, .5));
    float ring = abs(length(float2((p.x + .35) / .65, p.y / .5)) - 1) * .5;
    return min(d, max(ring, -.35 - p.x));
}
float letterE(float2 p) {
    float d = stroke(p, float2(-.3,-.5), float2(-.3,.5));
    d = min(d, stroke(p, float2(-.3,-.5), float2(.3,-.5)));
    d = min(d, stroke(p, float2(-.3,0), float2(.2,0)));
    return min(d, stroke(p, float2(-.3,.5), float2(.3,.5)));
}
float letterN(float2 p) {
    return min(stroke(p, float2(-.3,-.5),float2(-.3,.5)),
        min(stroke(p,float2(-.3,-.5),float2(.3,.5)),stroke(p,float2(.3,-.5),float2(.3,.5))));
}
float letterG(float2 p) {
    float ring = abs(length(float2(p.x / .38, p.y / .5)) - 1) * .38;
    float gap = max(p.x, -p.y);
    float d = max(ring, gap > 0 ? min(p.x, -p.y) : -1);
    d = min(d, stroke(p,float2(.02,.04),float2(.36,.04)));
    return min(d,stroke(p,float2(.36,.04),float2(.36,.38)));
}
float letterI(float2 p) { return stroke(p,float2(0,-.5),float2(0,.5)); }
float wordmark(float2 p) {
    float result = 100;
    [unroll] for (int i = 0; i < 7; ++i) {
        float2 q = (p - float2((float(i) - 3) * .48, 1.35)) / .43;
        float d = i == 0 ? letterD(q) : i == 1 || i == 6 ? letterE(q) :
            i == 2 || i == 5 ? letterN(q) : i == 3 ? letterG(q) : letterI(q);
        result = min(result, (d - .045) * .43);
    }
    return result;
}
[shader("fragment")]
float4 ps_main(SplashVertex v, uniform SplashPush p) : SV_Target {
    float unit = max(1, min(min(p.width, p.height) * .12, 90));
    float2 xy = (v.uv * float2(p.width,p.height) - float2(p.width,p.height) * .5) / unit;
    float2 mark = (xy - float2(.10, -.35 + (1-p.settle)*.12)) / lerp(.96,1,p.settle);
    float2 q = mark / 1.55;
    float d = letterD(q) * 1.55 - .072;
    float aa = max(fwidth(d), .0001);
    float ink = saturate(.5 - d/aa) * (1-smoothstep(.8, 1.1, mark.y + 2*(1-p.reveal)));
    float rail = stroke(mark,float2(-.86,-.61),float2(-.86,.61)) - .035;
    float accent = saturate(.5 - rail/max(fwidth(rail),.0001));
    float text = wordmark(xy);
    float letters = saturate(.5 - text/max(fwidth(text),.0001)) * p.text_alpha;
    float3 bg = float3(.0031,.0045,.0080) * p.background;
    float3 color = lerp(bg, float3(.78,.84,.94), ink * p.alpha);
    color = lerp(color, float3(.22,.43,.82), accent * p.alpha);
    color = lerp(color, float3(.62,.69,.79), letters * p.alpha);
    return float4(color,1);
}
