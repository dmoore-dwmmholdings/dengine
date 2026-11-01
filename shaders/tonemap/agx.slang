/* MIT License
Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
// AgX minimal analytic fit by Benjamin Wrensch / Missing Deadlines (MIT), based on
// Troy Sobotka's AgX. Standard -12.47393..4.026069 EV range, not the UE exposure variant.
// https://iolite-engine.com/blog_posts/minimal_agx_implementation
import "itonemapper";
public struct AgxTonemapper : ITonemapper {
    public float3 apply(float3 c, TonemapParams p, OutputInfo o) {
        float peak = de_tonemap_peak(o);
        c = mul(float3x3(0.8424790623,0.0784336,0.0792237451,
                        0.0423282423,0.8784686365,0.0791661275,
                        0.0423756549,0.0784336,0.8791429738), c / peak);
        c = saturate((log2(max(c, 1e-10)) + 12.47393) / 16.5);
        c = (((((15.5*c-40.14)*c+31.96)*c-6.868)*c+0.4298)*c+0.1191)*c-0.00232;
        c = max(c, 0.0);
        if (p.data0.x > 0.5) {
            float luma = dot(c, float3(0.2126,0.7152,0.0722));
            c = max(lerp(float3(luma), pow(c, 1.35), 1.4), 0.0);
        }
        c = mul(float3x3(1.1968790051,-0.0980208811,-0.0990297441,
                        -0.0528968518,1.1519031299,-0.0989611768,
                        -0.0529716355,-0.0980434501,1.1510736726), c);
        return saturate(pow(max(c, 0.0), 2.2)) * peak;
    }
}
