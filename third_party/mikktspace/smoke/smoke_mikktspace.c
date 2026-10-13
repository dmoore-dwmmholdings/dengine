/* Smoke test for de_tp_mikktspace: tangents of one quad in the XY plane with UVs along +X/+Y are
 * +X with handedness +1. The importer tests are in tests/unit/import/. */
#include <math.h>
#include <string.h>

#include <mikktspace.h>

#include "de_test.h"

typedef struct quad {
    float pos[4][3], nrm[4][3], uv[4][2];
    float tan[4][4];
} quad;

static int num_faces(const SMikkTSpaceContext *c) {
    (void)c;
    return 1;
}
static int num_verts(const SMikkTSpaceContext *c, const int face) {
    (void)c;
    (void)face;
    return 4;
}
static void get_pos(const SMikkTSpaceContext *c, float out[], const int face, const int vert) {
    (void)face;
    memcpy(out, ((quad *)c->m_pUserData)->pos[vert], sizeof(float) * 3);
}
static void get_nrm(const SMikkTSpaceContext *c, float out[], const int face, const int vert) {
    (void)face;
    memcpy(out, ((quad *)c->m_pUserData)->nrm[vert], sizeof(float) * 3);
}
static void get_uv(const SMikkTSpaceContext *c, float out[], const int face, const int vert) {
    (void)face;
    memcpy(out, ((quad *)c->m_pUserData)->uv[vert], sizeof(float) * 2);
}
static void set_tan(const SMikkTSpaceContext *c, const float t[], const float sign, const int face,
                    const int vert) {
    (void)face;
    quad *q = (quad *)c->m_pUserData;
    memcpy(q->tan[vert], t, sizeof(float) * 3);
    q->tan[vert][3] = sign;
}

TEST(de_tp_smoke_mikktspace, quad_tangents) {
    quad q = {
        .pos = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}},
        .nrm = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}},
        .uv = {{0, 0}, {1, 0}, {1, 1}, {0, 1}},
    };
    SMikkTSpaceInterface it;
    memset(&it, 0, sizeof it);
    it.m_getNumFaces = num_faces;
    it.m_getNumVerticesOfFace = num_verts;
    it.m_getPosition = get_pos;
    it.m_getNormal = get_nrm;
    it.m_getTexCoord = get_uv;
    it.m_setTSpaceBasic = set_tan;
    SMikkTSpaceContext ctx = {&it, &q};
    ASSERT_TRUE(genTangSpaceDefault(&ctx) != 0);
    for (int v = 0; v < 4; ++v) {
        EXPECT_FLOAT_EQ(q.tan[v][0], 1.0f, 1e-5f);
        EXPECT_FLOAT_EQ(q.tan[v][1], 0.0f, 1e-5f);
        EXPECT_FLOAT_EQ(q.tan[v][2], 0.0f, 1e-5f);
        EXPECT_FLOAT_EQ(q.tan[v][3], 1.0f, 0.0f);
    }
}
