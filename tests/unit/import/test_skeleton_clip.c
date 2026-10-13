/*
 * test_skeleton_clip.c: skeleton and animation clip cooking of the model importer (DENG-104):
 * skeleton order and inverse binds (DSKL), clip resampling, compression and round trip (DCLP,
 * decoded with the reference sampler src/anim/formats/clip_decode.c), determinism and settings.
 *
 * Fixtures: Khronos CesiumMan and RiggedFigure (data/anim/, mounted as animdata:/, licences in
 * KHRONOS_SAMPLES.txt), DENG-101's skinned.gltf (data/gltf/, as animgltf:/), and small IRs built
 * in memory for the track cases (constant, uniform scale, hemisphere flips, step and cubic).
 */
#include "de_test.h"

#include <dengine/de_asset.h>
#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include "anim/formats/de_cooked_clip.h"
#include "anim/formats/de_cooked_skeleton.h"
#include "asset/manifest_internal.h"
#include "cook/importers/model/anim_cook.h"
#include "cook/importers/model/fbx_import.h"
#include "cook/importers/model/model_import.h"
#include "cook/importers/model/model_internal.h"
#include "cook/importers/model/model_ir.h"
#include "cook/lib/de_cooker.h"
#include "render/formats/de_cooked_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define DEG 0.017453292519943295

/* Float comparisons (the de_test comparison macros take integers); the values are printed. */
static void expect_fle(double a, double b, const char *as, const char *bs, int line) {
    if (!(a <= b))
        printf("  line %d: %s = %.9g is not <= %s = %.9g\n", line, as, a, bs, b);
    EXPECT_TRUE(a <= b);
}
#define EXPECT_FLE(a, b) expect_fle((double)(a), (double)(b), #a, #b, __LINE__)
#define EXPECT_FGE(a, b) expect_fle((double)(b), (double)(a), #b, #a, __LINE__)

/* ---- Fixture --------------------------------------------------------------------------------- */

static void anim_mount(void) {
    static bool mounted;
    if (mounted)
        return;
    ASSERT_EQ(de_fs_mount("animdata", DE_IMPORT_TEST_DATA_DIR "/anim", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("animgltf", DE_IMPORT_TEST_DATA_DIR "/gltf", 0, 0), DE_OK);
    ASSERT_EQ(de_fs_mount("animfbx", DE_IMPORT_TEST_DATA_DIR "/fbx", 0, 0), DE_OK);
    mounted = true;
}

typedef struct anim_fx {
    de_tracking_allocator track;
    de_allocator alloc;
    de_model_ir ir;
    de_cooker *c;
    void *file;
    u64 file_size;
} anim_fx;

static void anim_fx_setup(anim_fx *f) {
    de_log_desc ld;
    memset(&ld, 0, sizeof ld);
    ld.no_console_sink = true;
    ASSERT_EQ(de_log_init(&ld), DE_OK);
    anim_mount();
    f->alloc = de_tracking_allocator_init(&f->track, NULL, DE_MEM_ASSET);
    de_model_ir_init(&f->ir, &f->alloc, "test");
    ASSERT_EQ(de_cooker_create(&(de_cooker_desc){.allocator = &f->alloc,
                                                 .verify_determinism = DE_COOK_VERIFY_ON},
                               &f->c),
              DE_OK);
    de_asset_type_desc model = {.name = "model",
                                .extensions = de_model_import_extensions,
                                .extension_count = DE_MODEL_IMPORT_EXTENSION_COUNT,
                                .importer_version = DE_MODEL_IMPORTER_VERSION,
                                .import = de_model_import};
    ASSERT_EQ(de_cooker_register_type(f->c, &model), DE_OK);
}

static void anim_fx_teardown(anim_fx *f) {
    de_model_ir_free(&f->ir);
    if (f->file != NULL)
        DE_FREE(&f->alloc, f->file, (size_t)f->file_size + 1u);
    if (f->c != NULL)
        de_cooker_destroy(f->c);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).bytes_live, 0u);
    EXPECT_EQ(de_tracking_allocator_stats(&f->track).blocks_live, 0u);
    de_log_shutdown();
}

/* Cooks `<root>:/<source>` as model `id` with the settings `kv`. */
static de_result cook_at(anim_fx *f, const char *root, const char *id, const char *source,
                         const de__manifest_kv *kv, u32 kv_count, de_cook_result *out) {
    char file[64], asset_root[64];
    snprintf(file, sizeof file, "%s:/assets.toml", root);
    snprintf(asset_root, sizeof asset_root, "%s:/", root);
    de__manifest_asset a;
    memset(&a, 0, sizeof a);
    a.id = id;
    a.type = "model";
    a.source = source;
    a.file = file;
    a.settings = kv;
    a.setting_count = kv_count;
    return de_cooker_cook(f->c, &(de_cook_input){.asset = &a, .asset_root = asset_root}, out);
}

static de_result cook(anim_fx *f, const char *id, const char *source, const de__manifest_kv *kv,
                      u32 kv_count, de_cook_result *out) {
    return cook_at(f, "animdata", id, source, kv, kv_count, out);
}

static const de_cook_subasset *sub(const de_cook_result *r, const char *name) {
    const de_cook_subasset *s = de_cook_result_subasset(r, name);
    if (s == NULL)
        printf("  no sub-asset \"%s\"\n", name);
    return s;
}

/* Loads `<root>:/<name>` into f->ir and converts it to engine coordinates, as the cook does. */
static void load_engine_ir(anim_fx *f, const char *root, const char *name) {
    char path[128];
    snprintf(path, sizeof path, "%s:/%s", root, name);
    ASSERT_EQ(de_file_read_all(path, &f->alloc, &f->file, &f->file_size), DE_OK);
    de_model_source src = {f->file, f->file_size, name, NULL, NULL};
    size_t nl = strlen(name);
    if (nl > 4 && strcmp(name + nl - 4, ".fbx") == 0) {
        /* The FBX front end converts axes and units itself (DENG-102). */
        de_model_fbx_options opt = {NULL, NULL};
        ASSERT_EQ(de_model_fbx_load(&src, &opt, &f->ir), DE_OK);
        de_model_ir_convert_axes(&f->ir, DE_MODEL_AXES_ENGINE, 1.0f);
        return;
    }
    ASSERT_EQ(de_model_gltf_load(&src, &f->ir), DE_OK);
    de_model_ir_convert_axes(&f->ir, DE_MODEL_AXES_GLTF, 1.0f);
}

static de__manifest_kv kv_float(const char *key, f64 v) {
    de__manifest_kv kv;
    memset(&kv, 0, sizeof kv);
    kv.key = key;
    kv.value.type = DE__MANIFEST_FLOAT;
    kv.value.u.f = v;
    return kv;
}

/* ---- Pose math (f64, independent of the cooker) ---------------------------------------------- */

static void trs_affine(const f64 t[3], const f64 q_in[4], const f64 s[3], f64 m[12]) {
    f64 n = sqrt(q_in[0] * q_in[0] + q_in[1] * q_in[1] + q_in[2] * q_in[2] + q_in[3] * q_in[3]);
    f64 x = q_in[0] / n, y = q_in[1] / n, z = q_in[2] / n, w = q_in[3] / n;
    f64 r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};
    for (u32 c = 0; c < 3; ++c)
        for (u32 k = 0; k < 3; ++k)
            m[c * 3 + k] = r[c * 3 + k] * s[c];
    for (u32 k = 0; k < 3; ++k)
        m[9 + k] = t[k];
}

static void aff_mul(const f64 a[12], const f64 b[12], f64 o[12]) {
    f64 r[12];
    for (u32 c = 0; c < 4; ++c)
        for (u32 k = 0; k < 3; ++k)
            r[c * 3 + k] = a[k] * b[c * 3] + a[3 + k] * b[c * 3 + 1] + a[6 + k] * b[c * 3 + 2] +
                           (c == 3 ? a[9 + k] : 0.0);
    memcpy(o, r, sizeof r);
}

static void pose_affine(const de_cooked_joint_pose *p, f64 m[12]) {
    f64 t[3] = {p->t[0], p->t[1], p->t[2]}, s[3] = {p->s[0], p->s[1], p->s[2]};
    f64 q[4] = {p->r[0], p->r[1], p->r[2], p->r[3]};
    trs_affine(t, q, s, m);
}

/* Model-space transforms of `pose` over the skeleton's parents. */
static void model_space(const de_cooked_skeleton_view *sk, const de_cooked_joint_pose *pose,
                        f64 *out) {
    for (u32 j = 0; j < sk->joint_count; ++j) {
        f64 l[12];
        pose_affine(&pose[j], l);
        u16 p = de_cooked_skeleton_parent(sk, j);
        if (p == DE_COOKED_SKEL_NO_PARENT)
            memcpy(out + 12u * j, l, sizeof l);
        else
            aff_mul(out + 12u * p, l, out + 12u * j);
    }
}

/* Largest change of a normalised column (about the angle in radians). */
static f64 orient_diff(const f64 *a, const f64 *b) {
    f64 e = 0.0;
    for (u32 c = 0; c < 3; ++c) {
        f64 la = 0, lb = 0, d = 0;
        for (u32 k = 0; k < 3; ++k) {
            la += a[c * 3 + k] * a[c * 3 + k];
            lb += b[c * 3 + k] * b[c * 3 + k];
        }
        for (u32 k = 0; k < 3; ++k) {
            f64 x = a[c * 3 + k] / sqrt(la) - b[c * 3 + k] / sqrt(lb);
            d += x * x;
        }
        e = sqrt(d) > e ? sqrt(d) : e;
    }
    return e;
}

static f64 pos_diff(const f64 *a, const f64 *b) {
    return sqrt((a[9] - b[9]) * (a[9] - b[9]) + (a[10] - b[10]) * (a[10] - b[10]) +
                (a[11] - b[11]) * (a[11] - b[11]));
}

/* The source channel at time t: the test's own evaluator (key value at a key time; linear lerp,
 * slerp on the short arc, step; cubic Hermite). */
static void chan_value(const de_model_anim_channel *c, f64 t, f64 out[4]) {
    u32 n = c->components, kc = c->key_count;
    u32 stride = c->interp == DE_MODEL_INTERP_CUBIC ? 3u * n : n;
    u32 off = c->interp == DE_MODEL_INTERP_CUBIC ? n : 0u;
    u32 lo = 0, hi = kc; /* the last key at or before t (0 when t is before every key) */
    while (hi - lo > 1u) {
        u32 mid = lo + (hi - lo) / 2u;
        if ((f64)c->times[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    const f32 *a = c->values + (u64)lo * stride + off;
    if (lo + 1u >= kc || t <= (f64)c->times[lo] || c->interp == DE_MODEL_INTERP_STEP) {
        for (u32 i = 0; i < n; ++i)
            out[i] = a[i];
        return;
    }
    const f32 *b = c->values + (u64)(lo + 1u) * stride + off;
    f64 dt = (f64)c->times[lo + 1u] - c->times[lo], s = (t - c->times[lo]) / dt;
    if (c->interp == DE_MODEL_INTERP_CUBIC) {
        const f32 *bo = c->values + (u64)lo * stride + 2u * n,
                  *ai = c->values + (u64)(lo + 1u) * stride;
        f64 s2 = s * s, s3 = s2 * s;
        for (u32 i = 0; i < n; ++i)
            out[i] = (2 * s3 - 3 * s2 + 1) * a[i] + (s3 - 2 * s2 + s) * dt * bo[i] +
                     (-2 * s3 + 3 * s2) * b[i] + (s3 - s2) * dt * ai[i];
        return;
    }
    if (n == 4u) {
        f64 qa[4], qb[4], la = 0.0, lb = 0.0, d = 0.0;
        for (u32 i = 0; i < 4; ++i) {
            la += (f64)a[i] * a[i];
            lb += (f64)b[i] * b[i];
        }
        for (u32 i = 0; i < 4; ++i) {
            qa[i] = a[i] / sqrt(la);
            qb[i] = b[i] / sqrt(lb);
            d += qa[i] * qb[i];
        }
        f64 sign = d < 0.0 ? -1.0 : 1.0, th = acos(fmin(1.0, fabs(d)));
        f64 wa = 1.0 - s, wb = s;
        if (th > 1e-9) {
            wa = sin((1.0 - s) * th) / sin(th);
            wb = sin(s * th) / sin(th);
        }
        for (u32 i = 0; i < 4; ++i)
            out[i] = wa * qa[i] + wb * sign * qb[i];
        return;
    }
    for (u32 i = 0; i < n; ++i)
        out[i] = a[i] + (b[i] - a[i]) * s;
}

/* The source pose of the skeleton's joints at time t: rest pose (DSKL) with the IR channels. */
static void source_pose(const de_model_ir *ir, const de_cooked_skeleton_view *sk,
                        const u32 *node_of_joint, const de_model_anim *an, f64 t,
                        de_cooked_joint_pose *out, f64 *tr) {
    for (u32 j = 0; j < sk->joint_count; ++j) {
        out[j] = de_cooked_skeleton_rest(sk, j);
        for (u32 k = 0; k < 3; ++k) {
            tr[10u * j + k] = out[j].t[k];
            tr[10u * j + 7 + k] = out[j].s[k];
        }
        for (u32 k = 0; k < 4; ++k)
            tr[10u * j + 3 + k] = out[j].r[k];
    }
    for (u32 c = 0; c < an->channel_count; ++c) {
        const de_model_anim_channel *ch = &an->channels[c];
        if (ch->path == DE_MODEL_ANIM_WEIGHTS)
            continue;
        for (u32 j = 0; j < sk->joint_count; ++j) {
            if (node_of_joint[j] != ch->node)
                continue;
            f64 v[4];
            chan_value(ch, t, v);
            u32 at = ch->path == DE_MODEL_ANIM_TRANSLATION ? 0u
                     : ch->path == DE_MODEL_ANIM_ROTATION  ? 3u
                                                           : 7u;
            for (u32 k = 0; k < ch->components; ++k)
                tr[10u * j + at + k] = v[k];
        }
    }
    (void)ir;
}

/* The IR node of each skeleton joint, by matching the joint name hash with the node sub-asset
 * names. */
static void match_joints(anim_fx *f, const de_cooked_skeleton_view *sk, u32 *node_of_joint) {
    de_model_names names;
    ASSERT_EQ(de_model_names_build(&f->ir, &names), DE_OK);
    for (u32 j = 0; j < sk->joint_count; ++j) {
        node_of_joint[j] = 0xFFFFFFFFu;
        for (u32 n = 0; n < f->ir.node_count; ++n)
            if (de_hash64_str(names.nodes[n]) == de_cooked_skeleton_name_hash(sk, j))
                node_of_joint[j] = n;
        ASSERT_NE(node_of_joint[j], 0xFFFFFFFFu);
    }
}

/* ---- import_anim.skeleton.parent_order ------------------------------------------------------- */

static void check_skeleton(const de_cook_result *r, const char *skel_name, u32 joints, u32 mesh,
                           const char *root_name, de_cooked_skeleton_view *out) {
    const de_cook_subasset *s = sub(r, skel_name);
    ASSERT_TRUE(s != NULL);
    EXPECT_STREQ(s->type, "skeleton");
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_skeleton_parse(s->data, s->size, out, &why));
    EXPECT_EQ(out->joint_count, joints);
    EXPECT_EQ(out->mesh_joint_count, mesh);
    u32 roots = 0;
    for (u32 j = 0; j < out->joint_count; ++j) {
        u16 p = de_cooked_skeleton_parent(out, j);
        if (p == DE_COOKED_SKEL_NO_PARENT)
            ++roots;
        else
            EXPECT_LT((u32)p, j);
        const char *nm = de_cooked_skeleton_name(out, j);
        ASSERT_TRUE(nm != NULL);
        EXPECT_EQ(de_cooked_skeleton_name_hash(out, j), de_hash64_str(nm));
    }
    EXPECT_EQ(roots, 1u);
    /* The remap is injective. */
    for (u32 i = 0; i < out->mesh_joint_count; ++i)
        for (u32 k = 0; k < i; ++k)
            EXPECT_NE(de_cooked_skeleton_remap(out, i), de_cooked_skeleton_remap(out, k));
    EXPECT_STREQ(de_cooked_skeleton_name(out, out->root_joint), root_name);
}

TEST_F(skeleton, parent_order, anim_fx) {
    de_cook_result r;
    de_cooked_skeleton_view sk;
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", NULL, 0, &r), DE_OK);
    /* The mesh node Cesium_Man and the top joint share the parent Armature (under Z_UP): the
     * skeleton is the 19 skin joints, in the source's order, and model space is Armature's. */
    check_skeleton(&r, "skeleton:Armature", 19, 19, "Skeleton_torso_joint_1", &sk);
    EXPECT_STREQ(de_cooked_skeleton_name(&sk, 0), "Skeleton_torso_joint_1");
    for (u32 i = 0; i < 19; ++i)
        EXPECT_EQ(de_cooked_skeleton_remap(&sk, i), i);
    /* The model's skinned node points at skeleton 0, whose ID is in the ID table. */
    de_cooked_model_view mv;
    ASSERT_TRUE(de_cooked_model_parse(r.data, r.size, &mv, NULL));
    EXPECT_EQ(mv.skeleton_count, 1u);
    EXPECT_EQ(mv.clip_count, 1u);
    EXPECT_EQ(de_cooked_model_skeleton_id(&mv, 0),
              de_asset_id_from("chars/cesium#skeleton:Armature").hash);
    EXPECT_EQ(de_cooked_model_clip_id(&mv, 0), de_asset_id_from("chars/cesium#anim:0").hash);
    bool skinned = false;
    for (u32 i = 0; i < mv.node_count; ++i)
        skinned = skinned || de_cooked_model_get_node(&mv, i).skin == 0u;
    EXPECT_TRUE(skinned);
    de_cook_result_free(&r);

    ASSERT_EQ(cook(f, "chars/figure", "RiggedFigure.glb", NULL, 0, &r), DE_OK);
    /* The mesh node Proxy is under Z_UP, the joints under Z_UP > Armature: Armature is a joint
     * (not a skin joint), placed before the first joint that needs it. */
    check_skeleton(&r, "skeleton:Armature", 20, 19, "torso_joint_1", &sk);
    EXPECT_STREQ(de_cooked_skeleton_name(&sk, 0), "Armature");
    EXPECT_EQ(de_cooked_skeleton_remap(&sk, 0), 1u);
    de_cook_result_free(&r);

    /* A skin listed child-first: the order is fixed, stable in source order. Nodes: R (root),
     * X and Y (children of R), C (child of Y). Skin joints [C, X, R, Y]. */
    de_model_ir *ir = &f->ir;
    ir->node_count = 4;
    ir->nodes = (de_model_node *)de_model_ir_array(ir, 4, sizeof(de_model_node));
    const char *nm[4] = {"R", "X", "Y", "C"};
    i32 par[4] = {-1, 0, 0, 2};
    for (u32 i = 0; i < 4; ++i) {
        ir->nodes[i].name = nm[i];
        ir->nodes[i].parent = par[i];
        ir->nodes[i].mesh = ir->nodes[i].skin = -1;
        ir->nodes[i].rotation[3] = 1.0f;
        ir->nodes[i].scale[0] = ir->nodes[i].scale[1] = ir->nodes[i].scale[2] = 1.0f;
        ir->nodes[i].translation[2] = i == 0 ? 0.0f : 1.0f;
    }
    ir->skin_count = 1;
    ir->skins = (de_model_skin *)de_model_ir_array(ir, 1, sizeof(de_model_skin));
    ir->skins[0].name = "S";
    ir->skins[0].joint_count = 4;
    ir->skins[0].joints = (u32 *)de_model_ir_array(ir, 4, sizeof(u32));
    ir->skins[0].inverse_bind = (f32 *)de_model_ir_array(ir, 64, sizeof(f32));
    ir->skins[0].skeleton_root = -1;
    u32 joints[4] = {3, 1, 0, 2};
    for (u32 i = 0; i < 4; ++i) {
        ir->skins[0].joints[i] = joints[i];
        for (u32 e = 0; e < 4; ++e)
            ir->skins[0].inverse_bind[16u * i + 5u * e] = 1.0f;
    }
    de_model_skeleton s;
    ASSERT_EQ(de_model_skeleton_build(ir, 0, &s), DE_OK);
    ASSERT_EQ(s.joint_count, 4u);
    EXPECT_EQ(s.mesh_node, -1); /* no node uses the skin: scene space */
    /* C needs Y needs R: R, Y, C; then X. */
    u32 want_nodes[4] = {0, 2, 3, 1};
    u16 want_parents[4] = {DE_COOKED_SKEL_NO_PARENT, 0, 1, 0};
    u16 want_remap[4] = {2, 3, 0, 1};
    for (u32 j = 0; j < 4; ++j) {
        EXPECT_EQ(s.nodes[j], want_nodes[j]);
        EXPECT_EQ(s.parents[j], want_parents[j]);
        EXPECT_EQ(s.remap[j], want_remap[j]);
    }
    EXPECT_EQ(s.root_joint, 0u);
}

/* ---- import_anim.skeleton.inverse_bind_identity ---------------------------------------------- */

static void check_inverse_bind(anim_fx *f, const char *source) {
    de_cook_result r;
    ASSERT_EQ(cook(f, "chars/x", source, NULL, 0, &r), DE_OK);
    const de_cook_subasset *s = sub(&r, "skeleton:Armature");
    ASSERT_TRUE(s != NULL);
    de_cooked_skeleton_view sk;
    ASSERT_TRUE(de_cooked_skeleton_parse(s->data, s->size, &sk, NULL));
    de_cooked_joint_pose pose[64];
    f64 model[64 * 12];
    ASSERT_LE(sk.joint_count, 64u);
    for (u32 j = 0; j < sk.joint_count; ++j)
        pose[j] = de_cooked_skeleton_rest(&sk, j);
    model_space(&sk, pose, model);
    f32 mfm32[12];
    f64 mfm[12];
    de_cooked_skeleton_mesh_from_model(&sk, mfm32);
    for (u32 k = 0; k < 12; ++k) {
        mfm[k] = mfm32[k];
        /* Both fixtures put the mesh node directly in the skeleton's frame. */
        EXPECT_FLOAT_EQ(mfm[k], (k == 0 || k == 4 || k == 8) ? 1.0 : 0.0, 1e-6);
    }
    f64 worst = 0.0;
    for (u32 j = 0; j < sk.joint_count; ++j) {
        f32 ibm32[12];
        f64 ibm[12], prod[12];
        de_cooked_skeleton_inverse_bind(&sk, j, ibm32);
        for (u32 k = 0; k < 12; ++k)
            ibm[k] = ibm32[k];
        /* The skin matrix in the rest pose: mesh_from_model x model x inverse bind. */
        aff_mul(model + 12u * j, ibm, prod);
        aff_mul(mfm, prod, prod);
        for (u32 k = 0; k < 12; ++k) {
            f64 want = (k == 0 || k == 4 || k == 8) ? 1.0 : 0.0;
            f64 d = fabs(prod[k] - want);
            worst = d > worst ? d : worst;
        }
    }
    printf("  %s: %u joints, largest |bind world x inverse bind - I| = %.3g\n", source,
           sk.joint_count, worst);
    EXPECT_FLE(worst, 1e-4);
    de_cook_result_free(&r);
}

TEST_F(skeleton, inverse_bind_identity, anim_fx) {
    check_inverse_bind(f, "CesiumMan.glb");
    check_inverse_bind(f, "RiggedFigure.glb");
}

/* ---- import_anim.clip.roundtrip_error -------------------------------------------------------- */

typedef struct roundtrip_result {
    f64 max_pos, max_ang;
    u32 times;
} roundtrip_result;

/* Cooks `source`, decodes the clip at every source key time and compares the model-space pose of
 * every joint with the source's. */
static roundtrip_result roundtrip(anim_fx *f, const char *root, const char *source,
                                  const char *skel_name, u32 anim, const char *clip_name) {
    roundtrip_result res = {0, 0, 0};
    de_cook_result r;
    ASSERT_EQ(cook_at(f, root, "chars/rt", source, NULL, 0, &r), DE_OK);
    const de_cook_subasset *ss = sub(&r, skel_name), *cs = sub(&r, clip_name);
    ASSERT_TRUE(ss != NULL && cs != NULL);
    EXPECT_STREQ(cs->type, "anim_clip");
    de_cooked_skeleton_view sk;
    de_cooked_clip_view cv;
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_skeleton_parse(ss->data, ss->size, &sk, &why));
    ASSERT_TRUE(de_cooked_clip_parse(cs->data, cs->size, &cv, &why));
    char skel_id[96];
    snprintf(skel_id, sizeof skel_id, "chars/rt#%s", skel_name);
    EXPECT_EQ(cv.skeleton_id, de_asset_id_from(skel_id).hash);
    /* Tracks of the root joint are flagged; the header names the root joint when one is. */
    bool root_track = false;
    for (u32 i = 0; i < cv.track_count; ++i) {
        de_cooked_clip_track t = de_cooked_clip_get_track(&cv, i);
        EXPECT_EQ((t.flags & DE_COOKED_CLIP_TRACK_ROOT) != 0, t.joint == sk.root_joint);
        root_track = root_track || t.joint == sk.root_joint;
    }
    EXPECT_EQ(cv.root_joint, root_track ? sk.root_joint : DE_COOKED_NONE);

    de_model_ir_free(&f->ir);
    de_model_ir_init(&f->ir, &f->alloc, "test");
    if (f->file != NULL) {
        DE_FREE(&f->alloc, f->file, (size_t)f->file_size + 1u);
        f->file = NULL;
    }
    load_engine_ir(f, root, source);
    ASSERT_LT(anim, f->ir.anim_count);
    u32 node_of_joint[64];
    ASSERT_LE(sk.joint_count, 64u);
    match_joints(f, &sk, node_of_joint);
    const de_model_anim *an = &f->ir.anims[anim];
    de_cooked_joint_pose dec[64], src[64];
    f64 tr[64 * 10], md[64 * 12], ms[64 * 12];
    for (u32 c = 0; c < an->channel_count; ++c) {
        const de_model_anim_channel *ch = &an->channels[c];
        for (u32 k = 0; k < ch->key_count; ++k) {
            f64 t = ch->times[k];
            ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, (f32)t, dec));
            source_pose(&f->ir, &sk, node_of_joint, an, t, src, tr);
            for (u32 j = 0; j < sk.joint_count; ++j) {
                f64 l[12];
                trs_affine(tr + 10u * j, tr + 10u * j + 3, tr + 10u * j + 7, l);
                u16 p = de_cooked_skeleton_parent(&sk, j);
                if (p == DE_COOKED_SKEL_NO_PARENT)
                    memcpy(ms + 12u * j, l, sizeof l);
                else
                    aff_mul(ms + 12u * p, l, ms + 12u * j);
            }
            model_space(&sk, dec, md);
            for (u32 j = 0; j < sk.joint_count; ++j) {
                f64 dp = pos_diff(md + 12u * j, ms + 12u * j);
                f64 da = orient_diff(md + 12u * j, ms + 12u * j);
                res.max_pos = dp > res.max_pos ? dp : res.max_pos;
                res.max_ang = da > res.max_ang ? da : res.max_ang;
            }
            ++res.times;
        }
    }
    printf("  %s: %u key times, %u tracks, %u samples, largest error %.3g mm, %.4g deg\n", source,
           res.times, cv.track_count, cv.time_count, res.max_pos * 1000.0, res.max_ang / DEG);
    de_cook_result_free(&r);
    return res;
}

TEST_F(clip, roundtrip_error, anim_fx) {
    /* The defaults: 0.1 mm and 0.05 degrees (a small slack for the test's own f64 math). */
    const f64 tol_p = 1e-4 + 1e-7, tol_a = 0.05 * DEG + 1e-7;
    roundtrip_result a =
        roundtrip(f, "animdata", "CesiumMan.glb", "skeleton:Armature", 0, "anim:0");
    EXPECT_GT(a.times, 0u);
    EXPECT_FLE(a.max_pos, tol_p);
    EXPECT_FLE(a.max_ang, tol_a);
    roundtrip_result b =
        roundtrip(f, "animdata", "RiggedFigure.glb", "skeleton:Armature", 0, "anim:0");
    EXPECT_FLE(b.max_pos, tol_p);
    EXPECT_FLE(b.max_ang, tol_a);
}

/* ---- In-memory clips ------------------------------------------------------------------------- */

/* A chain of `n` joints 0.5 m apart along Z, one skin over all, one animation with room for
 * `channels` channels. */
static void synth_chain(de_model_ir *ir, u32 n, u32 channels) {
    ir->node_count = n;
    ir->nodes = (de_model_node *)de_model_ir_array(ir, n, sizeof(de_model_node));
    ir->skin_count = 1;
    ir->skins = (de_model_skin *)de_model_ir_array(ir, 1, sizeof(de_model_skin));
    ir->skins[0].name = "S";
    ir->skins[0].joint_count = n;
    ir->skins[0].joints = (u32 *)de_model_ir_array(ir, n, sizeof(u32));
    ir->skins[0].inverse_bind = (f32 *)de_model_ir_array(ir, 16u * n, sizeof(f32));
    ir->skins[0].skeleton_root = -1;
    for (u32 i = 0; i < n; ++i) {
        char nm[16];
        snprintf(nm, sizeof nm, "j%u", i);
        de_model_node *nd = &ir->nodes[i];
        nd->name = de_model_ir_strdup(ir, nm);
        nd->parent = (i32)i - 1;
        nd->mesh = nd->skin = -1;
        nd->rotation[3] = 1.0f;
        nd->scale[0] = nd->scale[1] = nd->scale[2] = 1.0f;
        nd->translation[2] = i == 0 ? 0.0f : 0.5f;
        ir->skins[0].joints[i] = i;
        for (u32 e = 0; e < 4; ++e)
            ir->skins[0].inverse_bind[16u * i + 5u * e] = 1.0f;
        ir->skins[0].inverse_bind[16u * i + 14u] = -0.5f * (f32)i;
    }
    ir->anim_count = 1;
    ir->anims = (de_model_anim *)de_model_ir_array(ir, 1, sizeof(de_model_anim));
    ir->anims[0].name = "A";
    ir->anims[0].channels =
        (de_model_anim_channel *)de_model_ir_array(ir, channels, sizeof(de_model_anim_channel));
}

static de_model_anim_channel *synth_channel(de_model_ir *ir, u32 node, de_model_anim_path path,
                                            de_model_anim_interp interp, u32 keys) {
    de_model_anim *an = &ir->anims[0];
    de_model_anim_channel *c = &an->channels[an->channel_count++];
    c->node = node;
    c->path = path;
    c->interp = interp;
    c->key_count = keys;
    c->components = path == DE_MODEL_ANIM_ROTATION ? 4u : 3u;
    u32 per = interp == DE_MODEL_INTERP_CUBIC ? 3u : 1u;
    c->times = (f32 *)de_model_ir_array(ir, keys, sizeof(f32));
    c->values = (f32 *)de_model_ir_array(ir, (u64)keys * per * c->components, sizeof(f32));
    return c;
}

/* Cooks ir->anims[0] over the skeleton of skin 0; the parsed views point into `buf`. */
static void synth_cook(de_model_ir *ir, const de_model_anim_settings *s, de_model_buf *sbuf,
                       de_model_buf *cbuf, de_cooked_skeleton_view *sk, de_cooked_clip_view *cv,
                       de_model_clip_stats *st) {
    de_model_names names;
    ASSERT_EQ(de_model_names_build(ir, &names), DE_OK);
    de_model_skeleton skel;
    ASSERT_EQ(de_model_skeleton_build(ir, 0, &skel), DE_OK);
    de_model_buf_init(sbuf, ir);
    de_model_skeleton_write(sbuf, ir, &skel, &names, true);
    de_model_buf_init(cbuf, ir);
    ASSERT_EQ(de_model_clip_write(cbuf, ir, &ir->anims[0], &skel, 42u, s, st), DE_OK);
    const char *why = NULL;
    ASSERT_TRUE(de_cooked_skeleton_parse(sbuf->data, sbuf->size, sk, &why));
    ASSERT_TRUE(de_cooked_clip_parse(cbuf->data, cbuf->size, cv, &why));
    EXPECT_EQ(cv->skeleton_id, 42u);
}

static i32 find_track(const de_cooked_clip_view *cv, u32 joint, u32 channel,
                      de_cooked_clip_track *out) {
    memset(out, 0, sizeof *out);
    for (u32 i = 0; i < cv->track_count; ++i) {
        *out = de_cooked_clip_get_track(cv, i);
        if (out->joint == joint && out->channel == channel)
            return (i32)i;
    }
    return -1;
}

/* ---- import_anim.clip.constant_tracks -------------------------------------------------------- */

TEST_F(clip, constant_tracks, anim_fx) {
    /* CesiumMan: every scale track is constant (1 within 1e-7) and stored as one value. */
    de_cook_result r;
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", NULL, 0, &r), DE_OK);
    de_cooked_clip_view cv;
    ASSERT_TRUE(de_cooked_clip_parse(sub(&r, "anim:0")->data, sub(&r, "anim:0")->size, &cv, NULL));
    u32 scale_tracks = 0, const_uniform = 0, consts = 0;
    for (u32 i = 0; i < cv.track_count; ++i) {
        de_cooked_clip_track t = de_cooked_clip_get_track(&cv, i);
        if (t.encoding == DE_COOKED_CLIP_ENC_CONST)
            ++consts;
        if (t.channel != DE_COOKED_CLIP_CH_SCALE)
            continue;
        ++scale_tracks;
        if (t.encoding == DE_COOKED_CLIP_ENC_CONST && t.components == 1u &&
            (t.flags & DE_COOKED_CLIP_TRACK_UNIFORM) != 0)
            ++const_uniform;
    }
    printf("  CesiumMan: %u of %u tracks constant\n", consts, cv.track_count);
    EXPECT_EQ(scale_tracks, 19u);
    EXPECT_EQ(const_uniform, scale_tracks);
    de_cook_result_free(&r);

    /* In memory: joint 1 translation constant, rotation constant (one key flipped in sign: the
     * same rotation), scale animated but uniform; joint 2 scale animated and not uniform. */
    de_model_ir *ir = &f->ir;
    synth_chain(ir, 3, 8);
    de_model_anim_channel *c;
    c = synth_channel(ir, 1, DE_MODEL_ANIM_TRANSLATION, DE_MODEL_INTERP_LINEAR, 5);
    for (u32 k = 0; k < 5; ++k) {
        c->times[k] = 0.25f * (f32)k;
        c->values[3 * k + 0] = 0.1f;
        c->values[3 * k + 1] = -0.2f;
        c->values[3 * k + 2] = 0.5f;
    }
    c = synth_channel(ir, 1, DE_MODEL_ANIM_ROTATION, DE_MODEL_INTERP_LINEAR, 5);
    for (u32 k = 0; k < 5; ++k) {
        f32 sg = (k % 2u) ? -1.0f : 1.0f;
        c->times[k] = 0.25f * (f32)k;
        c->values[4 * k + 0] = 0.0f;
        c->values[4 * k + 1] = sg * 0.6f;
        c->values[4 * k + 2] = 0.0f;
        c->values[4 * k + 3] = sg * 0.8f;
    }
    c = synth_channel(ir, 1, DE_MODEL_ANIM_SCALE, DE_MODEL_INTERP_LINEAR, 5);
    for (u32 k = 0; k < 5; ++k) {
        c->times[k] = 0.25f * (f32)k;
        c->values[3 * k + 0] = c->values[3 * k + 1] = c->values[3 * k + 2] = 1.0f + 0.1f * (f32)k;
    }
    c = synth_channel(ir, 2, DE_MODEL_ANIM_SCALE, DE_MODEL_INTERP_LINEAR, 5);
    for (u32 k = 0; k < 5; ++k) {
        c->times[k] = 0.25f * (f32)k;
        c->values[3 * k + 0] = 1.0f + 0.1f * (f32)k;
        c->values[3 * k + 1] = 1.0f;
        c->values[3 * k + 2] = 1.0f - 0.05f * (f32)k;
    }
    de_model_anim_settings s;
    de_model_anim_settings_default(&s);
    de_model_buf sb, cb;
    de_cooked_skeleton_view sk;
    de_model_clip_stats st;
    synth_cook(ir, &s, &sb, &cb, &sk, &cv, &st);
    EXPECT_EQ(cv.track_count, 4u);
    de_cooked_clip_track t = {0};
    ASSERT_GE(find_track(&cv, 1, DE_COOKED_CLIP_CH_TRANSLATION, &t), 0);
    EXPECT_EQ(t.encoding, (u32)DE_COOKED_CLIP_ENC_CONST);
    EXPECT_EQ(t.components, 3u);
    ASSERT_GE(find_track(&cv, 1, DE_COOKED_CLIP_CH_ROTATION, &t), 0);
    EXPECT_EQ(t.encoding, (u32)DE_COOKED_CLIP_ENC_CONST);
    ASSERT_GE(find_track(&cv, 1, DE_COOKED_CLIP_CH_SCALE, &t), 0);
    EXPECT_NE(t.encoding, (u32)DE_COOKED_CLIP_ENC_CONST);
    EXPECT_EQ(t.components, 1u);
    EXPECT_TRUE((t.flags & DE_COOKED_CLIP_TRACK_UNIFORM) != 0);
    /* A linear ramp keeps only its end keys. */
    EXPECT_EQ(t.key_count, 2u);
    ASSERT_GE(find_track(&cv, 2, DE_COOKED_CLIP_CH_SCALE, &t), 0);
    EXPECT_EQ(t.components, 3u);
    EXPECT_EQ(st.const_tracks, 2u);
    /* The decoded constant rotation is the source one, up to sign. */
    de_cooked_joint_pose pose[3];
    ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, 0.6f, pose));
    EXPECT_FLOAT_EQ(fabs(pose[1].r[1]), 0.6, 1e-6);
    EXPECT_FLOAT_EQ(fabs(pose[1].r[3]), 0.8, 1e-6);
    EXPECT_FLOAT_EQ(pose[1].t[1], -0.2, 1e-6);
    EXPECT_FLOAT_EQ(pose[1].s[0], 1.24, 1e-4);
    EXPECT_FLOAT_EQ(pose[1].s[2], 1.24, 1e-4);
    /* Joints without a track keep the rest pose. */
    EXPECT_FLOAT_EQ(pose[2].t[2], 0.5, 0.0);
}

/* ---- import_anim.clip.smallest_three_quat ---------------------------------------------------- */

static u32 lcg(u32 *s) {
    *s = *s * 1664525u + 1013904223u;
    return *s >> 8;
}

TEST(clip, smallest_three_quat) {
    u32 seed = 12345u;
    f64 worst = 0.0;
    u32 by_index[4] = {0, 0, 0, 0};
    for (u32 n = 0; n < 4000; ++n) {
        f64 q[4], l = 0.0;
        for (u32 i = 0; i < 4; ++i) {
            q[i] = (f64)lcg(&seed) / (f64)(1u << 24) * 2.0 - 1.0;
            l += q[i] * q[i];
        }
        if (l < 1e-6)
            continue;
        for (u32 i = 0; i < 4; ++i)
            q[i] /= sqrt(l);
        u16 w[3], w2[3];
        f32 d[4], d2[4];
        de_model_clip_s3_encode(q, w, d);
        f64 nq[4] = {-q[0], -q[1], -q[2], -q[3]};
        de_model_clip_s3_encode(nq, w2, d2);
        EXPECT_TRUE(w[0] == w2[0] && w[1] == w2[1] && w[2] == w2[2]);
        /* The decoded value is what the sampler reads from the bytes. */
        u8 b[6] = {(u8)w[0], (u8)(w[0] >> 8), (u8)w[1], (u8)(w[1] >> 8), (u8)w[2], (u8)(w[2] >> 8)};
        f32 dd[4];
        de_cooked_clip_s3_decode(b, dd);
        for (u32 i = 0; i < 4; ++i) /* this file may contract a*b+c, the cooker does not */
            EXPECT_FLOAT_EQ(dd[i], d[i], 1e-6);
        u32 idx = (u32)(((u64)w[2] << 32 | (u64)w[1] << 16 | w[0]) >> 45) & 3u;
        ++by_index[idx];
        EXPECT_FGE(d[idx], 0.0f);
        /* The largest source component is the dropped one. */
        for (u32 i = 0; i < 4; ++i)
            EXPECT_FLE(fabs(q[i]), fabs(q[idx]) + 1e-12);
        f64 dot = 0.0, dl = 0.0;
        for (u32 i = 0; i < 4; ++i) {
            dot += q[i] * d[i];
            dl += (f64)d[i] * d[i];
        }
        EXPECT_FLOAT_EQ(dl, 1.0, 2e-4);
        f64 ang = 2.0 * acos(fmin(1.0, fabs(dot) / sqrt(dl)));
        worst = ang > worst ? ang : worst;
    }
    printf("  4000 quaternions: largest error %.3g rad (%.4f deg); index use %u %u %u %u\n", worst,
           worst / DEG, by_index[0], by_index[1], by_index[2], by_index[3]);
    /* 15 bits over [-1/sqrt2, 1/sqrt2]: half a step is 2.2e-5 per component. */
    EXPECT_FLE(worst, 1.2e-4);
    for (u32 i = 0; i < 4; ++i)
        EXPECT_GT(by_index[i], 0u);
    /* Identity: index 3, the other three in the middle of the range. */
    f64 id[4] = {0, 0, 0, 1};
    u16 w[3];
    f32 d[4];
    de_model_clip_s3_encode(id, w, d);
    u64 bits = (u64)w[2] << 32 | (u64)w[1] << 16 | w[0];
    EXPECT_EQ((u32)(bits >> 45), 3u);
    u32 mid = (u32)(bits & 0x7FFFu);
    EXPECT_TRUE(mid == 16383u || mid == 16384u);
    EXPECT_FLOAT_EQ(d[3], 1.0, 1e-6);
}

/* ---- import_anim.clip.hemisphere_continuity -------------------------------------------------- */

TEST_F(clip, hemisphere_continuity, anim_fx) {
    /* Joint 1 turns 0..340 degrees about Z in 20-degree linear keys over 1.7 s; every other key
     * is stored negated (the same rotation, other hemisphere). The largest component changes from
     * w to z on the way, so the smallest-three sign normalisation also flips. */
    de_model_ir *ir = &f->ir;
    synth_chain(ir, 2, 1);
    const u32 keys = 18;
    de_model_anim_channel *c =
        synth_channel(ir, 1, DE_MODEL_ANIM_ROTATION, DE_MODEL_INTERP_LINEAR, keys);
    for (u32 k = 0; k < keys; ++k) {
        f64 th = 20.0 * DEG * (f64)k;
        f32 sg = (k % 2u) ? -1.0f : 1.0f;
        c->times[k] = 0.1f * (f32)k;
        c->values[4 * k + 2] = sg * (f32)sin(th / 2);
        c->values[4 * k + 3] = sg * (f32)cos(th / 2);
    }
    de_model_anim_settings s;
    de_model_anim_settings_default(&s);
    de_model_buf sb, cb;
    de_cooked_skeleton_view sk;
    de_cooked_clip_view cv;
    de_model_clip_stats st;
    synth_cook(ir, &s, &sb, &cb, &sk, &cv, &st);
    de_cooked_clip_track t = {0};
    ASSERT_GE(find_track(&cv, 1, DE_COOKED_CLIP_CH_ROTATION, &t), 0);
    EXPECT_EQ(t.encoding, (u32)DE_COOKED_CLIP_ENC_SMALLEST3);
    /* Sample densely, also between keys: the error to the shortest-arc slerp of the source stays
     * near the tolerance, never near 180 degrees (a long-way interpolation). */
    f64 worst = 0.0, worst_key = 0.0;
    for (u32 i = 0; i <= 340; ++i) {
        f64 tm = 1.7 * (f64)i / 340.0;
        de_cooked_joint_pose pose[2];
        ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, (f32)tm, pose));
        f64 th = 200.0 * DEG * tm; /* 20 degrees per 0.1 s */
        f64 q[4] = {0, 0, sin(th / 2), cos(th / 2)};
        f64 dot = 0.0;
        for (u32 k = 0; k < 4; ++k)
            dot += q[k] * pose[1].r[k];
        f64 ang = 2.0 * acos(fmin(1.0, fabs(dot)));
        worst = ang > worst ? ang : worst;
        if (i % 20u == 0)
            worst_key = ang > worst_key ? ang : worst_key;
    }
    printf("  hemisphere flips: largest error %.4f deg (at keys %.4f deg), %u keys kept of %u\n",
           worst / DEG, worst_key / DEG, t.key_count, cv.time_count);
    EXPECT_FLE(worst_key, 0.05 * DEG);
    /* Between 20-degree keys, nlerp and slerp differ by well under a degree. */
    EXPECT_FLE(worst, 0.5 * DEG);
}

/* ---- import_anim.clip.resample_interpolation ------------------------------------------------- */

TEST_F(clip, resample_interpolation, anim_fx) {
    /* Step translation keys and a cubic-spline translation: at every sample of the time table the
     * decoded value matches the source evaluated with its own interpolation. */
    de_model_ir *ir = &f->ir;
    synth_chain(ir, 3, 2);
    de_model_anim_channel *st =
        synth_channel(ir, 1, DE_MODEL_ANIM_TRANSLATION, DE_MODEL_INTERP_STEP, 4);
    const f32 st_times[4] = {0.0f, 0.3f, 0.6f, 0.9f};
    for (u32 k = 0; k < 4; ++k) {
        st->times[k] = st_times[k];
        st->values[3 * k + 0] = 0.1f * (f32)(k * k);
        st->values[3 * k + 2] = 0.5f;
    }
    de_model_anim_channel *cu =
        synth_channel(ir, 2, DE_MODEL_ANIM_TRANSLATION, DE_MODEL_INTERP_CUBIC, 3);
    f32 cv_vals[3][3][3] = {
        /* in-tangent, value, out-tangent */
        {{0, 0, 0}, {0, 0, 0.5f}, {1, 0, 0}},
        {{0, 2, 0}, {0.4f, 0.2f, 0.5f}, {0, 2, 0}},
        {{-1, 0, 0}, {0, 0.6f, 0.5f}, {0, 0, 0}},
    };
    const f32 cu_times[3] = {0.0f, 0.45f, 0.9f};
    for (u32 k = 0; k < 3; ++k) {
        cu->times[k] = cu_times[k];
        memcpy(cu->values + 9 * k, cv_vals[k], sizeof cv_vals[k]);
    }
    de_model_anim_settings s;
    de_model_anim_settings_default(&s);
    de_model_buf sb, cb;
    de_cooked_skeleton_view sk;
    de_cooked_clip_view cv;
    de_model_clip_stats stats;
    synth_cook(ir, &s, &sb, &cb, &sk, &cv, &stats);
    /* 0.9 s at 30 Hz: 28 grid points; the step keys 0.3 and 0.6 are on the grid, the cubic key
     * 0.45 is not and adds a sample. */
    EXPECT_FLOAT_EQ(cv.duration, 0.9, 1e-6);
    EXPECT_EQ(cv.time_count, 29u);
    f64 worst = 0.0;
    for (u32 i = 0; i < cv.time_count; ++i) {
        f32 tm = de_cooked_clip_time(&cv, i);
        de_cooked_joint_pose pose[3];
        ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, tm, pose));
        f64 a[4], b[4];
        chan_value(st, tm, a);
        chan_value(cu, tm, b);
        for (u32 k = 0; k < 3; ++k) {
            worst = fmax(worst, fabs(pose[1].t[k] - a[k]));
            worst = fmax(worst, fabs(pose[2].t[k] - b[k]));
        }
    }
    printf("  step + cubic: %u samples, largest error %.3g m\n", cv.time_count, worst);
    EXPECT_FLE(worst, 1e-4);
    /* The step holds its value up to the next key: the sample just before 0.3 s is key 0. */
    de_cooked_joint_pose pose[3];
    ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, de_cooked_clip_time(&cv, 8), pose));
    EXPECT_FLOAT_EQ(pose[1].t[0], 0.0, 1e-4);
    ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, de_cooked_clip_time(&cv, 9), pose));
    EXPECT_FLOAT_EQ(pose[1].t[0], 0.1, 1e-4);
}

/* ---- import_anim.clip.compression_ratio ------------------------------------------------------ */

static f64 ratio_of(anim_fx *f, const char *source) {
    de_cook_result r;
    de_result res = cook(f, "chars/ratio", source, NULL, 0, &r);
    EXPECT_EQ(res, DE_OK);
    if (res != DE_OK)
        return 0.0;
    const de_cook_subasset *s = sub(&r, "anim:0");
    de_cooked_clip_view cv;
    memset(&cv, 0, sizeof cv);
    EXPECT_TRUE(s != NULL && de_cooked_clip_parse(s->data, s->size, &cv, NULL));
    if (s == NULL || cv.base == NULL) {
        de_cook_result_free(&r);
        return 0.0;
    }
    /* Raw: every animated joint as f32 TRS (40 bytes) at 30 Hz. */
    u32 joints = 0, last = 0xFFFFFFFFu;
    u32 consts = 0, keys = 0, by_enc[DE_COOKED_CLIP_ENC_COUNT] = {0, 0, 0, 0};
    for (u32 i = 0; i < cv.track_count; ++i) {
        de_cooked_clip_track t = de_cooked_clip_get_track(&cv, i);
        if (t.joint != last)
            ++joints;
        last = t.joint;
        if (t.encoding == DE_COOKED_CLIP_ENC_CONST)
            ++consts;
        else
            keys += t.key_count;
        ++by_enc[t.encoding];
    }
    u32 frames = (u32)floor((f64)cv.duration * 30.0 + 0.5) + 1u;
    f64 raw = (f64)joints * frames * 40.0;
    f64 ratio = raw / (f64)s->size;
    printf("  %s: raw f32 TRS at 30 Hz %u joints x %u frames = %.0f bytes; cooked %llu bytes "
           "(%u tracks, %u constant, %u keys); ratio %.2fx\n",
           source, joints, frames, raw, (unsigned long long)s->size, cv.track_count, consts, keys,
           ratio);
    printf("    encodings: %u constant, %u quant16, %u raw, %u smallest-three; %u samples\n",
           by_enc[DE_COOKED_CLIP_ENC_CONST], by_enc[DE_COOKED_CLIP_ENC_QUANT16],
           by_enc[DE_COOKED_CLIP_ENC_RAW], by_enc[DE_COOKED_CLIP_ENC_SMALLEST3], cv.time_count);
    de_cook_result_free(&r);
    return ratio;
}

TEST_F(clip, compression_ratio, anim_fx) {
    EXPECT_FGE(ratio_of(f, "CesiumMan.glb"), 4.0);
    EXPECT_FGE(ratio_of(f, "RiggedFigure.glb"), 4.0);
}

/* ---- import_anim.clip.cook_deterministic ----------------------------------------------------- */

TEST_F(clip, cook_deterministic, anim_fx) {
    /* verify_determinism is on: each cook runs the importer twice and compares the bytes. */
    de_cook_result a, b;
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", NULL, 0, &a), DE_OK);
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", NULL, 0, &b), DE_OK);
    EXPECT_EQ(a.output_hash, b.output_hash);
    ASSERT_EQ(a.subasset_count, b.subasset_count);
    for (u32 i = 0; i < a.subasset_count; ++i) {
        EXPECT_STREQ(a.subassets[i].name, b.subassets[i].name);
        ASSERT_EQ(a.subassets[i].size, b.subassets[i].size);
        EXPECT_MEMEQ(a.subassets[i].data, b.subassets[i].data, (size_t)a.subassets[i].size);
    }
    /* Order: textures, materials, meshes, skeleton, clip, prefab. */
    ASSERT_GE(a.subasset_count, 3u);
    EXPECT_STREQ(a.subassets[a.subasset_count - 3u].name, "skeleton:Armature");
    EXPECT_STREQ(a.subassets[a.subasset_count - 2u].name, "anim:0");
    EXPECT_STREQ(a.subassets[a.subasset_count - 1u].name, "prefab");
    bool dep_s = false, dep_c = false;
    for (u32 i = 0; i < a.dependency_count; ++i) {
        dep_s = dep_s ||
                a.dependencies[i].hash == de_asset_id_from("chars/cesium#skeleton:Armature").hash;
        dep_c = dep_c || a.dependencies[i].hash == de_asset_id_from("chars/cesium#anim:0").hash;
    }
    EXPECT_TRUE(dep_s && dep_c);
    de_cook_result_free(&b);

    /* anim_sample_rate and anim_error are in the settings hash and change the clip. */
    const de_cook_subasset *ca = sub(&a, "anim:0");
    ASSERT_TRUE(ca != NULL);
    de__manifest_kv kv[1] = {kv_float("anim_sample_rate", 60.0)};
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", kv, 1, &b), DE_OK);
    EXPECT_NE(a.settings_hash, b.settings_hash);
    const de_cook_subasset *cb = sub(&b, "anim:0");
    ASSERT_TRUE(cb != NULL);
    EXPECT_TRUE(ca->size != cb->size || memcmp(ca->data, cb->data, (size_t)ca->size) != 0);
    de_cook_result_free(&b);
    kv[0] = kv_float("anim_error", 0.002);
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", kv, 1, &b), DE_OK);
    EXPECT_NE(a.settings_hash, b.settings_hash);
    cb = sub(&b, "anim:0");
    ASSERT_TRUE(cb != NULL);
    printf("  CesiumMan clip: %llu bytes at anim_error 0.1 mm, %llu bytes at 2 mm\n",
           (unsigned long long)ca->size, (unsigned long long)cb->size);
    EXPECT_LT(cb->size, ca->size);
    de_cook_result_free(&b);
    kv[0] = kv_float("anim_error_angle", 0.5);
    ASSERT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", kv, 1, &b), DE_OK);
    EXPECT_NE(a.settings_hash, b.settings_hash);
    de_cook_result_free(&b);
    de_cook_result_free(&a);

    /* A bad setting fails the cook with a message. */
    kv[0] = kv_float("anim_sample_rate", 0.0);
    EXPECT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", kv, 1, &b), DE_ERR_INVALID_ARGUMENT);
    kv[0] = kv_float("anim_error", -1.0);
    EXPECT_EQ(cook(f, "chars/cesium", "CesiumMan.glb", kv, 1, &b), DE_ERR_INVALID_ARGUMENT);
}

/* ---- import_anim.clip.step_fixture ----------------------------------------------------------- */

TEST_F(clip, step_fixture, anim_fx) {
    /* DENG-101's skinned.gltf: a STEP rotation and a linear translation over two joints. */
    roundtrip_result r = roundtrip(f, "animgltf", "skinned.gltf", "skeleton:Rig", 0, "anim:Walk");
    EXPECT_GT(r.times, 0u);
    EXPECT_FLE(r.max_pos, 1e-4 + 1e-7);
    EXPECT_FLE(r.max_ang, 0.05 * DEG + 1e-7);
}

/* ---- import_anim.clip.reject_bad_input ------------------------------------------------------- */

TEST_F(clip, reject_bad_input, anim_fx) {
    de_model_ir *ir = &f->ir;
    synth_chain(ir, 2, 1);
    de_model_anim_channel *c =
        synth_channel(ir, 1, DE_MODEL_ANIM_TRANSLATION, DE_MODEL_INTERP_LINEAR, 3);
    c->times[0] = 0.0f;
    c->times[1] = 0.5f;
    c->times[2] = 0.25f; /* decreasing */
    de_model_anim_settings s;
    de_model_anim_settings_default(&s);
    de_model_skeleton skel;
    ASSERT_EQ(de_model_skeleton_build(ir, 0, &skel), DE_OK);
    de_model_buf b;
    de_model_buf_init(&b, ir);
    EXPECT_EQ(de_model_clip_write(&b, ir, &ir->anims[0], &skel, 1u, &s, NULL), DE_ERR_PARSE);
    c->times[2] = 1.0f;
    c->values[4] = NAN;
    EXPECT_EQ(de_model_clip_write(&b, ir, &ir->anims[0], &skel, 1u, &s, NULL), DE_ERR_PARSE);
    c->values[4] = 0.0f;
    /* Too many samples for u16 time indices. */
    c->times[2] = 5000.0f;
    EXPECT_EQ(de_model_clip_write(&b, ir, &ir->anims[0], &skel, 1u, &s, NULL), DE_ERR_UNSUPPORTED);
    /* The format parsers refuse truncated blobs. */
    c->times[2] = 1.0f;
    de_model_buf_init(&b, ir);
    ASSERT_EQ(de_model_clip_write(&b, ir, &ir->anims[0], &skel, 1u, &s, NULL), DE_OK);
    de_cooked_clip_view cv;
    EXPECT_TRUE(de_cooked_clip_parse(b.data, b.size, &cv, NULL));
    for (u64 n = 0; n < b.size; n += 4)
        EXPECT_FALSE(de_cooked_clip_parse(b.data, n, &cv, NULL));
    de_model_names names;
    ASSERT_EQ(de_model_names_build(ir, &names), DE_OK);
    de_model_buf sb;
    de_model_buf_init(&sb, ir);
    de_model_skeleton_write(&sb, ir, &skel, &names, true);
    de_cooked_skeleton_view sk;
    EXPECT_TRUE(de_cooked_skeleton_parse(sb.data, sb.size, &sk, NULL));
    for (u64 n = 0; n < sb.size; n += 4)
        EXPECT_FALSE(de_cooked_skeleton_parse(sb.data, n, &sk, NULL));
}

/* ---- import_anim.clip.fbx_roundtrip ---------------------------------------------------------- */

TEST_F(clip, fbx_roundtrip, anim_fx) {
    /* DENG-102's skinned.fbx (Maya axes, centimetres): a 5-bone chain and two animation stacks,
     * through the same IR as glTF. Both stacks round-trip at every source key time. */
    roundtrip_result a = roundtrip(f, "animfbx", "skinned.fbx", "skeleton:Skin", 0, "anim:Wave");
    EXPECT_GT(a.times, 0u);
    EXPECT_FLE(a.max_pos, 1e-4 + 1e-7);
    EXPECT_FLE(a.max_ang, 0.05 * DEG + 1e-7);
    roundtrip_result b = roundtrip(f, "animfbx", "skinned.fbx", "skeleton:Skin", 1, "anim:Idle");
    EXPECT_GT(b.times, 0u);
    EXPECT_FLE(b.max_pos, 1e-4 + 1e-7);
    EXPECT_FLE(b.max_ang, 0.05 * DEG + 1e-7);
}

/* ---- import_anim.clip.long_clip -------------------------------------------------------------- */

/* Replaces every channel of f->ir.anims[0] (linear or step) by the clip, its key times
 * multiplied by `stretch`, played forward and backward in turn until it lasts about `seconds`: a
 * long take with no seams. Returns the new duration. */
static f64 lengthen_anim(anim_fx *f, f64 seconds, f64 stretch) {
    de_model_ir *ir = &f->ir;
    de_model_anim *an = &ir->anims[0];
    f64 d = 0.0;
    for (u32 c = 0; c < an->channel_count; ++c)
        if (an->channels[c].key_count != 0)
            d = fmax(d, (f64)an->channels[c].times[an->channels[c].key_count - 1u]);
    d *= stretch;
    u32 cycles = (u32)ceil(seconds / d);
    for (u32 c = 0; c < an->channel_count; ++c) {
        de_model_anim_channel *ch = &an->channels[c];
        ASSERT_TRUE(ch->interp != DE_MODEL_INTERP_CUBIC);
        u32 kc = ch->key_count, n = ch->components, out = 0;
        f32 *times = (f32 *)de_model_ir_array(ir, (u64)kc * cycles, sizeof(f32));
        f32 *values = (f32 *)de_model_ir_array(ir, (u64)kc * cycles * n, sizeof(f32));
        ASSERT_TRUE(times != NULL && values != NULL);
        for (u32 y = 0; y < cycles; ++y)
            for (u32 k = 0; k < kc; ++k) {
                u32 src = (y % 2u) == 0 ? k : kc - 1u - k;
                f64 ts = stretch * (f64)ch->times[src];
                f64 t = (f64)y * d + ((y % 2u) == 0 ? ts : d - ts);
                if (out > 0 && !((f32)t > times[out - 1u]))
                    continue;
                times[out] = (f32)t;
                memcpy(values + (u64)out * n, ch->values + (u64)src * n, sizeof(f32) * n);
                ++out;
            }
        ch->times = times;
        ch->values = values;
        ch->key_count = out;
    }
    an->duration = (f32)(d * cycles);
    return d * cycles;
}

/* Cooks f->ir.anims[0] (lengthened to `seconds`, keys `stretch` times further apart) over the
 * CesiumMan skeleton and checks the error at every 7th sample against the source. The cook time
 * is printed, not checked (Testing: no wall-clock bounds in unit tests); the issue's budget is 2 s
 * for 5 minutes in a Release build. */
static void long_clip(anim_fx *f, f64 seconds, f64 stretch, de_model_clip_stats *st) {
    load_engine_ir(f, "animdata", "CesiumMan.glb");
    ASSERT_GE(f->ir.anim_count, 1u);
    f64 dur = lengthen_anim(f, seconds, stretch);
    de_model_anim_settings s;
    de_model_anim_settings_default(&s);
    de_model_buf sb, cb;
    de_cooked_skeleton_view sk;
    de_cooked_clip_view cv;
    memset(&sk, 0, sizeof sk);
    memset(&cv, 0, sizeof cv);
    u64 t0 = de_time_now_ns();
    synth_cook(&f->ir, &s, &sb, &cb, &sk, &cv, st);
    f64 ms = (f64)(de_time_now_ns() - t0) / 1e6;
    printf(
        "  %.0f s clip, keys every %.3g s: %u samples, %u tracks (%u constant, %u raw), %u keys, "
        "%u passes, %llu bytes, %.3g mm, %.4g deg; cooked in %.0f ms\n",
        dur, stretch / 24.0, st->times, st->tracks, st->const_tracks, st->raw_tracks, st->keys,
        st->iterations, (unsigned long long)st->bytes, st->max_position * 1000.0,
        st->max_angle / DEG, ms);
    EXPECT_FGE(cv.duration, seconds - 1.0);
    EXPECT_FLE(st->max_position, (f64)s.error);
    EXPECT_FLE(st->max_angle, (f64)s.error_angle);
    /* Independent check: the model-space pose at every 7th sample, against the source. */
    ASSERT_LE(sk.joint_count, 64u);
    u32 node_of_joint[64];
    match_joints(f, &sk, node_of_joint);
    de_cooked_joint_pose dec[64], src[64];
    f64 tr[64 * 10], md[64 * 12], msrc[64 * 12], max_pos = 0.0, max_ang = 0.0;
    for (u32 i = 0; i < cv.time_count; i += 7u) {
        f32 t = de_cooked_clip_time(&cv, i);
        ASSERT_TRUE(de__anim_clip_sample(&cv, &sk, t, dec));
        source_pose(&f->ir, &sk, node_of_joint, &f->ir.anims[0], (f64)t, src, tr);
        for (u32 j = 0; j < sk.joint_count; ++j) {
            f64 l[12];
            trs_affine(tr + 10u * j, tr + 10u * j + 3, tr + 10u * j + 7, l);
            u16 p = de_cooked_skeleton_parent(&sk, j);
            if (p == DE_COOKED_SKEL_NO_PARENT)
                memcpy(msrc + 12u * j, l, sizeof l);
            else
                aff_mul(msrc + 12u * p, l, msrc + 12u * j);
        }
        model_space(&sk, dec, md);
        for (u32 j = 0; j < sk.joint_count; ++j) {
            max_pos = fmax(max_pos, pos_diff(md + 12u * j, msrc + 12u * j));
            max_ang = fmax(max_ang, orient_diff(md + 12u * j, msrc + 12u * j));
        }
    }
    printf("    independent check: %.3g mm, %.4g deg\n", max_pos * 1000.0, max_ang / DEG);
    EXPECT_FLE(max_pos, 1e-4 + 1e-7);
    EXPECT_FLE(max_ang, 0.05 * DEG + 1e-7);
}

/* The issue's case is 5 minutes; Debug builds (the sanitizer jobs) cook 1 minute. */
#if defined(DE_DEBUG)
#define LONG_CLIP_SECONDS 60.0
#else
#define LONG_CLIP_SECONDS 300.0
#endif

TEST_F(clip, long_clip, anim_fx) {
    /* DENG-399: a 5-minute take at 30 Hz on the CesiumMan skeleton, its 2 s walk (24 Hz keys)
     * played forward and backward. */
    de_model_clip_stats st;
    memset(&st, 0, sizeof st);
    long_clip(f, LONG_CLIP_SECONDS, 1.0, &st);
    EXPECT_LE(st.iterations, 3u);
}

TEST_F(clip, long_smooth_clip, anim_fx) {
    /* The same walk slowed down 150 times: one 5-minute take with a key every 6.25 s. Linear
     * keys that far apart make long segments (up to MAX_SEGMENT samples), the case that cost
     * O(samples x 256) evaluations per track before DENG-399. */
    de_model_clip_stats st;
    memset(&st, 0, sizeof st);
    long_clip(f, LONG_CLIP_SECONDS, 150.0, &st);
    EXPECT_LE(st.iterations, 3u);
}
