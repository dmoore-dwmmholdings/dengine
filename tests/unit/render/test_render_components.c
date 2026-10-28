/*
 * test_render_components.c: the render components of de_render.h and their registration
 * (DENG-178; SPEC §9.1, §12.4, §20.1). CTest names render_components.<test>.
 *
 *   - registered: every component is found by name, a second call (and de_renderer_attach on the
 *     null backend) changes nothing, worlds share a registry, a foreign type of the same name is
 *     refused.
 *   - text_roundtrip: every serialized field of every component survives the S04 TOML writer and
 *     reader; asset handles are written as asset IDs; runtime handles are not written.
 *   - defaults: the construct hooks give the documented defaults, from zeroed and from garbage
 *     memory, also through de_entity_add.
 *   - asset_ref_flags: flags, asset types, units, ranges and enums of the reflected fields.
 *   - gltf_prefab_instantiate (cooker builds): hierarchy.gltf cooked in-process through the
 *     renderer's model type, its `#prefab` instantiated: the hierarchy with DeTransform,
 *     DeChildOf and DeMeshRenderer whose handles resolve to the model's mesh and material.
 * Scratch files go below <build>/tests/unit/render/test_render_components_tmp/ (mounted as rc:/).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_ecs.h>
#include <dengine/de_gpu.h>
#include <dengine/de_math.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>
#include <dengine/de_reflect.h>
#include <dengine/de_render.h>
#include <dengine/de_task.h>

#include "asset/load_internal.h"
#include "render/components_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_NS ((u64)30 * 1000 * 1000 * 1000)

/* Every component, with its number of reflected fields (one per struct member). */
static const struct {
    const char *name;
    u32 fields;
    size_t offset; /* in DeRenderComponents */
} k_comps[] = {
    {"DeCamera", 8, offsetof(DeRenderComponents, camera)},
    {"DeMeshRenderer", 5, offsetof(DeRenderComponents, mesh_renderer)},
    {"DeLight", 10, offsetof(DeRenderComponents, light)},
    {"DeEnvironment", 3, offsetof(DeRenderComponents, environment)},
    {"DeSprite", 16, offsetof(DeRenderComponents, sprite)},
    {"DeSky", 5, offsetof(DeRenderComponents, sky)},
    {"DeFogVolume", 4, offsetof(DeRenderComponents, fog_volume)},
    {"DeDecal", 4, offsetof(DeRenderComponents, decal)},
    {"DeParticleSystem", 4, offsetof(DeRenderComponents, particle_system)},
    {"DeTerrain", 5, offsetof(DeRenderComponents, terrain)},
    {"DeWater", 4, offsetof(DeRenderComponents, water)},
};
#define COMP_COUNT ((u32)(sizeof k_comps / sizeof k_comps[0]))

static de_component comp_id(const DeRenderComponents *ids, u32 i) {
    de_component c;
    memcpy(&c, (const u8 *)ids + k_comps[i].offset, sizeof c);
    return c;
}

/* ---- Fixture -------------------------------------------------------------------------------- */

typedef struct cfx {
    de_tracking_allocator track, dev_track;
    de_allocator alloc, dev_alloc;
    de_type_registry *types;
    de_world *w;
    de_task_system *tasks;
    de_asset_system *s;
    de_gpu_device *d;
    de_renderer *r;
} cfx;

static void cfx_setup(cfx *f) {
    static bool mounted;
    if (!mounted) {
        ASSERT_EQ(de_fs_mount("rc", DE_RC_TEST_TMP_DIR, DE_MOUNT_WRITABLE, 0), DE_OK);
        ASSERT_EQ(de_fs_mount("rcimp", DE_RC_TEST_IMPORT_DATA_DIR, 0, 0), DE_OK);
        mounted = true;
    }
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_RENDER);
    f->dev_alloc = de_tracking_allocator_init(&f->dev_track, NULL, DE_MEM_RENDER);
    de_fp_env_init_thread(); /* the task system checks the main thread's FP mode */
    ASSERT_EQ(de_type_registry_create(&f->alloc, &f->types), DE_OK);
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &f->w),
              DE_OK);
}

static void cfx_teardown(cfx *f) {
    if (f->s != NULL)
        de_asset_system_destroy(f->s); /* first: its unloads use the renderer's uploader */
    de_renderer_destroy(f->r);
    if (f->d != NULL)
        de_gpu_device_destroy(f->d);
    if (f->w != NULL)
        de_world_destroy(f->w);
    if (f->tasks != NULL)
        de_task_system_destroy(f->tasks);
    if (f->types != NULL)
        de_type_registry_destroy(f->types);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->dev_track).bytes_live, 0u);
}

static void null_device(cfx *f) {
    de_gpu_device_desc dd = {
        .backend = DE_GPU_NULL, .allocator = &f->dev_alloc, .pipeline_cache_dir = ""};
    ASSERT_EQ(de_gpu_device_create(&dd, &f->d), DE_OK);
}

static de_type_id type_of(cfx *f, const char *name) {
    return de_type_find(f->types, name);
}

static const de_field_info *field_of(cfx *f, const char *type, const char *name) {
    u32 n = 0;
    const de_field_info *fs = de_type_fields(f->types, type_of(f, type), &n);
    for (u32 i = 0; i < n; ++i)
        if (strcmp(fs[i].name, name) == 0)
            return &fs[i];
    return NULL;
}

/* ---- render_components.registered ------------------------------------------------------------ */

TEST_F(render_components, registered, cfx) {
    EXPECT_TRUE(de__render_components(f->w) == NULL);
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    const DeRenderComponents *ids = de__render_components(f->w);
    ASSERT_TRUE(ids != NULL);
    const de_type_desc *desc[COMP_COUNT];
    for (u32 i = 0; i < COMP_COUNT; ++i) {
        const de_component c = de_component_find(f->w, k_comps[i].name);
        ASSERT_TRUE(DE_HANDLE_VALID(c));
        EXPECT_EQ(c.id, comp_id(ids, i).id);
        desc[i] = de_type_get(f->types, type_of(f, k_comps[i].name));
        ASSERT_TRUE(desc[i] != NULL);
        EXPECT_EQ(desc[i]->field_count, k_comps[i].fields);
        EXPECT_TRUE(desc[i]->hooks.construct != NULL);
        EXPECT_TRUE(desc[i]->is_pod);
    }
    EXPECT_TRUE(DE_HANDLE_VALID(type_of(f, "de_projection")));
    EXPECT_TRUE(DE_HANDLE_VALID(type_of(f, "de_light_kind")));

    /* Idempotent: same ids, the registry not touched (same descriptions, no new memory). */
    const u64 live = de_tracking_allocator_stats(&f->track).bytes_live;
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, live);
    EXPECT_TRUE(de__render_components(f->w) == ids);
    for (u32 i = 0; i < COMP_COUNT; ++i) {
        EXPECT_EQ(de_component_find(f->w, k_comps[i].name).id, comp_id(ids, i).id);
        EXPECT_TRUE(de_type_get(f->types, type_of(f, k_comps[i].name)) == desc[i]);
    }

    /* A second world on the same registry reuses the types and gets its own components. */
    de_world *w2 = NULL;
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = f->types}, &w2),
              DE_OK);
    (void)de_builtin_components(w2); /* other component ids than the first world's */
    ASSERT_EQ(de_render_register_components(w2), DE_OK);
    const DeRenderComponents *ids2 = de__render_components(w2);
    ASSERT_TRUE(ids2 != NULL);
    for (u32 i = 0; i < COMP_COUNT; ++i) {
        EXPECT_EQ(de_component_find(w2, k_comps[i].name).id, comp_id(ids2, i).id);
        EXPECT_TRUE(de_type_get(f->types, type_of(f, k_comps[i].name)) == desc[i]);
    }
    de_world_destroy(w2);

    EXPECT_EQ(de_render_register_components(NULL), DE_ERR_INVALID_ARGUMENT);

    /* A registry with another type called DeLight: refused, nothing registered as a component. */
    typedef struct OtherLight {
        f32 watts;
    } OtherLight;
    static const de_field_info k_other[] = {DE_FIELD(OtherLight, watts, DE_FK_F32, 0)};
    de_type_registry *reg = NULL;
    de_world *w3 = NULL;
    ASSERT_EQ(de_type_registry_create(&f->alloc, &reg), DE_OK);
    de_type_desc od = DE_TYPE_DESC(OtherLight, 1, k_other);
    od.name = "DeLight";
    ASSERT_TRUE(DE_HANDLE_VALID(de_type_register(reg, &od)));
    ASSERT_EQ(de_world_create(&(de_world_desc){.allocator = &f->alloc, .types = reg}, &w3), DE_OK);
    EXPECT_EQ(de_render_register_components(w3), DE_ERR_ALREADY_EXISTS);
    EXPECT_FALSE(DE_HANDLE_VALID(de_component_find(w3, "DeLight")));
    EXPECT_TRUE(de__render_components(w3) == NULL);
    de_world_destroy(w3);
    de_type_registry_destroy(reg);
}

/* de_renderer_attach registers them (once), on the null backend. */
TEST_F(render_components, attach, cfx) {
    null_device(f);
    ASSERT_EQ(
        de_renderer_create(&(de_renderer_desc){.device = f->d, .allocator = &f->alloc}, &f->r),
        DE_OK);
    de_renderer_attach(f->r, f->w);
    const DeRenderComponents *ids = de__render_components(f->w);
    ASSERT_TRUE(ids != NULL);
    for (u32 i = 0; i < COMP_COUNT; ++i)
        EXPECT_EQ(de_component_find(f->w, k_comps[i].name).id, comp_id(ids, i).id);
    const u64 live = de_tracking_allocator_stats(&f->track).bytes_live;
    de_renderer_attach(f->r, f->w); /* after a world reload: nothing changes */
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, live);
    EXPECT_TRUE(de__render_components(f->w) == ids);
    de_renderer_attach(f->r, NULL);
    de_renderer_attach(NULL, f->w);
}

/* ---- render_components.text_roundtrip -------------------------------------------------------- */

#define ASSET_ID_BASE 0xa55e700000000000ull

/* Asset handles as IDs: handle h <-> ASSET_ID_BASE | h; 0 stays 0. Counts the calls. */
typedef struct asset_map {
    u32 to_id, to_handle;
    bool bad_type;
} asset_map;

static u64 map_asset_to_id(u32 h, const char *asset_type, void *user) {
    asset_map *m = (asset_map *)user;
    (void)asset_type;
    m->to_id++;
    return h != 0 ? ASSET_ID_BASE | h : 0u;
}
static u32 map_id_to_asset(u64 id, const char *asset_type, void *user) {
    asset_map *m = (asset_map *)user;
    (void)asset_type;
    m->to_handle++;
    if (id == 0)
        return 0;
    if ((id & ~(u64)0xFFFFFFFFu) != ASSET_ID_BASE)
        m->bad_type = true;
    return (u32)(id & 0xFFFFFFFFu);
}

/* Writes a value different from the default into every field of `obj` (seed `k`), runtime
 * fields included, and returns the number of asset handles written. */
static u32 fill_fields(cfx *f, de_type_id t, u8 *obj, u32 k) {
    u32 n = 0, assets = 0;
    const de_field_info *fs = de_type_fields(f->types, t, &n);
    for (u32 i = 0; i < n; ++i) {
        const de_field_info *fi = &fs[i];
        u8 *p = obj + fi->offset;
        const f32 base = 0.25f + 0.5f * (f32)(k + i);
        switch (fi->kind) {
        case DE_FK_BOOL:
            p[0] = p[0] ? 0 : 1;
            break;
        case DE_FK_I16: {
            const i16 v = (i16)(-7 - (i32)i);
            memcpy(p, &v, sizeof v);
            break;
        }
        case DE_FK_I32: {
            const i32 v = -1000 - (i32)i;
            memcpy(p, &v, sizeof v);
            break;
        }
        case DE_FK_U32: {
            const u32 v = 1000u + i + k;
            memcpy(p, &v, sizeof v);
            break;
        }
        case DE_FK_F32:
            memcpy(p, &base, sizeof base);
            break;
        case DE_FK_VEC2:
        case DE_FK_VEC3:
        case DE_FK_VEC4:
        case DE_FK_COLOR:
            for (u32 c = 0; c < fi->size / 4u; ++c) {
                const f32 v = base + (f32)c;
                memcpy(p + 4u * c, &v, sizeof v);
            }
            break;
        case DE_FK_ENUM: {
            const de_enum_info *e = de_enum_get(f->types, type_of(f, fi->attrs.nested_type));
            const i32 v = (i32)e->values[e->count - 1u].value;
            memcpy(p, &v, sizeof v);
            break;
        }
        case DE_FK_ASSET: {
            const u32 h = 10u + i + k;
            memcpy(p, &h, sizeof h);
            assets++;
            break;
        }
        case DE_FK_FIXED_ARRAY: {
            const de_field_info *el = de_type_array_element(f->types, t, i);
            for (u32 e = 0; e < fi->attrs.array_count; ++e) {
                if (el->kind == DE_FK_ASSET) {
                    const u32 h = e == 2 ? 0u : 100u + 8u * i + e; /* one empty slot */
                    memcpy(p + 4u * e, &h, sizeof h);
                    assets += h != 0;
                } else {
                    const f32 v = base + 0.125f * (f32)e;
                    memcpy(p + 4u * e, &v, sizeof v);
                }
            }
            break;
        }
        default:
            EXPECT_TRUE(false); /* a field kind this test does not fill */
            break;
        }
    }
    return assets;
}

TEST_F(render_components, text_roundtrip, cfx) {
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    for (u32 c = 0; c < COMP_COUNT; ++c) {
        const de_type_id t = type_of(f, k_comps[c].name);
        const de_type_desc *d = de_type_get(f->types, t);
        ASSERT_TRUE(d != NULL);
        u8 src[512], dst[512];
        ASSERT_LE(d->size, (u32)sizeof src);
        ASSERT_EQ(de_type_construct(f->types, t, src), DE_OK);
        const u32 assets = fill_fields(f, t, src, c);

        asset_map m;
        memset(&m, 0, sizeof m);
        const de_ser_context ctx = {NULL, NULL, map_asset_to_id, map_id_to_asset, &m};
        const de_ser_options opt = {DE_SER_TEXT, DE_FIELD_SERIALIZE, &ctx};
        de_mem_writer mw;
        de_mem_writer_init(&mw, &f->alloc);
        de_writer w = de_writer_from_mem(&mw);
        ASSERT_EQ(de_serialize_object(f->types, t, src, &opt, &w), DE_OK);
        const char nul = '\0';
        ASSERT_EQ(w.write(w.user, &nul, 1), DE_OK);
        const char *text = (const char *)mw.data;

        /* Assets are written as IDs, never as the handle value. */
        u32 nf = 0;
        const de_field_info *fs = de_type_fields(f->types, t, &nf);
        for (u32 i = 0; i < nf; ++i) {
            if (fs[i].kind != DE_FK_ASSET)
                continue;
            u32 h = 0;
            memcpy(&h, src + fs[i].offset, 4);
            char want[64];
            snprintf(want, sizeof want, "%s = { id = \"0x%016llx\"", fs[i].name,
                     (unsigned long long)(ASSET_ID_BASE | h));
            if (strstr(text, want) == NULL)
                printf("%s: missing `%s` in:\n%s\n", k_comps[c].name, want, text);
            EXPECT_TRUE(strstr(text, want) != NULL);
        }
        EXPECT_GE(m.to_id, assets);
        /* Runtime handles are not written. */
        EXPECT_TRUE(strstr(text, "view =") == NULL);
        EXPECT_TRUE(strstr(text, "tonemapper =") == NULL);

        ASSERT_EQ(de_type_construct(f->types, t, dst), DE_OK);
        de_mem_reader mr;
        de_mem_reader_init(&mr, mw.data, mw.size - 1u);
        de_reader rd = de_reader_from_mem(&mr);
        const de_result r = de_deserialize_object(f->types, t, dst, &opt, &rd);
        if (r != DE_OK)
            printf("%s:\n%s\n%s\n", k_comps[c].name, text,
                   de_error_last() ? de_error_last()->message : "");
        ASSERT_EQ(r, DE_OK);
        EXPECT_FALSE(m.bad_type);
        EXPECT_GE(m.to_handle, assets);

        /* Every serialized field is back bit for bit; the others keep the default. */
        u8 def[512];
        ASSERT_EQ(de_type_construct(f->types, t, def), DE_OK);
        for (u32 i = 0; i < nf; ++i) {
            const u8 *want =
                (fs[i].flags & DE_FIELD_SERIALIZE) ? src + fs[i].offset : def + fs[i].offset;
            if (memcmp(dst + fs[i].offset, want, fs[i].size) != 0)
                printf("%s.%s differs\n", k_comps[c].name, fs[i].name);
            EXPECT_MEMEQ(dst + fs[i].offset, want, fs[i].size);
        }
        de_mem_writer_free(&mw);
    }

    /* The default camera's NaN exposure (auto) round-trips as NaN. */
    const de_type_id ct = type_of(f, "DeCamera");
    DeCamera cam, back;
    ASSERT_EQ(de_type_construct(f->types, ct, &cam), DE_OK);
    de_mem_writer mw;
    de_mem_writer_init(&mw, &f->alloc);
    de_writer w = de_writer_from_mem(&mw);
    ASSERT_EQ(de_serialize_object(f->types, ct, &cam, NULL, &w), DE_OK);
    ASSERT_EQ(de_type_construct(f->types, ct, &back), DE_OK);
    back.exposure_ev100 = 3.0f;
    de_mem_reader mr;
    de_mem_reader_init(&mr, mw.data, mw.size);
    de_reader rd = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(f->types, ct, &back, NULL, &rd), DE_OK);
    EXPECT_TRUE(isnan(back.exposure_ev100));
    de_mem_writer_free(&mw);
}

TEST_F(render_components, legacy_light_selection_and_high_bit_roundtrip, cfx) {
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    const de_type_id type = type_of(f, "DeLight");
    const char legacy[] = "__type = \"DeLight\"\n__version = 1\nintensity = 17\n";
    DeLight light;
    ASSERT_EQ(de_type_construct(f->types, type, &light), DE_OK);
    de_mem_reader mr;
    de_mem_reader_init(&mr, legacy, sizeof legacy - 1);
    de_reader reader = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(f->types, type, &light, NULL, &reader), DE_OK);
    EXPECT_EQ(light.view_mask, 0u);
    EXPECT_FLOAT_EQ(light.intensity, 17, 0);
    light.view_mask = 0x80000002u;
    de_mem_writer mw;
    de_mem_writer_init(&mw, &f->alloc);
    de_writer writer = de_writer_from_mem(&mw);
    ASSERT_EQ(de_serialize_object(f->types, type, &light, NULL, &writer), DE_OK);
    DeLight copy;
    ASSERT_EQ(de_type_construct(f->types, type, &copy), DE_OK);
    de_mem_reader_init(&mr, mw.data, mw.size);
    reader = de_reader_from_mem(&mr);
    ASSERT_EQ(de_deserialize_object(f->types, type, &copy, NULL, &reader), DE_OK);
    EXPECT_EQ(copy.view_mask, 0x80000002u);
    EXPECT_FLOAT_EQ(copy.intensity, 17, 0);
    de_mem_writer_free(&mw);
}

/* ---- render_components.defaults -------------------------------------------------------------- */

static void construct_from(cfx *f, const char *name, void *obj, size_t size, u8 fill) {
    memset(obj, fill, size);
    ASSERT_EQ(de_type_construct(f->types, type_of(f, name), obj), DE_OK);
}

TEST_F(render_components, defaults, cfx) {
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    static const u8 fills[2] = {0x00, 0xCD}; /* zeroed and garbage memory */
    for (u32 k = 0; k < 2; ++k) {
        const u8 fill = fills[k];
        DeCamera cam;
        construct_from(f, "DeCamera", &cam, sizeof cam, fill);
        EXPECT_EQ((int)cam.projection, (int)DE_PROJ_PERSPECTIVE);
        EXPECT_FLOAT_EQ(cam.fov_y, 60.0 * 3.14159265358979 / 180.0, 1e-6);
        EXPECT_FLOAT_EQ(cam.ortho_height, 10.0, 0);
        EXPECT_FLOAT_EQ(cam.near_z, 0.1, 1e-7);
        EXPECT_TRUE(isnan(cam.exposure_ev100));
        EXPECT_EQ(cam.view.id, 0u);
        EXPECT_EQ(cam.tonemapper.id, 0u);
        for (u32 i = 0; i < 8; ++i)
            EXPECT_FLOAT_EQ(cam.tonemap_params[i], 0.0, 0);

        DeMeshRenderer mr;
        construct_from(f, "DeMeshRenderer", &mr, sizeof mr, fill);
        EXPECT_EQ(mr.mesh.id, 0u);
        for (u32 i = 0; i < 8; ++i)
            EXPECT_EQ(mr.materials[i].id, 0u);
        EXPECT_EQ(mr.layer_mask, ~0u);
        EXPECT_TRUE(mr.cast_shadows);
        EXPECT_TRUE(mr.visible_in_rt);

        DeLight l;
        construct_from(f, "DeLight", &l, sizeof l, fill);
        EXPECT_EQ((int)l.kind, (int)DE_LIGHT_POINT);
        EXPECT_FLOAT_EQ(l.color.x, 1.0, 0);
        EXPECT_FLOAT_EQ(l.color.y, 1.0, 0);
        EXPECT_FLOAT_EQ(l.color.z, 1.0, 0);
        EXPECT_FLOAT_EQ(l.intensity, 800.0, 0);
        EXPECT_FLOAT_EQ(l.range, 0.0, 0);
        EXPECT_FLOAT_EQ(l.spot_inner, 0.0, 0);
        EXPECT_FLOAT_EQ(l.spot_outer, 3.14159265358979 / 4.0, 1e-6);
        EXPECT_FLOAT_EQ(l.area_size.x, 1.0, 0);
        EXPECT_FLOAT_EQ(l.area_size.y, 1.0, 0);
        EXPECT_FLOAT_EQ(l.source_radius, 0.05, 1e-7);
        EXPECT_TRUE(l.cast_shadows);
        EXPECT_EQ(l.view_mask, 0u);

        DeEnvironment env;
        construct_from(f, "DeEnvironment", &env, sizeof env, fill);
        EXPECT_EQ(env.hdri.id, 0u);
        EXPECT_FLOAT_EQ(env.intensity, 1.0, 0);
        EXPECT_FLOAT_EQ(env.rotation, 0.0, 0);

        DeSprite sp;
        construct_from(f, "DeSprite", &sp, sizeof sp, fill);
        EXPECT_EQ(sp.texture.id, 0u);
        EXPECT_FLOAT_EQ(sp.uv_rect.x, 0.0, 0);
        EXPECT_FLOAT_EQ(sp.uv_rect.w, 1.0, 0);
        EXPECT_FLOAT_EQ(sp.size.x, 1.0, 0);
        EXPECT_FLOAT_EQ(sp.pivot.y, 0.5, 0);
        EXPECT_FLOAT_EQ(sp.color.w, 1.0, 0);
        EXPECT_FLOAT_EQ(sp.thickness, 0.0, 0);
        EXPECT_EQ((int)sp.layer, 0);
        EXPECT_FALSE(sp.flip_x || sp.flip_y || sp.translucent);
        EXPECT_TRUE(sp.cast_shadows);

        DeSky sky;
        construct_from(f, "DeSky", &sky, sizeof sky, fill);
        EXPECT_FLOAT_EQ(sky.time_of_day_hours, 12.0, 0);
        EXPECT_FLOAT_EQ(sky.latitude_deg, 45.0, 0);
        EXPECT_FLOAT_EQ(sky.turbidity, 2.0, 0);
        EXPECT_FALSE(sky.clouds);
        EXPECT_FLOAT_EQ(sky.cloud_coverage, 0.5, 0);

        DeFogVolume fog;
        construct_from(f, "DeFogVolume", &fog, sizeof fog, fill);
        EXPECT_FLOAT_EQ(fog.extent.z, 10.0, 0);
        EXPECT_FLOAT_EQ(fog.density, 0.02, 1e-8);
        EXPECT_FLOAT_EQ(fog.albedo.x, 1.0, 0);
        EXPECT_FLOAT_EQ(fog.anisotropy, 0.0, 0);

        DeDecal dc;
        construct_from(f, "DeDecal", &dc, sizeof dc, fill);
        EXPECT_EQ(dc.material.id, 0u);
        EXPECT_FLOAT_EQ(dc.extent.y, 1.0, 0);
        EXPECT_FLOAT_EQ(dc.angle_fade, 1.0, 0);
        EXPECT_EQ(dc.sort, 0);

        DeParticleSystem ps;
        construct_from(f, "DeParticleSystem", &ps, sizeof ps, fill);
        EXPECT_EQ(ps.effect.id, 0u);
        EXPECT_TRUE(ps.playing);
        EXPECT_FLOAT_EQ(ps.time_scale, 1.0, 0);
        EXPECT_EQ(ps.seed, 0u);

        DeTerrain tr;
        construct_from(f, "DeTerrain", &tr, sizeof tr, fill);
        EXPECT_EQ(tr.heightmap.id, 0u);
        EXPECT_EQ(tr.layer_count, 0u);
        EXPECT_FLOAT_EQ(tr.cell_size, 1.0, 0);
        EXPECT_FLOAT_EQ(tr.height_scale, 100.0, 0);

        DeWater wt;
        construct_from(f, "DeWater", &wt, sizeof wt, fill);
        EXPECT_FALSE(wt.ocean);
        EXPECT_FLOAT_EQ(wt.wind.x, 5.0, 0);
        EXPECT_FLOAT_EQ(wt.wind.y, 0.0, 0);
        EXPECT_FLOAT_EQ(wt.choppiness, 1.0, 0);
    }

    /* The ECS runs the hooks too. */
    const DeRenderComponents *ids = de__render_components(f->w);
    ASSERT_TRUE(ids != NULL);
    const de_entity e = de_entity_spawn(f->w);
    const DeCamera *cam = (const DeCamera *)de_entity_add(f->w, e, ids->camera);
    ASSERT_TRUE(cam != NULL);
    EXPECT_FLOAT_EQ(cam->near_z, 0.1, 1e-7);
    const DeMeshRenderer *mr = (const DeMeshRenderer *)de_entity_add(f->w, e, ids->mesh_renderer);
    ASSERT_TRUE(mr != NULL);
    EXPECT_EQ(mr->layer_mask, ~0u);
    const DeLight *l = (const DeLight *)de_entity_add(f->w, e, ids->light);
    ASSERT_TRUE(l != NULL);
    EXPECT_TRUE(l->cast_shadows);

    /* Default intensities per kind, in the kind's unit. */
    EXPECT_FLOAT_EQ(de_light_default_intensity(DE_LIGHT_DIRECTIONAL), 100000.0, 0);
    EXPECT_FLOAT_EQ(de_light_default_intensity(DE_LIGHT_POINT), 800.0, 0);
    EXPECT_FLOAT_EQ(de_light_default_intensity(DE_LIGHT_SPOT), 800.0, 0);
    EXPECT_FLOAT_EQ(de_light_default_intensity(DE_LIGHT_RECT), 1000.0, 0);
    EXPECT_FLOAT_EQ(de_light_default_intensity(DE_LIGHT_DISK), 1000.0, 0);
    EXPECT_FLOAT_EQ(de_light_default_intensity((de_light_kind)99), 800.0, 0);
}

/* ---- render_components.asset_ref_flags ------------------------------------------------------- */

TEST_F(render_components, asset_ref_flags, cfx) {
    ASSERT_EQ(de_render_register_components(f->w), DE_OK);
    /* Every asset field, with its asset type (NULL: an array of "asset"). */
    static const struct {
        const char *type, *field, *asset;
    } k_assets[] = {
        {"DeMeshRenderer", "mesh", "mesh"},      {"DeMeshRenderer", "materials", NULL},
        {"DeEnvironment", "hdri", "texture"},    {"DeSprite", "texture", "texture"},
        {"DeSprite", "normal_tex", "texture"},   {"DeSprite", "rough_metal_tex", "texture"},
        {"DeSprite", "emissive_tex", "texture"}, {"DeSprite", "height_tex", "texture"},
        {"DeDecal", "material", "material"},     {"DeParticleSystem", "effect", "blob"},
        {"DeTerrain", "heightmap", "blob"},      {"DeTerrain", "layers", NULL},
        {"DeWater", "material", "material"},
    };
    const u32 asset_count = (u32)(sizeof k_assets / sizeof k_assets[0]);
    for (u32 a = 0; a < asset_count; ++a) {
        const de_field_info *fi = field_of(f, k_assets[a].type, k_assets[a].field);
        ASSERT_TRUE(fi != NULL);
        EXPECT_TRUE((fi->flags & DE_FIELD_ASSET_REF) != 0);
        EXPECT_TRUE((fi->flags & DE_FIELD_SERIALIZE) != 0);
        if (k_assets[a].asset != NULL) {
            EXPECT_EQ((int)fi->kind, (int)DE_FK_ASSET);
            EXPECT_STREQ(fi->attrs.nested_type, k_assets[a].asset);
        } else {
            EXPECT_EQ((int)fi->kind, (int)DE_FK_FIXED_ARRAY);
            EXPECT_STREQ(fi->attrs.nested_type, "asset");
            EXPECT_EQ(fi->attrs.array_count, 8u);
        }
    }
    /* No other field is an asset reference; runtime handles are hidden and not serialized; all
     * other fields are authorable (serialize + snapshot). */
    u32 seen_assets = 0;
    for (u32 c = 0; c < COMP_COUNT; ++c) {
        u32 n = 0;
        const de_field_info *fs = de_type_fields(f->types, type_of(f, k_comps[c].name), &n);
        for (u32 i = 0; i < n; ++i) {
            const bool is_asset =
                fs[i].kind == DE_FK_ASSET ||
                (fs[i].kind == DE_FK_FIXED_ARRAY && strcmp(fs[i].attrs.nested_type, "asset") == 0);
            EXPECT_EQ(is_asset, (fs[i].flags & DE_FIELD_ASSET_REF) != 0);
            seen_assets += is_asset;
            const bool runtime =
                strcmp(k_comps[c].name, "DeCamera") == 0 &&
                (strcmp(fs[i].name, "view") == 0 || strcmp(fs[i].name, "tonemapper") == 0);
            if (runtime) {
                EXPECT_EQ(fs[i].flags, (u32)(DE_FIELD_SNAPSHOT | DE_FIELD_EDITOR_HIDDEN));
            } else {
                EXPECT_TRUE((fs[i].flags & DE_FIELD_DEFAULT) == DE_FIELD_DEFAULT);
                EXPECT_TRUE((fs[i].flags & DE_FIELD_EDITOR_HIDDEN) == 0);
            }
        }
    }
    EXPECT_EQ(seen_assets, asset_count);

    /* Units, ranges, enums. */
    const de_field_info *fov = field_of(f, "DeCamera", "fov_y");
    ASSERT_TRUE(fov != NULL);
    EXPECT_STREQ(fov->attrs.units, "rad");
    EXPECT_FLOAT_EQ(fov->attrs.min, 0.01, 0);
    EXPECT_FLOAT_EQ(fov->attrs.max, 3.13, 0);
    EXPECT_STREQ(field_of(f, "DeCamera", "near_z")->attrs.units, "m");
    EXPECT_STREQ(field_of(f, "DeCamera", "ortho_height")->attrs.units, "m");
    EXPECT_STREQ(field_of(f, "DeCamera", "exposure_ev100")->attrs.units, "EV100");
    EXPECT_TRUE(field_of(f, "DeCamera", "exposure_ev100")->attrs.tooltip != NULL);
    EXPECT_STREQ(field_of(f, "DeLight", "spot_inner")->attrs.units, "rad");
    EXPECT_STREQ(field_of(f, "DeLight", "spot_outer")->attrs.units, "rad");
    EXPECT_STREQ(field_of(f, "DeLight", "range")->attrs.units, "m");
    EXPECT_STREQ(field_of(f, "DeLight", "source_radius")->attrs.units, "m");
    EXPECT_STREQ(field_of(f, "DeLight", "intensity")->attrs.units, "lux|lm|nit");
    EXPECT_TRUE(field_of(f, "DeLight", "intensity")->attrs.tooltip != NULL);
    EXPECT_STREQ(field_of(f, "DeSprite", "size")->attrs.units, "m");
    EXPECT_STREQ(field_of(f, "DeWater", "wind")->attrs.units, "m/s");

    const de_field_info *proj = field_of(f, "DeCamera", "projection");
    ASSERT_TRUE(proj != NULL);
    EXPECT_EQ((int)proj->kind, (int)DE_FK_ENUM);
    EXPECT_STREQ(proj->attrs.nested_type, "de_projection");
    const de_field_info *kind = field_of(f, "DeLight", "kind");
    ASSERT_TRUE(kind != NULL);
    EXPECT_EQ((int)kind->kind, (int)DE_FK_ENUM);
    const de_enum_info *lk = de_enum_get(f->types, type_of(f, "de_light_kind"));
    ASSERT_TRUE(lk != NULL);
    EXPECT_EQ(lk->count, 5u);
    i64 v = -1;
    EXPECT_TRUE(de_enum_value_of(f->types, type_of(f, "de_light_kind"), "DE_LIGHT_SPOT", &v));
    EXPECT_EQ(v, (i64)DE_LIGHT_SPOT);
    EXPECT_STREQ(de_enum_name_of(f->types, type_of(f, "de_projection"), DE_PROJ_ORTHOGRAPHIC),
                 "DE_PROJ_ORTHOGRAPHIC");
}

/* ---- render_components.gltf_prefab_instantiate ----------------------------------------------- */

#if DE_HAS_COOKER
static void copy_file(cfx *f, const char *from, const char *to) {
    void *p = NULL;
    u64 n = 0;
    ASSERT_EQ(de_file_read_all(from, &f->alloc, &p, &n), DE_OK);
    ASSERT_EQ(de_file_write_all_atomic(to, p, n), DE_OK);
    DE_FREE(&f->alloc, p, (size_t)n + 1u);
}

static bool final_state(de_load_state st) {
    return st == DE_LOAD_READY || st == DE_LOAD_PARTIAL || st == DE_LOAD_FAILED ||
           st == DE_LOAD_CANCELLED;
}

/* Frames (renderer + asset update) until `h` is final. */
static de_load_state pump(cfx *f, de_load_handle h) {
    const u64 t0 = de_time_now_ns();
    for (;;) {
        (void)de_renderer_render_frame(f->r);
        de__asset_wake_reset(f->s);
        de_asset_system_update(f->s, NULL);
        const de_load_state st = de_asset_status(f->s, h, NULL);
        if (final_state(st) || de_time_now_ns() - t0 > WAIT_NS)
            return st;
        (void)de__asset_wake_block(f->s, 2000000u);
    }
}

/* Visits the tree under `e` (depth first): entity count, and the ones with DeMeshRenderer. */
static void walk(cfx *f, de_entity e, de_component mr, u32 *count, de_entity *with_mesh,
                 u32 *mesh_count) {
    (*count)++;
    if (de_entity_has(f->w, e, mr) && *mesh_count < 4)
        with_mesh[(*mesh_count)++] = e;
    for (de_entity c = de_entity_children_first(f->w, e); c.bits != 0;
         c = de_entity_children_next(f->w, c)) {
        EXPECT_EQ(de_entity_parent(f->w, c).bits, e.bits);
        walk(f, c, mr, count, with_mesh, mesh_count);
    }
}
#endif

/* The S11 loop closed: a glTF model cooked in-process (the renderer's model type with its
 * importer) whose prefab, instantiated, gives the node hierarchy with DeTransform, DeChildOf and
 * DeMeshRenderer, the mesh and material handles resolved to the model's sub-assets. */
TEST_F(render_components, gltf_prefab_instantiate, cfx) {
#if !DE_HAS_COOKER
    SKIP("this build does not link the cooker into the runtime (DE_HAS_COOKER)");
#else
    if (!de_path_exists("rc:/prefab"))
        ASSERT_EQ(de_dir_create("rc:/prefab"), DE_OK);
    copy_file(f, "rcimp:/gltf/hierarchy.gltf", "rc:/prefab/hierarchy.gltf");
    static const char manifest[] =
        "[assets]\n\"props/robot\" = { type = \"model\", source = \"hierarchy.gltf\" }\n";
    ASSERT_EQ(de_file_write_all_atomic("rc:/prefab/assets.toml", manifest, sizeof manifest - 1),
              DE_OK);

    ASSERT_EQ(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &f->tasks), DE_OK);
    ASSERT_EQ(de_asset_system_create(&(de_asset_system_desc){.allocator = &f->alloc,
                                                             .tasks = f->tasks,
                                                             .io_threads = 1,
                                                             .cache_dir = "rc:/prefab/decache",
                                                             .types = f->types},
                                     &f->s),
              DE_OK);
    null_device(f);
    ASSERT_EQ(de_renderer_create(
                  &(de_renderer_desc){
                      .device = f->d, .assets = f->s, .types = f->types, .allocator = &f->alloc},
                  &f->r),
              DE_OK);
    /* Attach before the cook: the prefab importer reads DeMeshRenderer from the registry. */
    de_renderer_attach(f->r, f->w);
    const DeRenderComponents *ids = de__render_components(f->w);
    ASSERT_TRUE(ids != NULL);

    ASSERT_EQ(de_asset_manifest_load(f->s, "rc:/prefab/assets.toml"), DE_OK);
    const de_model mh = de_asset_model(f->s, de_asset_id_from("props/robot"));
    ASSERT_TRUE(DE_HANDLE_VALID(mh));
    const de_load_handle h = de_asset_load(f->s, de_asset_id_from("props/robot"), 0);
    const de_load_state st = pump(f, h);
    if (st != DE_LOAD_READY) {
        de_load_info info;
        (void)de_asset_status(f->s, h, &info);
        printf("  %s\n", info.first_error_msg);
    }
    ASSERT_EQ((int)st, (int)DE_LOAD_READY);

    const de_prefab p = de_model_prefab(f->s, mh);
    ASSERT_TRUE(DE_HANDLE_VALID(p));
    de_entity root = DE_ENTITY_NULL;
    ASSERT_EQ(de_prefab_instantiate(f->s, f->w, p, DE_ENTITY_NULL, &root), DE_OK);
    ASSERT_TRUE(de_entity_alive(f->w, root));

    /* The model root, Root (mesh) -> Arm -> Hand (mesh), Lamp; NotInScene is not in the scene. */
    const de_builtin_component_ids b = de_builtin_components(f->w);
    EXPECT_EQ(de_entity_parent(f->w, root).bits, 0u);
    EXPECT_FALSE(de_entity_has(f->w, root, b.child_of));
    EXPECT_TRUE(de_entity_has(f->w, root, b.transform));
    EXPECT_EQ(de_entity_children_count(f->w, root), 2u);
    u32 count = 0, mesh_count = 0;
    de_entity with_mesh[4];
    walk(f, root, ids->mesh_renderer, &count, with_mesh, &mesh_count);
    EXPECT_EQ(count, 5u);
    ASSERT_EQ(mesh_count, 2u);

    const de_mesh root_mesh = de_asset_mesh(f->s, de_asset_id_from("props/robot#Root"));
    const de_mesh hand_mesh = de_asset_mesh(f->s, de_asset_id_from("props/robot#Hand"));
    const de_material metal =
        de_asset_material(f->s, de_asset_id_from("props/robot#material:Metal"));
    ASSERT_TRUE(DE_HANDLE_VALID(root_mesh) && DE_HANDLE_VALID(hand_mesh));
    ASSERT_TRUE(DE_HANDLE_VALID(metal));
    u32 seen = 0;
    for (u32 i = 0; i < mesh_count; ++i) {
        const de_entity e = with_mesh[i];
        EXPECT_TRUE(de_entity_has(f->w, e, b.transform));
        EXPECT_TRUE(de_entity_has(f->w, e, b.child_of));
        const DeMeshRenderer *m =
            (const DeMeshRenderer *)de_entity_get_const(f->w, e, ids->mesh_renderer);
        ASSERT_TRUE(m != NULL);
        /* Fields the prefab does not name keep the construct defaults. */
        EXPECT_EQ(m->layer_mask, ~0u);
        EXPECT_TRUE(m->cast_shadows);
        EXPECT_TRUE(m->visible_in_rt);
        EXPECT_EQ(m->materials[0].id, metal.id);
        const DeTransform *t = (const DeTransform *)de_entity_get_const(f->w, e, b.transform);
        ASSERT_TRUE(t != NULL);
        if (m->mesh.id == root_mesh.id) {
            seen |= 1u;
            EXPECT_EQ(de_entity_parent(f->w, e).bits, root.bits);
            EXPECT_EQ(m->materials[1].id, 0u);
        } else if (m->mesh.id == hand_mesh.id) {
            seen |= 2u;
            /* Hand: two submeshes, the second without a material (the mesh's own). */
            EXPECT_EQ(m->materials[1].id, 0u);
            const de_entity arm = de_entity_parent(f->w, e);
            EXPECT_NE(arm.bits, root.bits);
            EXPECT_FALSE(de_entity_has(f->w, arm, ids->mesh_renderer));
            EXPECT_FLOAT_EQ(t->position.y, -1.0, 1e-6);
            EXPECT_FLOAT_EQ(t->scale.x, 0.5, 1e-6);
        }
    }
    EXPECT_EQ(seen, 3u);
    de_asset_release(f->s, h);
#endif
}
