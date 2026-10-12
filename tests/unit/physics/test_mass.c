/* test_mass.c: mass properties of every shape kind (DENG-132): analytic primitives, hulls by
 * tetrahedra, compounds by the parallel axis theorem. */
#include "de_test.h"

#include <dengine/de_physics.h>

#include "physics/physics_internal.h"
#include "physics/shapes/shape_internal.h"

#include <string.h>

#define PI_D 3.14159265358979323846

typedef struct pm_fx {
    de_physics_world *p;
} pm_fx;

static void pm_fx_setup(pm_fx *f) {
    ASSERT_EQ(de_physics_create(NULL, &f->p), DE_OK);
}

static void pm_fx_teardown(pm_fx *f) {
    de_physics_destroy(f->p);
}

static de_shape mk(de_physics_world *p, de_shape_kind k, f32 r, f32 h, de_vec3 e) {
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = k;
    d.radius = r;
    d.half_height = h;
    d.half_extents = e;
    const de_shape s = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(s));
    return s;
}

static de_mass_props props(de_physics_world *p, de_shape s, f32 density) {
    de_mass_props m;
    ASSERT_EQ(de_shape_get_mass_props(p, s, density, &m), DE_OK);
    return m;
}

/* |got - want| <= rel * |want| (or <= rel * scale when want is 0). */
static void expect_rel(f64 got, f64 want, f64 rel, f64 scale) {
    const f64 ref = want != 0.0 ? (want < 0.0 ? -want : want) : scale;
    const f64 diff = got > want ? got - want : want - got;
    EXPECT_TRUE(diff <= rel * ref);
}

static void expect_props(const de_mass_props *m, f64 mass, de_dvec3 c, f64 ixx, f64 iyy, f64 izz,
                         f64 rel) {
    expect_rel(m->mass, mass, rel, 1.0);
    const f64 scale = ixx > iyy ? (ixx > izz ? ixx : izz) : (iyy > izz ? iyy : izz);
    expect_rel(m->center.x, c.x, rel, 1.0);
    expect_rel(m->center.y, c.y, rel, 1.0);
    expect_rel(m->center.z, c.z, rel, 1.0);
    expect_rel(m->inertia.m[0], ixx, rel, scale);
    expect_rel(m->inertia.m[4], iyy, rel, scale);
    expect_rel(m->inertia.m[8], izz, rel, scale);
    const u32 off[6] = {1, 2, 3, 5, 6, 7};
    for (u32 k = 0; k < 6; ++k)
        expect_rel(m->inertia.m[off[k]], 0.0, rel, scale);
}

TEST_F(physics_mass, analytic_primitives, pm_fx) {
    const de_dvec3 o = {0, 0, 0};
    const f64 rho = 1000.0;
    {
        const f64 r = 0.6, m = rho * 4.0 / 3.0 * PI_D * r * r * r, i = 0.4 * m * r * r;
        const de_mass_props p = props(f->p, mk(f->p, DE_SHAPE_SPHERE, 0.6f, 0, (de_vec3){0}), 0);
        expect_props(&p, m, o, i, i, i, 1e-4);
    }
    {
        const f64 m = rho * 8.0 * 0.5 * 1.0 * 1.5;
        const de_mass_props p =
            props(f->p, mk(f->p, DE_SHAPE_BOX, 0, 0, (de_vec3){0.5f, 1.0f, 1.5f}), 1000.0f);
        expect_props(&p, m, o, m / 3.0 * (1.0 + 2.25), m / 3.0 * (0.25 + 2.25),
                     m / 3.0 * (0.25 + 1.0), 1e-4);
    }
    {
        const f64 r = 0.4, h = 0.75, m = rho * PI_D * r * r * 2.0 * h;
        const f64 ixx = m * (3.0 * r * r + 4.0 * h * h) / 12.0;
        const de_mass_props p =
            props(f->p, mk(f->p, DE_SHAPE_CYLINDER, 0.4f, 0.75f, (de_vec3){0}), 1000.0f);
        expect_props(&p, m, o, ixx, ixx, m * r * r * 0.5, 1e-4);
    }
    {
        /* Capsule: a cylinder of height 2h plus a sphere split in two hemispheres whose
         * centers of mass sit 3r/8 beyond the cylinder ends. */
        const f64 r = 0.3, h = 0.8;
        const f64 mc = rho * PI_D * r * r * 2.0 * h, ms = rho * 4.0 / 3.0 * PI_D * r * r * r;
        const f64 izz = mc * r * r / 2.0 + ms * 2.0 / 5.0 * r * r;
        /* Hemisphere about its own center of mass: 83/320 m r^2 (transverse axis). */
        const f64 mh = ms / 2.0, dz = h + 3.0 * r / 8.0;
        const f64 ixx = mc * ((2.0 * h) * (2.0 * h) / 12.0 + r * r / 4.0) +
                        2.0 * (83.0 / 320.0 * mh * r * r + mh * dz * dz);
        const de_mass_props p =
            props(f->p, mk(f->p, DE_SHAPE_CAPSULE, 0.3f, 0.8f, (de_vec3){0}), 1000.0f);
        expect_props(&p, mc + ms, o, ixx, ixx, izz, 1e-4);
        /* A capsule of zero height is a sphere. */
        const de_mass_props ps =
            props(f->p, mk(f->p, DE_SHAPE_CAPSULE, 0.3f, 0.0f, (de_vec3){0}), 1000.0f);
        const f64 i = 0.4 * ms * r * r;
        expect_props(&ps, ms, o, i, i, i, 1e-4);
    }
    /* Density scales mass and inertia, not the center. */
    const de_shape b = mk(f->p, DE_SHAPE_BOX, 0, 0, (de_vec3){1, 1, 1});
    const de_mass_props a1 = props(f->p, b, 1.0f), a2 = props(f->p, b, 2.0f);
    EXPECT_FLOAT_EQ(a2.mass, 2.0f * a1.mass, 1e-6f);
    EXPECT_FLOAT_EQ(a2.inertia.m[0], 2.0f * a1.inertia.m[0], 1e-5f);
    de_mass_props bad;
    EXPECT_EQ(de_shape_get_mass_props(f->p, b, -1.0f, &bad), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(de_shape_get_mass_props(f->p, b, 1.0f, NULL), DE_ERR_INVALID_ARGUMENT);
}

TEST_F(physics_mass, hull_of_box_corners_equals_box, pm_fx) {
    const de_vec3 e = {0.5f, 1.0f, 1.5f};
    const de_vec3 offsets[3] = {{0, 0, 0}, {3, -2, 1}, {-20.5f, 7.25f, 100}};
    const de_mass_props box = props(f->p, mk(f->p, DE_SHAPE_BOX, 0, 0, e), 1000.0f);
    for (u32 k = 0; k < 3; ++k) {
        de_vec3 pts[8];
        for (u32 c = 0; c < 8; ++c)
            pts[c] = (de_vec3){offsets[k].x + ((c & 1) ? e.x : -e.x),
                               offsets[k].y + ((c & 2) ? e.y : -e.y),
                               offsets[k].z + ((c & 4) ? e.z : -e.z)};
        de_shape_desc d;
        memset(&d, 0, sizeof d);
        d.kind = DE_SHAPE_CONVEX;
        d.points = pts;
        d.point_count = 8;
        const de_shape h = de_shape_create(f->p, &d);
        ASSERT_TRUE(DE_HANDLE_VALID(h));
        const de_mass_props m = props(f->p, h, 1000.0f);
        expect_props(&m, box.mass, de__d3_from(offsets[k]), box.inertia.m[0], box.inertia.m[4],
                     box.inertia.m[8], 1e-4);
    }
}

TEST_F(physics_mass, hull_prism_approximates_cylinder, pm_fx) {
    enum { N = 128 };
    de_vec3 pts[2 * N];
    for (u32 i = 0; i < N; ++i) {
        const f32 a = (f32)i * (DE_TAU / (f32)N);
        const f32 x = 0.5f * de_dcos(a), y = 0.5f * de_dsin(a);
        pts[2 * i] = (de_vec3){x, y, -1.0f};
        pts[2 * i + 1] = (de_vec3){x, y, 1.0f};
    }
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_CONVEX;
    d.points = pts;
    d.point_count = 2 * N;
    const de_shape h = de_shape_create(f->p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(h));
    const de__hull *hull = &de__shape_get(f->p, h)->u.hull;
    EXPECT_EQ(hull->vertex_count, 2u * N);
    EXPECT_EQ(hull->face_count, (u32)N + 2u); /* sides + two merged caps */
    const de_mass_props m = props(f->p, h, 1000.0f);
    const de_mass_props c =
        props(f->p, mk(f->p, DE_SHAPE_CYLINDER, 0.5f, 1.0f, (de_vec3){0}), 1000.0f);
    expect_props(&m, c.mass, (de_dvec3){0, 0, 0}, c.inertia.m[0], c.inertia.m[4], c.inertia.m[8],
                 2e-3);
}

TEST_F(physics_mass, compound_parallel_axis, pm_fx) {
    de_physics_world *p = f->p;
    const de_shape cube = mk(p, DE_SHAPE_BOX, 0, 0, (de_vec3){0.5f, 0.5f, 0.5f});
    const de_mass_props one = props(p, cube, 1000.0f);
    const f64 m = one.mass, i0 = one.inertia.m[0];

    /* Two cubes at x = +-2: COM at 0, Iyy = Izz = 2 (I0 + m d^2). */
    de_shape kids[2] = {cube, cube};
    de_transform xf[2];
    memset(xf, 0, sizeof xf);
    xf[0].position = (de_dvec3){-2, 0, 0};
    xf[1].position = (de_dvec3){2, 0, 0};
    xf[0].rotation = xf[1].rotation = (de_quat){0, 0, 0, 1};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = kids;
    d.child_xforms = xf;
    d.child_count = 2;
    const de_mass_props two = props(p, de_shape_create(p, &d), 1000.0f);
    expect_props(&two, 2.0 * m, (de_dvec3){0, 0, 0}, 2.0 * i0, 2.0 * (i0 + m * 4.0),
                 2.0 * (i0 + m * 4.0), 1e-4);

    /* One sphere off the origin: COM moves, inertia about the COM is the sphere's. */
    const de_shape sph = mk(p, DE_SHAPE_SPHERE, 0.75f, 0, (de_vec3){0});
    const de_mass_props ms = props(p, sph, 1000.0f);
    kids[0] = sph;
    xf[0].position = (de_dvec3){1, 2, 3};
    d.child_count = 1;
    const de_mass_props off = props(p, de_shape_create(p, &d), 1000.0f);
    expect_props(&off, ms.mass, (de_dvec3){1, 2, 3}, ms.inertia.m[0], ms.inertia.m[0],
                 ms.inertia.m[0], 1e-4);

    /* A box rotated 90 deg about Z is the box with x and y swapped. */
    const de_shape slab = mk(p, DE_SHAPE_BOX, 0, 0, (de_vec3){1.0f, 0.5f, 0.25f});
    const de_mass_props swapped =
        props(p, mk(p, DE_SHAPE_BOX, 0, 0, (de_vec3){0.5f, 1.0f, 0.25f}), 1000.0f);
    kids[0] = slab;
    xf[0].position = (de_dvec3){0, 0, 0};
    xf[0].rotation = (de_quat){0, 0, 0.70710678f, 0.70710678f};
    const de_mass_props rot = props(p, de_shape_create(p, &d), 1000.0f);
    expect_props(&rot, swapped.mass, (de_dvec3){0, 0, 0}, swapped.inertia.m[0],
                 swapped.inertia.m[4], swapped.inertia.m[8], 1e-4);

    /* Unequal children: COM at the mass-weighted mean. */
    kids[0] = cube;
    kids[1] = sph;
    xf[0].position = (de_dvec3){0, 0, 0};
    xf[0].rotation = (de_quat){0, 0, 0, 1};
    xf[1].position = (de_dvec3){3, 0, 0};
    d.child_count = 2;
    const de_mass_props mix = props(p, de_shape_create(p, &d), 1000.0f);
    expect_rel(mix.mass, m + ms.mass, 1e-5, 1.0);
    expect_rel(mix.center.x, 3.0 * ms.mass / (m + ms.mass), 1e-5, 1.0);
}

TEST_F(physics_mass, static_only_kinds_and_bodies, pm_fx) {
    de_physics_world *p = f->p;
    const de_vec3 v[4] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    const u32 idx[6] = {0, 1, 3, 0, 3, 2};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_MESH;
    d.vertices = v;
    d.vertex_count = 4;
    d.indices = idx;
    d.index_count = 6;
    const de_shape mesh = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(mesh));
    EXPECT_TRUE(props(p, mesh, 1000.0f).mass == 0.0f);
    const f32 hts[4] = {0, 1, 0, 1};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_HEIGHTFIELD;
    d.heights = hts;
    d.rows = d.cols = 2;
    d.scale = (de_vec3){1, 1, 1};
    const de_shape hf = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(hf));
    EXPECT_TRUE(props(p, hf, 1000.0f).mass == 0.0f);
    const de_shape cube = mk(p, DE_SHAPE_BOX, 0, 0, (de_vec3){1, 1, 1});
    const de_shape kids[2] = {cube, mesh};
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = kids;
    d.child_count = 2;
    const de_shape comp = de_shape_create(p, &d);
    ASSERT_TRUE(DE_HANDLE_VALID(comp));
    EXPECT_FALSE(de__shape_get(p, comp)->has_volume);
    EXPECT_TRUE(props(p, comp, 1000.0f).mass == 0.0f);
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_DYNAMIC;
    const de_shape refused[3] = {mesh, hf, comp};
    for (u32 i = 0; i < 3; ++i) {
        bd.shape = refused[i];
        EXPECT_FALSE(DE_HANDLE_VALID(de_body_create(p, &bd)));
    }
}

TEST_F(physics_mass, body_inverse_inertia, pm_fx) {
    de_physics_world *p = f->p;
    /* A compound with a non-diagonal inertia tensor (a rotated slab off the axis). */
    const de_shape slab = mk(p, DE_SHAPE_BOX, 0, 0, (de_vec3){1.0f, 0.3f, 0.1f});
    const de_shape sph = mk(p, DE_SHAPE_SPHERE, 0.4f, 0, (de_vec3){0});
    const de_shape kids[2] = {slab, sph};
    de_transform xf[2];
    memset(xf, 0, sizeof xf);
    xf[0].rotation = de_dnormalize_quat((de_quat){0.3f, -0.2f, 0.5f, 0.8f});
    xf[1].position = (de_dvec3){0.5, 0.7, -0.2};
    xf[1].rotation = (de_quat){0, 0, 0, 1};
    de_shape_desc d;
    memset(&d, 0, sizeof d);
    d.kind = DE_SHAPE_COMPOUND;
    d.children = kids;
    d.child_xforms = xf;
    d.child_count = 2;
    const de_shape comp = de_shape_create(p, &d);
    de_body_desc bd;
    memset(&bd, 0, sizeof bd);
    bd.type = DE_BODY_DYNAMIC;
    bd.shape = comp;
    const de_body b = de_body_create(p, &bd);
    ASSERT_TRUE(DE_HANDLE_VALID(b));
    const u32 i = de__handle_index(b.id);
    const de_mat3 *I = &p->bodies.local_inertia[i];
    const de_mat3 *J = &p->bodies.local_inv_inertia[i];
    EXPECT_TRUE(de__absf(I->m[1]) > 1e-3f); /* really non-diagonal */
    for (u32 col = 0; col < 3; ++col)
        for (u32 row = 0; row < 3; ++row) {
            f64 s = 0.0;
            for (u32 k = 0; k < 3; ++k)
                s += (f64)I->m[k * 3 + row] * (f64)J->m[col * 3 + k];
            EXPECT_FLOAT_EQ(s, row == col ? 1.0 : 0.0, 1e-5);
        }
    /* Symmetric. */
    EXPECT_FLOAT_EQ(I->m[1], I->m[3], 1e-6f);
    EXPECT_FLOAT_EQ(I->m[2], I->m[6], 1e-6f);
    EXPECT_FLOAT_EQ(I->m[5], I->m[7], 1e-6f);
    /* The body's center of mass is the shape's. */
    de_mass_props sm;
    ASSERT_EQ(de_shape_get_mass_props(p, comp, 0.0f, &sm), DE_OK);
    EXPECT_TRUE(memcmp(&sm.center, &p->bodies.local_com[i], sizeof sm.center) == 0);
    EXPECT_FLOAT_EQ(p->bodies.inv_mass[i] * sm.mass, 1.0f, 1e-6f);
    /* A singular tensor inverts to zero. */
    const f64 zero[9] = {0}, *zp = zero;
    f64 inv[9];
    de__mat3_inverse_d(zp, inv);
    for (u32 k = 0; k < 9; ++k)
        EXPECT_TRUE(inv[k] == 0.0);
}
