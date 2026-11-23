/*
 * celestial.c: the sun and the moon in the sky (DENG-206, SPEC §12.13).
 *
 * The sun follows NOAA's solar calculator (Meeus' low-precision series): better than 0.01 degrees
 * from 1900 to 2100. The moon follows Meeus, "Astronomical Algorithms", chapter 47, with the
 * periodic terms above about 0.002 degrees, and chapter 48 for the illuminated fraction; the
 * parallax correction makes the position topocentric. Both are geometric: no refraction.
 * Pure double-precision functions; no allocation.
 */
#include "world/sky/sky_internal.h"

#include <math.h>

#define DEG (3.14159265358979323846 / 180.0)

static f64 wrap360(f64 a) {
    a = fmod(a, 360.0);
    return a < 0.0 ? a + 360.0 : a;
}

f64 de__sky_julian_day(i32 year, i32 month, i32 day, f64 hours) {
    if (month <= 2) {
        year -= 1;
        month += 12;
    }
    const f64 a = floor((f64)year / 100.0);
    const f64 b = 2.0 - a + floor(a / 4.0);
    return floor(365.25 * (f64)(year + 4716)) + floor(30.6001 * (f64)(month + 1)) + (f64)day + b -
           1524.5 + hours / 24.0;
}

/* NOAA's intermediate solar quantities. */
typedef struct sun_terms {
    f64 l0, m, e, app_long, obliq, decl, eot_min, dist_au;
} sun_terms;

static sun_terms sun_compute(f64 jd) {
    sun_terms s;
    const f64 t = (jd - 2451545.0) / 36525.0;
    s.l0 = wrap360(280.46646 + t * (36000.76983 + t * 0.0003032));
    s.m = 357.52911 + t * (35999.05029 - 0.0001537 * t);
    s.e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    const f64 mr = s.m * DEG;
    const f64 c = sin(mr) * (1.914602 - t * (0.004817 + 0.000014 * t)) +
                  sin(2.0 * mr) * (0.019993 - 0.000101 * t) + sin(3.0 * mr) * 0.000289;
    const f64 true_long = s.l0 + c, true_anom = s.m + c;
    s.dist_au = 1.000001018 * (1.0 - s.e * s.e) / (1.0 + s.e * cos(true_anom * DEG));
    const f64 omega = 125.04 - 1934.136 * t;
    s.app_long = true_long - 0.00569 - 0.00478 * sin(omega * DEG);
    const f64 mean_obliq =
        23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0;
    s.obliq = mean_obliq + 0.00256 * cos(omega * DEG);
    s.decl = asin(sin(s.obliq * DEG) * sin(s.app_long * DEG)) / DEG;
    const f64 y = tan(s.obliq * DEG * 0.5) * tan(s.obliq * DEG * 0.5);
    const f64 l0r = s.l0 * DEG;
    s.eot_min = 4.0 / DEG *
                (y * sin(2.0 * l0r) - 2.0 * s.e * sin(mr) +
                 4.0 * s.e * y * sin(mr) * cos(2.0 * l0r) - 0.5 * y * y * sin(4.0 * l0r) -
                 1.25 * s.e * s.e * sin(2.0 * mr));
    return s;
}

f64 de__sky_equation_of_time(f64 jd) {
    return sun_compute(jd).eot_min;
}

/* Altitude and azimuth (from north, toward east) of hour angle `h` and declination `d`. */
static de__sky_horizontal to_horizontal(f64 h_deg, f64 d_deg, f64 lat_deg) {
    const f64 h = h_deg * DEG, d = d_deg * DEG, p = lat_deg * DEG;
    f64 sa = sin(p) * sin(d) + cos(p) * cos(d) * cos(h);
    sa = sa > 1.0 ? 1.0 : (sa < -1.0 ? -1.0 : sa);
    de__sky_horizontal o;
    o.altitude_deg = asin(sa) / DEG;
    /* From south toward west, then from north. */
    o.azimuth_deg = wrap360(atan2(sin(h), cos(h) * sin(p) - tan(d) * cos(p)) / DEG + 180.0);
    return o;
}

de__sky_horizontal de__sky_sun(f64 jd, f64 latitude_deg, f64 longitude_deg) {
    const sun_terms s = sun_compute(jd);
    const f64 day_frac = (jd + 0.5) - floor(jd + 0.5);
    const f64 tst = fmod(day_frac * 1440.0 + s.eot_min + 4.0 * longitude_deg, 1440.0);
    f64 ha = (tst < 0.0 ? tst + 1440.0 : tst) / 4.0 - 180.0;
    return to_horizontal(ha, s.decl, latitude_deg);
}

/* Meeus table 47.A: D, M, M', F, sum l (1e-6 deg), sum r (1e-3 km). */
static const i8 k_lr_args[][4] = {
    {0, 0, 1, 0},  {2, 0, -1, 0}, {2, 0, 0, 0},   {0, 0, 2, 0},  {0, 1, 0, 0},  {0, 0, 0, 2},
    {2, 0, -2, 0}, {2, -1, -1, 0}, {2, 0, 1, 0},  {2, -1, 0, 0}, {0, 1, -1, 0}, {1, 0, 0, 0},
    {0, 1, 1, 0},  {2, 0, 0, -2}, {0, 0, 1, 2},   {0, 0, 1, -2}, {4, 0, -1, 0}, {0, 0, 3, 0},
    {4, 0, -2, 0}, {2, 1, -1, 0}, {2, 1, 0, 0},   {1, 0, -1, 0}, {1, 1, 0, 0},  {2, -1, 1, 0},
    {2, 0, 2, 0},  {4, 0, 0, 0},  {2, 0, -3, 0},  {0, 1, -2, 0}, {2, 0, -1, 2}, {2, -1, -2, 0},
    {1, 0, 1, 0},  {2, -2, 0, 0}};
static const i32 k_lr_coef[][2] = {
    {6288774, -20905355}, {1274027, -3699111}, {658314, -2955968}, {213618, -569925},
    {-185116, 48888},     {-114332, -3149},    {58793, 246158},    {57066, -152138},
    {53322, -170733},     {45758, -204586},    {-40923, -129620},  {-34720, 108743},
    {-30383, 104755},     {15327, 10321},      {-12528, 0},        {10980, 79661},
    {10675, -34782},      {10034, -23210},     {8548, -21636},     {-7888, 24208},
    {-6766, 30824},       {-5163, -8379},      {4987, -16675},     {4036, -12831},
    {3994, -10445},       {3861, -11650},      {3665, 14403},      {-2689, -7003},
    {-2602, 0},           {2390, 10056},       {-2348, 6322},      {2236, -9884}};
/* Meeus table 47.B: D, M, M', F, sum b (1e-6 deg). */
static const i8 k_b_args[][4] = {
    {0, 0, 0, 1},  {0, 0, 1, 1},  {0, 0, 1, -1},  {2, 0, 0, -1},  {2, 0, -1, 1}, {2, 0, -1, -1},
    {2, 0, 0, 1},  {0, 0, 2, 1},  {2, 0, 1, -1},  {0, 0, 2, -1},  {2, -1, 0, -1}, {2, 0, -2, -1},
    {2, 0, 1, 1},  {2, 1, 0, -1}, {2, -1, -1, 1}, {2, -1, 0, 1},  {2, -1, -1, -1}};
static const i32 k_b_coef[] = {5128122, 280602, 277693, 173237, 55413, 46271, 32573, 17198, 9266,
                               8822,    8216,   4324,   4200,   -3359, 2463, 2211,  2065};

de__sky_moon_state de__sky_moon(f64 jd, f64 latitude_deg, f64 longitude_deg,
                                f64 phase_override) {
    const f64 t = (jd - 2451545.0) / 36525.0;
    const sun_terms s = sun_compute(jd);
    const f64 omega = 125.04 - 1934.136 * t;
    const f64 dpsi = -0.00478 * sin(omega * DEG);
    f64 lambda, beta, dist;
    if (phase_override >= 0.0) {
        lambda = s.app_long + 360.0 * phase_override;
        beta = 0.0;
        dist = 385000.56;
    } else {
        const f64 t2 = t * t, t3 = t2 * t, t4 = t3 * t;
        const f64 lp =
            218.3164477 + 481267.88123421 * t - 0.0015786 * t2 + t3 / 538841.0 - t4 / 65194000.0;
        const f64 d = 297.8501921 + 445267.1114034 * t - 0.0018819 * t2 + t3 / 545868.0 -
                      t4 / 113065000.0;
        const f64 m = 357.5291092 + 35999.0502909 * t - 0.0001536 * t2 + t3 / 24490000.0;
        const f64 mp =
            134.9633964 + 477198.8675055 * t + 0.0087414 * t2 + t3 / 69699.0 - t4 / 14712000.0;
        const f64 f =
            93.2720950 + 483202.0175233 * t - 0.0036539 * t2 - t3 / 3526000.0 + t4 / 863310000.0;
        const f64 a1 = 119.75 + 131.849 * t, a2 = 53.09 + 479264.290 * t,
                  a3 = 313.45 + 481266.484 * t;
        const f64 e = 1.0 - 0.002516 * t - 0.0000074 * t2;
        f64 sl = 0.0, sr = 0.0, sb = 0.0;
        for (size_t i = 0; i < sizeof k_lr_coef / sizeof k_lr_coef[0]; ++i) {
            const i8 *a = k_lr_args[i];
            const f64 arg = (a[0] * d + a[1] * m + a[2] * mp + a[3] * f) * DEG;
            const f64 ef = a[1] == 0 ? 1.0 : (a[1] == 1 || a[1] == -1 ? e : e * e);
            sl += k_lr_coef[i][0] * ef * sin(arg);
            sr += k_lr_coef[i][1] * ef * cos(arg);
        }
        for (size_t i = 0; i < sizeof k_b_coef / sizeof k_b_coef[0]; ++i) {
            const i8 *a = k_b_args[i];
            const f64 arg = (a[0] * d + a[1] * m + a[2] * mp + a[3] * f) * DEG;
            const f64 ef = a[1] == 0 ? 1.0 : (a[1] == 1 || a[1] == -1 ? e : e * e);
            sb += k_b_coef[i] * ef * sin(arg);
        }
        sl += 3958.0 * sin(a1 * DEG) + 1962.0 * sin((lp - f) * DEG) + 318.0 * sin(a2 * DEG);
        sb += -2235.0 * sin(lp * DEG) + 382.0 * sin(a3 * DEG) + 175.0 * sin((a1 - f) * DEG) +
              175.0 * sin((a1 + f) * DEG) + 127.0 * sin((lp - mp) * DEG) -
              115.0 * sin((lp + mp) * DEG);
        lambda = lp + sl / 1e6 + dpsi;
        beta = sb / 1e6;
        dist = 385000.56 + sr / 1000.0;
    }
    lambda = wrap360(lambda);
    const f64 eps = s.obliq * DEG, lr = lambda * DEG, br = beta * DEG;
    const f64 ra =
        atan2(sin(lr) * cos(eps) - tan(br) * sin(eps), cos(lr)) / DEG;
    const f64 dec = asin(sin(br) * cos(eps) + cos(br) * sin(eps) * sin(lr)) / DEG;
    const f64 jd0 = jd - 2451545.0;
    const f64 gmst = wrap360(280.46061837 + 360.98564736629 * jd0 + 0.000387933 * t * t -
                             t * t * t / 38710000.0 + dpsi * cos(eps));
    de__sky_moon_state o;
    o.pos = to_horizontal(gmst + longitude_deg - ra, dec, latitude_deg);
    /* Parallax: the observer is on the surface, not at the centre. */
    const f64 par = asin(6378.14 / dist) / DEG;
    o.pos.altitude_deg -= par * cos(o.pos.altitude_deg * DEG);
    o.distance_km = dist;
    const f64 r_sun = s.dist_au * 149597870.7;
    const f64 cpsi = cos(br) * cos((lambda - s.app_long) * DEG);
    const f64 psi = acos(cpsi < -1.0 ? -1.0 : (cpsi > 1.0 ? 1.0 : cpsi));
    const f64 inc = atan2(r_sun * sin(psi), dist - r_sun * cos(psi));
    o.illuminated_fraction = (1.0 + cos(inc)) * 0.5;
    o.phase = wrap360(lambda - s.app_long) / 360.0;
    return o;
}

f64 de__sky_local_solar_jd(f64 day_of_year, f64 hours) {
    const f64 jd0 = de__sky_julian_day(2026, 1, 1, 0.0) + (day_of_year - 1.0);
    f64 jd = jd0 + hours / 24.0;
    for (int i = 0; i < 2; ++i)
        jd = jd0 + (hours - de__sky_equation_of_time(jd) / 60.0) / 24.0;
    return jd;
}

de_vec3 de__sky_horizontal_dir(de__sky_horizontal h) {
    const f64 a = h.altitude_deg * DEG, z = h.azimuth_deg * DEG;
    return (de_vec3){(f32)(cos(a) * sin(z)), (f32)(cos(a) * cos(z)), (f32)sin(a)};
}
