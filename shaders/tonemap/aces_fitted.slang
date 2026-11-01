/* MIT License
Copyright (c) 2016 MJP

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
// Stephen Hill RRT/ODT fit, via MJP/BakingLab (MIT). Coefficients are row-major.
// https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl
import "itonemapper";
public struct AcesFittedTonemapper : ITonemapper {
    public float3 apply(float3 c, TonemapParams p, OutputInfo o) {
        float peak = de_tonemap_peak(o);
        c = mul(float3x3(0.59719,0.35458,0.04823,
                        0.07600,0.90834,0.01566,
                        0.02840,0.13383,0.83777), c / peak);
        c = (c * (c + 0.0245786) - 0.000090537) /
            (c * (0.983729 * c + 0.4329510) + 0.238081);
        c = mul(float3x3(1.60475,-0.53108,-0.07367,
                        -0.10208,1.10813,-0.00605,
                        -0.00327,-0.07276,1.07602), c);
        return saturate(c) * peak;
    }
}
