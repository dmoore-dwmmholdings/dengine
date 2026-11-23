/*
 * test_celestial.c: the sky's celestial model and CPU atmosphere (DENG-206, SPEC §12.13).
 * The references are PyEphem 4.1 (UTC, geometric: pressure 0, topocentric moon).
 */
#include "de_test.h"

#include <dengine/de_base.h>
#include <dengine/de_math.h>

#include "world/sky/sky_internal.h"

#include <math.h>

typedef struct ref_row {
    i32 year, month, day;
    f64 hours, lat, lon;
    f64 sun_alt, sun_az, moon_alt, moon_az, moon_frac;
} ref_row;

static const ref_row k_ref[] = {
    {2026, 3, 20, 12.0, 45.0, 0.0, 44.9227, 177.3737, 51.5615, 150.1748, 0.0285},
    {2026, 6, 21, 18.5, 51.48, -0.0, 13.9580, 290.2301, 35.5429, 191.8973, 0.4885},
    {2026, 12, 21, 3.25, -33.87, 151.21, 69.2710, 294.8551, -32.8350, 83.3404, 0.8772},
    {2026, 9, 23, 15.0, 40.71, -74.01, 42.2267, 142.0453, -63.1380, 15.9694, 0.8994},
    {2026, 1, 15, 8.0, 64.13, -21.9, -16.4423, 104.5785, -8.3468, 143.8406, 0.1140},
    {2026, 7, 4, 22.75, 0.0, -78.5, 7.7194, 293.0268, -63.4787, 107.3208, 0.7842},
};
#define REF_N (sizeof k_ref / sizeof k_ref[0])

static f64 az_diff(f64 a, f64 b) {
    f64 d = fmod(fabs(a - b), 360.0);
    return d > 180.0 ? 360.0 - d : d;
}

TEST(celestial, test_celestial_sun_reference) {
    for (u32 i = 0; i < REF_N; ++i) {
        const ref_row *r = &k_ref[i];
        const f64 jd = de__sky_julian_day(r->year, r->month, r->day, r->hours);
        const de__sky_horizontal h = de__sky_sun(jd, r->lat, r->lon);
        EXPECT_TRUE((fabs(h.altitude_deg - r->sun_alt)) <= (0.1));
        EXPECT_TRUE((az_diff(h.azimuth_deg, r->sun_az)) <= (0.1));
    }
}

TEST(celestial, test_celestial_moon_phase) {
    for (u32 i = 0; i < REF_N; ++i) {
        const ref_row *r = &k_ref[i];
        const f64 jd = de__sky_julian_day(r->year, r->month, r->day, r->hours);
        const de__sky_moon_state m = de__sky_moon(jd, r->lat, r->lon, -1.0);
        EXPECT_TRUE((fabs(m.pos.altitude_deg - r->moon_alt)) <= (0.5));
        EXPECT_TRUE((az_diff(m.pos.azimuth_deg, r->moon_az) * cos(r->moon_alt * DE_PI / 180.0)) <= (0.5));
        EXPECT_TRUE((fabs(m.illuminated_fraction - r->moon_frac)) <= (0.01));
    }
    /* The phase override. */
    const f64 jd = de__sky_julian_day(2026, 3, 20, 0.0);
    EXPECT_TRUE((de__sky_moon(jd, 45.0, 0.0, 0.0).illuminated_fraction) <= (0.01));
    EXPECT_TRUE((de__sky_moon(jd, 45.0, 0.0, 0.5).illuminated_fraction) >= (0.99));
    EXPECT_FLOAT_EQ(de__sky_moon(jd, 45.0, 0.0, 0.25).illuminated_fraction, 0.5, 0.02);
    EXPECT_FLOAT_EQ(de__sky_moon(jd, 45.0, 0.0, 0.75).illuminated_fraction, 0.5, 0.02);
}

TEST(celestial, local_solar_noon) {
    /* DeSky time is local apparent solar time: at 12 h the sun is on the meridian. */
    for (f64 doy = 1.0; doy < 366.0; doy += 45.0) {
        const de__sky_horizontal h = de__sky_sun(de__sky_local_solar_jd(doy, 12.0), 45.0, 0.0);
        EXPECT_TRUE((az_diff(h.azimuth_deg, 180.0)) <= (0.5));
    }
    /* Equinox at latitude 45: noon altitude near 45, sunrise near 6 h, midnight far below. */
    EXPECT_FLOAT_EQ(de__sky_sun(de__sky_local_solar_jd(80.0, 12.0), 45.0, 0.0).altitude_deg, 45.0,
                    1.0);
    EXPECT_FLOAT_EQ(de__sky_sun(de__sky_local_solar_jd(80.0, 6.0), 45.0, 0.0).altitude_deg, 0.0,
                    1.5);
    EXPECT_TRUE((de__sky_sun(de__sky_local_solar_jd(80.0, 0.0), 45.0, 0.0).altitude_deg) < (-40.0));
}

TEST(celestial, horizontal_dir) {
    const de_vec3 up = de__sky_horizontal_dir((de__sky_horizontal){90.0, 0.0});
    EXPECT_FLOAT_EQ(up.z, 1.0f, 1e-5f);
    const de_vec3 east = de__sky_horizontal_dir((de__sky_horizontal){0.0, 90.0});
    EXPECT_FLOAT_EQ(east.x, 1.0f, 1e-5f);
    EXPECT_FLOAT_EQ(east.y, 0.0f, 1e-5f);
    const de_vec3 north = de__sky_horizontal_dir((de__sky_horizontal){0.0, 0.0});
    EXPECT_FLOAT_EQ(north.y, 1.0f, 1e-5f);
}

TEST(atmosphere, cpu_reference) {
    EXPECT_FLOAT_EQ(de__sky_mie_scale(1.0), 0.0, 1e-12);
    EXPECT_FLOAT_EQ(de__sky_mie_scale(2.0), 1.0, 1e-12);
    f64 z[3], h[3], z2[3];
    de__sky_transmittance_cpu(DE__SKY_BOTTOM, 1.0, 1.0, 1024, z);
    EXPECT_TRUE((z[0]) > (0.9));
    EXPECT_TRUE((z[0]) < (0.97));
    EXPECT_TRUE((z[2]) < (z[0])); /* blue scatters more */
    de__sky_transmittance_cpu(DE__SKY_BOTTOM, 0.0, 1.0, 1024, h);
    EXPECT_TRUE((h[0]) < (0.5 * z[0]));
    /* Converged: 1024 and 4096 steps agree. */
    de__sky_transmittance_cpu(DE__SKY_BOTTOM, 0.3, 1.0, 1024, z);
    de__sky_transmittance_cpu(DE__SKY_BOTTOM, 0.3, 1.0, 4096, z2);
    for (int k = 0; k < 3; ++k)
        EXPECT_TRUE((fabs(z[k] - z2[k])) <= (1e-4 * z2[k]));
    /* The LUT parameterisation covers the ground to the top, straight up at the top-left texel. */
    f64 r, mu;
    de__sky_transmittance_rmu(0.5 / DE__SKY_TRANSMITTANCE_W, 0.5 / DE__SKY_TRANSMITTANCE_H, &r, &mu);
    EXPECT_FLOAT_EQ(r, DE__SKY_BOTTOM, 1e-6);
    EXPECT_FLOAT_EQ(mu, 1.0, 1e-9);
    de__sky_transmittance_rmu(1.0 - 0.5 / DE__SKY_TRANSMITTANCE_W,
                              1.0 - 0.5 / DE__SKY_TRANSMITTANCE_H, &r, &mu);
    EXPECT_FLOAT_EQ(r, DE__SKY_TOP, 1e-6);
}
