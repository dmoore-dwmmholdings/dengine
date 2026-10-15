/* vis_scenes.c — golden scenes of the S13 visibility buffer (DENG-240, SPEC §19.2, §12.5 steps
 * 1-4). Each scene runs the vis frame for several frames through vis_golden.h, checks in C what
 * the picture cannot show, and leaves one picture in the harness target for the FLIP compare:
 *
 *   vis_cull_frustum         instances around and behind a camera that turns a full circle in six
 *                            frames: culling on == culling off on every frame
 *   vis_cull_occlusion_path  a city block grid (building boxes, props behind them), a scripted
 *                            camera walk of 30 frames down a street: culling on (two-phase
 *                            occlusion) == culling off on every frame
 *   vis_cull_small           1600 small spheres (8 cm) 60 to 220 m away (sub-pixel meshlets): on ==
 * off, and the small-meshlet test removes meshlets vis_paths_equal          mixed meshes: single-
 * and double-sided, mirrored, alpha-tested cutout cards; the compute path on == off everywhere, and
 * the mesh-shader path
 *                            == the compute path on Metal
 *   vis_gbuffer_debug_views  the thin G-buffer of DENG-238's resolve (resolve debug readback):
 *                            normal, base colour and motion panels
 *   vis_stats_sanity         a wall with props in front and behind: instance stats per frame in
 *                            their expected ranges; phase 2 draws everything on the first frame
 *                            (no history) and nothing once the history holds the occluders
 *
 * Pictures: the decoded vis IDs as flat colours (vg_show_ids), or the G-buffer panels. The checks
 * compare decoded (instance, triangle) pixels with a budget of 0.01 % edge ties (VG_TIE_BUDGET);
 * anything over it fails the scene. Meshes are procedural, cooked through the S11 model importer
 * (tests/golden/visbuffer/assets/README).
 */
#include "vis_golden.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979f

static de_vec3 v3(f32 x, f32 y, f32 z) {
    return (de_vec3){x, y, z};
}

/* One frame with both variants; checks on == off. */
static bool frame_on_off(vg *v, const char *path, const char *what, u32 frame) {
    if (!vg_frame(v, &(vg_frame_opts){.on = true, .off = true, .path = path}))
        return false;
    u32 fx = 0, fy = 0;
    const u32 diff = vg_compare(v, &v->img[VG_ON], &v->img[VG_OFF], &fx, &fy);
    return vg_check_budget(v, what, frame, diff, fx, fy);
}

/* ================================================================================================
 * vis_cull_frustum
 * ============================================================================================== */

DE_GOLDEN_SCENE(vis_cull_frustum) {
    vg v;
    if (!vg_open(&v, ctx, 256) || vg_skip_without_cull_off(&v)) {
        vg_close(&v);
        return;
    }
    const u32 box = vg_mesh_box(&v, v3(0.5f, 0.5f, 0.5f), 3);
    const u32 ball = vg_mesh_sphere(&v, 0.6f, 24);
    const u32 mat = vg_material(&v, 0.8f, 0.8f, 0.8f, false);
    if (box == 0 || ball == 0 || mat == 0) {
        vg_close(&v);
        return;
    }
    /* Three rings around the camera: 16, 24 and 32 instances at 5, 9 and 14 m. */
    u32 s = 0;
    const u32 count[3] = {16, 24, 32};
    const f32 radius[3] = {5.0f, 9.0f, 14.0f};
    for (u32 r = 0; r < 3; ++r)
        for (u32 k = 0; k < count[r]; ++k, ++s) {
            const f32 a = 2.0f * PI * ((f32)k + 0.5f * (f32)r) / (f32)count[r];
            const f32 z = 1.5f + 1.2f * sinf(3.0f * a + (f32)r);
            vg_set(&v, s, (s & 1u) ? ball : box, v3(radius[r] * cosf(a), radius[r] * sinf(a), z), a,
                   v3(1.0f, 1.0f + 0.3f * (f32)r, 1.0f), mat);
        }
    v.eye = v3(0.0f, 0.0f, 1.6f);
    u32 visible_min = UINT32_MAX;
    for (u32 f = 0; f < 6; ++f) {
        const f32 yaw = 2.0f * PI * (f32)f / 6.0f + 0.2f;
        v.target = de_v3_add(v.eye, v3(cosf(yaw), sinf(yaw), -0.1f));
        if (!frame_on_off(&v, "auto", "cull_frustum", f))
            break;
        const de__vis_stats *st = &v.img[VG_ON].stats;
        if (st->valid && st->instances_visible < visible_min)
            visible_min = st->instances_visible;
    }
    if (!de_golden_stopped(ctx)) {
        printf("  cull_frustum: the fewest instances drawn in a frame: %u of %u\n", visible_min, s);
        if (visible_min == 0 || visible_min >= s)
            de_golden_fail(ctx, "frustum culling drew %u of %u instances (expected some, not all)",
                           visible_min, s);
        vg_show_ids(&v, &v.img[VG_ON]);
    }
    vg_close(&v);
}

/* ================================================================================================
 * The city (vis_cull_occlusion_path, vis_stats_sanity's neighbour)
 * ============================================================================================== */

/* A 5 x 5 grid of 8 x 8 m blocks with 6 m streets; a building per block, a prop sphere in each
 * block's corner courtyard and on the cross streets. Returns the slots used. */
static u32 build_city(vg *v, u32 ground_mat, u32 wall_mat, u32 prop_mat) {
    const u32 ground = vg_mesh_plane(v, 90.0f, 8, 1.0f);
    const u32 block = vg_mesh_box(v, v3(4.0f, 4.0f, 0.5f), 2);
    const u32 prop = vg_mesh_sphere(v, 0.8f, 20);
    if (ground == 0 || block == 0 || prop == 0)
        return 0;
    u32 s = 0;
    vg_set(v, s++, ground, v3(0, 0, 0), 0.0f, v3(1, 1, 1), ground_mat);
    for (i32 j = -2; j <= 2; ++j)
        for (i32 i = -2; i <= 2; ++i) {
            const f32 cx = (f32)i * 14.0f + 7.0f, cy = (f32)j * 14.0f;
            const f32 h = 6.0f + (f32)(((i + 3) * 7 + (j + 3) * 5) % 9); /* 6 .. 14 m */
            vg_set(v, s++, block, v3(cx, cy, 0.5f * h), 0.0f, v3(1.0f, 1.0f, h), wall_mat);
            vg_set(v, s++, prop, v3(cx + 5.5f, cy + 5.5f, 0.8f), 0.0f, v3(1, 1, 1), prop_mat);
            vg_set(v, s++, prop, v3(cx - 2.0f, cy + 7.0f, 1.2f), 0.0f, v3(1, 1, 1.5f), prop_mat);
        }
    return s;
}

DE_GOLDEN_SCENE(vis_cull_occlusion_path) {
    vg v;
    if (!vg_open(&v, ctx, 128) || vg_skip_without_cull_off(&v)) {
        vg_close(&v);
        return;
    }
    const u32 grey = vg_material(&v, 0.35f, 0.35f, 0.35f, false);
    const u32 wall = vg_material(&v, 0.7f, 0.6f, 0.5f, false);
    const u32 prop = vg_material(&v, 0.2f, 0.5f, 0.8f, false);
    const u32 slots = grey && wall && prop ? build_city(&v, grey, wall, prop) : 0;
    if (slots == 0) {
        vg_close(&v);
        return;
    }
    /* The walk: down the street at x = 0 (between block columns), 1.7 m high, 2 m per frame (below
     * r.occlusion_cut_m), swaying the view left and right. */
    u32 hidden_frames = 0;
    for (u32 f = 0; f < 30; ++f) {
        v.eye = v3(0.0f, -40.0f + 2.0f * (f32)f, 1.7f);
        const f32 sway = 0.6f * sinf(0.35f * (f32)f);
        v.target = de_v3_add(v.eye, v3(sway, 1.0f, -0.05f));
        if (!frame_on_off(&v, "auto", "cull_occlusion_path", f))
            break;
        const de__vis_stats *on = &v.img[VG_ON].stats, *off = &v.img[VG_OFF].stats;
        if (on->valid && off->valid && on->frame == off->frame &&
            on->instances_visible < off->instances_visible)
            ++hidden_frames;
    }
    if (!de_golden_stopped(ctx)) {
        printf("  cull_occlusion_path: frames where occlusion drew fewer instances: %u of 30\n",
               hidden_frames);
        if (hidden_frames < 20)
            de_golden_fail(ctx, "occlusion culled something in only %u of 30 frames",
                           hidden_frames);
        vg_show_ids(&v, &v.img[VG_ON]);
    }
    vg_close(&v);
}

/* ================================================================================================
 * vis_cull_small
 * ============================================================================================== */

DE_GOLDEN_SCENE(vis_cull_small) {
    vg v;
    if (!vg_open(&v, ctx, 1664) || vg_skip_without_cull_off(&v)) {
        vg_close(&v);
        return;
    }
    const u32 dot = vg_mesh_sphere(&v, 0.08f, 12);
    const u32 big = vg_mesh_box(&v, v3(1.0f, 1.0f, 1.0f), 2);
    const u32 mat = vg_material(&v, 0.9f, 0.9f, 0.6f, false);
    if (dot == 0 || big == 0 || mat == 0) {
        vg_close(&v);
        return;
    }
    u32 s = 0;
    for (u32 j = 0; j < 40; ++j)
        for (u32 i = 0; i < 40; ++i) {
            const f32 d = 60.0f + 4.0f * (f32)j; /* 60 .. 216 m away */
            /* Across the view; each row shifted, so the dots do not line up in screen columns. */
            const f32 x = ((f32)i - 19.5f + 0.23f * (f32)(j % 5u)) * (d / 40.0f);
            const f32 z = 1.0f + 0.25f * (f32)((i * 7u + j * 3u) % 11u) * (d / 60.0f);
            vg_set(&v, s++, dot, v3(x, d, z), 0.0f, v3(1, 1, 1), mat);
        }
    for (u32 k = 0; k < 4; ++k)
        vg_set(&v, s++, big, v3(-6.0f + 4.0f * (f32)k, 20.0f + 10.0f * (f32)k, 1.0f), 0.3f * (f32)k,
               v3(1, 1, 1), mat);
    v.eye = v3(0.0f, 0.0f, 3.0f);
    v.target = v3(0.0f, 100.0f, 3.0f);
    u32 culled_more = 0;
    for (u32 f = 0; f < 3; ++f) {
        if (!frame_on_off(&v, "auto", "cull_small", f))
            break;
        const de__vis_meshlet_stats *on = &v.img[VG_ON].mstats, *off = &v.img[VG_OFF].mstats;
        if (on->valid && off->valid && on->visible < off->visible)
            culled_more = off->visible - on->visible;
    }
    if (!de_golden_stopped(ctx)) {
        printf("  cull_small: meshlets the small-meshlet test removed: %u\n", culled_more);
        if (culled_more == 0)
            de_golden_fail(ctx,
                           "the small-meshlet test removed no meshlet of 1600 distant spheres");
        vg_show_ids(&v, &v.img[VG_ON]);
    }
    vg_close(&v);
}

/* ================================================================================================
 * vis_paths_equal
 * ============================================================================================== */

static void paths_world(vg *v, u32 box, u32 ball, u32 card, u32 opaque, u32 two_sided, u32 cutout) {
    u32 s = 0;
    for (u32 k = 0; k < 4; ++k) {
        vg_set(v, s++, (k & 1u) ? ball : box, v3(-4.5f + 3.0f * (f32)k, 9.0f, 0.0f), 0.4f * (f32)k,
               v3(1, 1, 1), opaque);
    }
    /* A mirrored box (negative determinant: the winding flips). */
    vg_set(v, s++, box, v3(-3.0f, 6.0f, 2.2f), 0.5f, v3(-1.0f, 1.0f, 1.0f), opaque);
    /* A card seen from behind: single-sided (culled) and double-sided (drawn). */
    vg_set(v, s++, card, v3(1.5f, 7.0f, 2.2f), PI, v3(1, 1, 1), opaque);
    vg_set(v, s++, card, v3(4.0f, 7.0f, 2.2f), PI, v3(1, 1, 1), two_sided);
    /* Cutout cards in front of the others, facing the camera and turned. */
    vg_set(v, s++, card, v3(-1.5f, 5.0f, -0.5f), 0.0f, v3(1, 1, 1), cutout);
    vg_set(v, s++, card, v3(1.5f, 5.5f, -0.6f), 0.7f, v3(1, 1, 1), cutout);
}

DE_GOLDEN_SCENE(vis_paths_equal) {
    vg v;
    if (!vg_open(&v, ctx, 16)) {
        vg_close(&v);
        return;
    }
    const u32 box = vg_mesh_box(&v, v3(0.8f, 0.8f, 0.8f), 3);
    const u32 ball = vg_mesh_sphere(&v, 1.0f, 28);
    const u32 card = vg_mesh_card(&v, 2.2f, 4, 2.0f);
    const u32 opaque = vg_material(&v, 0.8f, 0.3f, 0.3f, false);
    const u32 two = vg_material(&v, 0.3f, 0.8f, 0.3f, true);
    const u32 cut = vg_material_cutout(&v);
    if (!box || !ball || !card || !opaque || !two || !cut) {
        vg_close(&v);
        return;
    }
    paths_world(&v, box, ball, card, opaque, two, cut);
    v.eye = v3(0.0f, -2.0f, 1.0f);
    v.target = v3(0.0f, 8.0f, 0.5f);
    /* The compute path on every backend: culling on == off (on only where culling cannot be turned
     * off: Shipping). */
    for (u32 f = 0; f < 2; ++f) {
        const bool ok = v.can_cull_off
                            ? frame_on_off(&v, "compute", "paths compute on/off", f)
                            : vg_frame(&v, &(vg_frame_opts){.on = true, .path = "compute"});
        if (!ok)
            break;
    }
    if (de_golden_stopped(ctx)) {
        vg_close(&v);
        return;
    }
    if (v.img[VG_ON].raster_path != DE__VIS_RASTER_COMPUTE) {
        de_golden_fail(ctx, "r.raster_path compute ran path %u", v.img[VG_ON].raster_path);
        vg_close(&v);
        return;
    }
    vg_image compute = v.img[VG_ON];
    memset(&v.img[VG_ON], 0, sizeof v.img[VG_ON]); /* keep it: the next frame frees its own */
    const u32 covered = vg_covered(&v, &compute);
    /* The mesh-shader path == the compute path: Metal only (lavapipe and WARP have no mesh
     * shaders; on other devices the mesh path is checked by the unit tests). */
    if (ctx->backend == DE_GPU_METAL && ctx->caps->mesh_shaders) {
        for (u32 f = 0; f < 2 && !de_golden_stopped(ctx); ++f) {
            if (v.can_cull_off)
                (void)frame_on_off(&v, "mesh", "paths mesh on/off", 2u + f);
            else
                (void)vg_frame(&v, &(vg_frame_opts){.on = true, .path = "mesh"});
        }
        if (!de_golden_stopped(ctx)) {
            if (v.img[VG_ON].raster_path != DE__VIS_RASTER_MESH)
                de_golden_fail(ctx, "r.raster_path mesh ran path %u", v.img[VG_ON].raster_path);
            u32 fx = 0, fy = 0;
            const u32 diff = vg_compare(&v, &v.img[VG_ON], &compute, &fx, &fy);
            (void)vg_check_budget(&v, "mesh path == compute path", 3, diff, fx, fy);
        }
    } else {
        printf("  paths: mesh == compute not checked on %s (Metal only; %s)\n",
               de_gpu_backend_name(ctx->backend),
               ctx->caps->mesh_shaders ? "the check is Metal-only" : "no mesh shaders");
    }
    if (!de_golden_stopped(ctx)) {
        printf("  paths: %u pixels covered\n", covered);
        if (covered < (v.width * v.height) / 10u)
            de_golden_fail(ctx, "only %u pixels covered", covered);
        vg_show_ids(&v, &compute);
    }
    free(compute.key);
    vg_close(&v);
}

/* ================================================================================================
 * vis_gbuffer_debug_views
 * ============================================================================================== */

static f32 half_to_f32(u16 h) {
    const u32 s = (u32)(h >> 15) << 31, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    u32 bits;
    if (e == 0) {
        if (m == 0) {
            bits = s;
        } else { /* subnormal */
            f32 f = (f32)m / 1024.0f / 16384.0f;
            return (h >> 15) ? -f : f;
        }
    } else if (e == 31) {
        bits = s | 0x7F800000u | (m << 13);
    } else {
        bits = s | ((e + 112u) << 23) | (m << 13);
    }
    f32 f;
    memcpy(&f, &bits, 4);
    return f;
}

static u8 to_u8(f32 x) {
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    return (u8)(x * 255.0f + 0.5f);
}

DE_GOLDEN_SCENE(vis_gbuffer_debug_views) {
    vg v;
    if (!vg_open(&v, ctx, 16)) {
        vg_close(&v);
        return;
    }
    v.width = ctx->width / 3u;
    v.height = ctx->height;
    const u32 box = vg_mesh_box(&v, v3(0.7f, 0.7f, 0.7f), 2);
    const u32 floor_m = vg_mesh_plane(&v, 20.0f, 4, 1.0f);
    const u32 red = vg_material(&v, 0.8f, 0.1f, 0.1f, false);
    const u32 green = vg_material(&v, 0.1f, 0.7f, 0.2f, false);
    const u32 blue = vg_material(&v, 0.1f, 0.2f, 0.9f, false);
    const u32 grey = vg_material(&v, 0.5f, 0.5f, 0.5f, false);
    if (!box || !floor_m || !red || !green || !blue || !grey) {
        vg_close(&v);
        return;
    }
    vg_set(&v, 0, floor_m, v3(0, 6, 0), 0.0f, v3(1, 1, 1), grey);
    vg_set(&v, 1, box, v3(-2.0f, 6.0f, 0.7f), 0.5f, v3(1, 1, 1), red);
    vg_set(&v, 2, box, v3(0.0f, 7.5f, 0.7f), 0.0f, v3(1, 1, 2), green);
    vg_set(&v, 3, box, v3(2.0f, 6.0f, 0.7f), -0.4f, v3(1, 1, 1), blue);
    v.eye = v3(0.0f, -1.0f, 2.5f);
    v.target = v3(0.0f, 7.0f, 0.6f);
    if (!vg_frame(&v, &(vg_frame_opts){.on = true, .gbuffer = true})) {
        vg_close(&v);
        return;
    }
    /* Frame 2: the red box moves right and up (object motion); the camera stays. */
    vg_set(&v, 1, box, v3(-1.6f, 6.0f, 0.9f), 0.5f, v3(1, 1, 1), red);
    if (!vg_frame(&v, &(vg_frame_opts){.on = true, .gbuffer = true})) {
        vg_close(&v);
        return;
    }
    const vg_image *img = &v.img[VG_ON];
    const u32 pitch = ctx->width * 4u, w = v.width, h = v.height;
    u8 *rgba = (u8 *)calloc((size_t)pitch * ctx->height, 1);
    if (rgba == NULL || img->gb0 == NULL) {
        de_golden_fail(ctx, "no G-buffer read back");
        free(rgba);
        vg_close(&v);
        return;
    }
    u32 moving = 0, still_bad = 0;
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x) {
            const size_t i = (size_t)y * w + x;
            const bool surface = img->key[i] != VG_EMPTY;
            u8 *pn = rgba + (size_t)y * pitch + (size_t)x * 4u;
            u8 *pc = pn + (size_t)w * 4u;
            u8 *pm = pc + (size_t)w * 4u;
            pn[3] = pc[3] = pm[3] = 255;
            if (!surface)
                continue;
            u32 w1;
            memcpy(&w1, img->gb1 + i * 4u, 4);
            f32 n[3], rough = 0;
            de__gb1_unpack(w1, n, &rough);
            pn[0] = to_u8(0.5f + 0.5f * n[0]);
            pn[1] = to_u8(0.5f + 0.5f * n[1]);
            pn[2] = to_u8(0.5f + 0.5f * n[2]);
            memcpy(pc, img->gb0 + i * 4u, 3); /* base colour, sRGB-encoded as stored */
            u16 mo[2];
            memcpy(mo, img->mot + i * 4u, 4);
            const f32 mx = half_to_f32(mo[0]), my = half_to_f32(mo[1]);
            pm[0] = to_u8(0.5f + 8.0f * mx);
            pm[1] = to_u8(0.5f + 8.0f * my);
            pm[2] = 128;
            const u32 inst = (u32)(img->key[i] >> 32);
            const bool fast = fabsf(mx) + fabsf(my) > 1e-3f;
            moving += inst == 1u && fast;
            still_bad += inst != 1u && fast;
        }
    printf("  gbuffer: moving pixels of the moved box %u, other pixels with motion %u\n", moving,
           still_bad);
    if (moving < 50u)
        de_golden_fail(ctx, "the moved box has %u pixels with motion", moving);
    if (still_bad > 0u)
        de_golden_fail(ctx, "%u pixels of static instances have motion", still_bad);
    vg_present(&v, rgba, pitch);
    free(rgba);
    vg_close(&v);
}

/* ================================================================================================
 * vis_stats_sanity
 * ============================================================================================== */

DE_GOLDEN_SCENE(vis_stats_sanity) {
    vg v;
    if (!vg_open(&v, ctx, 128) || vg_skip_without_cull_off(&v)) {
        vg_close(&v);
        return;
    }
    const u32 wall_m = vg_mesh_box(&v, v3(1.0f, 0.25f, 1.0f), 2);
    const u32 ball = vg_mesh_sphere(&v, 0.4f, 16);
    const u32 ground = vg_mesh_plane(&v, 60.0f, 6, 1.0f);
    const u32 wall_mat = vg_material(&v, 0.6f, 0.6f, 0.65f, false);
    const u32 front_mat = vg_material(&v, 0.9f, 0.5f, 0.1f, false);
    const u32 back_mat = vg_material(&v, 0.1f, 0.5f, 0.9f, false);
    if (!wall_m || !ball || !ground || !wall_mat || !front_mat || !back_mat) {
        vg_close(&v);
        return;
    }
    /* A ground, a wall 24 m wide and 8 m high at y = 12 (12 x 4 segments of 2 x 2 m), 20 props in
     * front, 40 behind it (hidden from the camera). The ground in front of the wall keeps the HZB
     * texels along the wall's foot near: without it they would hold the background, and the props
     * just above the foot would (rightly, conservatively) pass. */
    u32 s = 0;
    vg_set(&v, s++, ground, v3(0.0f, 20.0f, 0.0f), 0.0f, v3(1, 1, 1), wall_mat);
    for (u32 j = 0; j < 4; ++j)
        for (u32 i = 0; i < 12; ++i)
            vg_set(&v, s++, wall_m, v3(-11.0f + 2.0f * (f32)i, 12.0f, 1.0f + 2.0f * (f32)j), 0.0f,
                   v3(1, 1, 1), wall_mat);
    const u32 walls = s; /* with the ground */
    for (u32 k = 0; k < 20; ++k)
        vg_set(&v, s++, ball, v3(-5.0f + 2.5f * (f32)(k % 5u), 5.0f + 1.5f * (f32)(k / 5u), 0.6f),
               0.0f, v3(1, 1, 1), front_mat);
    const u32 front = 20;
    for (u32 k = 0; k < 40; ++k)
        vg_set(&v, s++, ball, v3(-6.0f + 1.5f * (f32)(k % 8u), 16.0f + 2.0f * (f32)(k / 8u), 0.6f),
               0.0f, v3(1, 1, 1), back_mat);
    v.eye = v3(0.0f, 0.0f, 1.5f);
    v.target = v3(0.0f, 12.0f, 1.5f);
    /* Per frame: the instance stats of the frame they describe (read back a frame late). */
    de__vis_stats on[8], off[8];
    memset(on, 0, sizeof on);
    memset(off, 0, sizeof off);
    for (u32 f = 0; f < 7; ++f) {
        if (!frame_on_off(&v, "auto", "stats_sanity", f))
            break;
        const de__vis_stats *a = &v.img[VG_ON].stats, *b = &v.img[VG_OFF].stats;
        if (a->valid && a->frame < 8)
            on[a->frame] = *a;
        if (b->valid && b->frame < 8)
            off[b->frame] = *b;
    }
    if (de_golden_stopped(ctx)) {
        vg_close(&v);
        return;
    }
    const u32 drawable = walls + front; /* what the camera sees (no prop behind the wall shows) */
    for (u32 f = 0; f < 6; ++f)
        printf("  frame %u: on visible %u (phase 1 %u, phase 2 %u), off visible %u\n", f,
               on[f].instances_visible, on[f].instances_visible_phase1,
               on[f].instances_visible_phase2, off[f].instances_visible);
    /* Frame 0: no history, so phase 1 draws nothing and phase 2 every frustum-visible instance. */
    if (!on[0].valid || on[0].instances_visible_phase1 != 0 ||
        on[0].instances_visible_phase2 < drawable)
        de_golden_fail(ctx, "frame 0: phase 1 %u (want 0), phase 2 %u (want >= %u)",
                       on[0].instances_visible_phase1, on[0].instances_visible_phase2, drawable);
    for (u32 f = 2; f < 6; ++f) {
        if (!on[f].valid || !off[f].valid) {
            de_golden_fail(ctx, "frame %u: no stats read back", f);
            break;
        }
        /* Once the history holds the wall: everything from phase 1, the hidden props culled. */
        if (on[f].instances_visible_phase2 > 2u ||
            on[f].instances_visible_phase2 * 4u > on[0].instances_visible_phase2)
            de_golden_fail(ctx, "frame %u: phase 2 drew %u (frame 0: %u)", f,
                           on[f].instances_visible_phase2, on[0].instances_visible_phase2);
        if (on[f].instances_visible < drawable - 4u || on[f].instances_visible > drawable + 4u)
            de_golden_fail(ctx, "frame %u: %u instances drawn, expected about %u", f,
                           on[f].instances_visible, drawable);
        if (off[f].instances_visible < drawable + 30u)
            de_golden_fail(ctx, "frame %u: culling off drew only %u", f, off[f].instances_visible);
    }
    vg_show_ids(&v, &v.img[VG_ON]);
    vg_close(&v);
}
