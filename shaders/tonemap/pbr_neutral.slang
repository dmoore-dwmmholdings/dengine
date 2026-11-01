// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 The Khronos Group, Inc.
// Adapted for Slang ITonemapper and HDR-relative output by DEngine.
// License: shaders/tonemap/LICENSES/Apache-2.0.txt
// Khronos PBR Neutral reference constants and flare-removal construction:
// https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/pbrNeutral.glsl
import "itonemapper";
public struct PbrNeutralTonemapper : ITonemapper {
    public float3 apply(float3 c, TonemapParams p, OutputInfo o) {
        float limit = de_tonemap_peak(o);
        c /= limit;
        float x = min(c.r, min(c.g, c.b));
        c -= x < 0.08 ? x - 6.25 * x * x : 0.04;
        float peak = max(c.r, max(c.g, c.b));
        if (peak < 0.76) return c * limit;
        float compressed = 1.0 - 0.0576 / (peak - 0.52);
        c *= compressed / peak;
        float desaturate = 1.0 - 1.0 / (0.15 * (peak - compressed) + 1.0);
        return lerp(c, float3(compressed), desaturate) * limit;
    }
}
