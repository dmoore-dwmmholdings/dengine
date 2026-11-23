/*
 * atmosphere.c: the CPU reference of the sky's atmosphere (DENG-206): the transmittance of
 * sky_common.slang's medium in double precision, and the transmittance LUT's parameterisation.
 * The sun light's colour and illuminance at the ground come from here too (sky.c).
 */
#include "world/sky/sky_internal.h"

#include <math.h>

f64 de__sky_mie_scale(f64 turbidity) {
    const f64 s = turbidity - 1.0;
    return s > 0.0 ? s : 0.0;
}

static f64 dist_to_top(f64 r, f64 mu) {
    const f64 disc = r * r * (mu * mu - 1.0) + DE__SKY_TOP * DE__SKY_TOP;
    const f64 d = -r * mu + sqrt(disc > 0.0 ? disc : 0.0);
    return d > 0.0 ? d : 0.0;
}

void de__sky_transmittance_cpu(f64 r, f64 mu, f64 mie_scale, u32 steps, f64 out_rgb[3]) {
    if (steps == 0)
        steps = 1;
    const f64 len = dist_to_top(r, mu), dt = len / (f64)steps;
    f64 dr = 0.0, dg = 0.0, db = 0.0;
    for (u32 i = 0; i < steps; ++i) {
        const f64 t = ((f64)i + 0.5) * dt;
        f64 h = sqrt(r * r + t * t + 2.0 * r * mu * t) - DE__SKY_BOTTOM;
        const f64 ho = fmax(0.0, 1.0 - fabs(h - DE__SKY_OZONE_CENTER) / DE__SKY_OZONE_HALF);
        h = h > 0.0 ? h : 0.0;
        const f64 hr = exp(-h / DE__SKY_RAYLEIGH_H), hm = exp(-h / DE__SKY_MIE_H) * mie_scale;
        const f64 mie = DE__SKY_MIE_EXT * hm;
        dr += DE__SKY_RAYLEIGH_R * hr + mie + DE__SKY_OZONE_R * ho;
        dg += DE__SKY_RAYLEIGH_G * hr + mie + DE__SKY_OZONE_G * ho;
        db += DE__SKY_RAYLEIGH_B * hr + mie + DE__SKY_OZONE_B * ho;
    }
    out_rgb[0] = exp(-dr * dt);
    out_rgb[1] = exp(-dg * dt);
    out_rgb[2] = exp(-db * dt);
}

void de__sky_transmittance_rmu(f64 u, f64 v, f64 *r, f64 *mu) {
    const f64 w = (f64)DE__SKY_TRANSMITTANCE_W, hh = (f64)DE__SKY_TRANSMITTANCE_H;
    const f64 x_mu = (u - 0.5 / w) / (1.0 - 1.0 / w);
    const f64 x_r = (v - 0.5 / hh) / (1.0 - 1.0 / hh);
    const f64 H = sqrt(DE__SKY_TOP * DE__SKY_TOP - DE__SKY_BOTTOM * DE__SKY_BOTTOM);
    const f64 rho = H * x_r;
    *r = sqrt(rho * rho + DE__SKY_BOTTOM * DE__SKY_BOTTOM);
    const f64 d_min = DE__SKY_TOP - *r, d_max = rho + H;
    const f64 d = d_min + x_mu * (d_max - d_min);
    f64 m = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * *r * d);
    *mu = m < -1.0 ? -1.0 : (m > 1.0 ? 1.0 : m);
}
