/* DENG-743: CPU-only contract of the actual service's shared name/parser table.
 * No host, window, renderer or GPU is created by these tests. */
#include "de_test.h"

#include "host/editor/editor_view_modes.h"

TEST(editor_view_modes, implemented_modes) {
    const struct {
        const char *name;
        de_view_render_mode mode;
    } expected[] = {
        {"lit", DE_VIEW_RENDER_LIT},
        {"unlit", DE_VIEW_RENDER_UNLIT},
        {"baseColor", DE_VIEW_RENDER_BASE_COLOR},
        {"normals", DE_VIEW_RENDER_NORMALS},
        {"roughness", DE_VIEW_RENDER_ROUGHNESS},
        {"metallic", DE_VIEW_RENDER_METALLIC},
        {"ao", DE_VIEW_RENDER_AO},
        {"emissive", DE_VIEW_RENDER_EMISSIVE},
        {"motionVectors", DE_VIEW_RENDER_MOTION},
        {"depth", DE_VIEW_RENDER_DEPTH},
        {"wireframe", DE_VIEW_RENDER_WIREFRAME},
        {"meshletIds", DE_VIEW_RENDER_MESHLETS},
        {"uv0", DE_VIEW_RENDER_UV0},
        {"uv1", DE_VIEW_RENDER_UV1},
    };
    EXPECT_EQ(sizeof expected / sizeof expected[0], (size_t)DE_VIEW_RENDER_MODE_COUNT);
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; ++i) {
        de_view_render_mode mode = DE_VIEW_RENDER_MODE_COUNT;
        EXPECT_STREQ(de__ed_view_render_names[i], expected[i].name);
        ASSERT_TRUE(
            de__ed_view_render_mode_parse(expected[i].name, strlen(expected[i].name), &mode));
        EXPECT_EQ(mode, expected[i].mode);
    }
}

TEST(editor_view_modes, default_lit) {
    de_view_render_mode mode = DE_VIEW_RENDER_NORMALS;
    EXPECT_TRUE(de__ed_view_render_mode_parse(NULL, 0, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_LIT);
    mode = DE_VIEW_RENDER_NORMALS;
    EXPECT_TRUE(de__ed_view_render_mode_parse("", 0, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_LIT);
    EXPECT_FALSE(de__ed_view_render_mode_parse("lit", 3, NULL));
}

TEST(editor_view_modes, unavailable_names_preserve_mode) {
    const char *const invalid[] = {"litWireframe", "uv2",       "uv3",
                                   "Wireframe",    "wirefram",  "wireframe.extra",
                                   "baseColor ",   "not-a-mode"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        de_view_render_mode mode = DE_VIEW_RENDER_NORMALS;
        EXPECT_FALSE(de__ed_view_render_mode_parse(invalid[i], strlen(invalid[i]), &mode));
        EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
    }
    de_view_render_mode mode = DE_VIEW_RENDER_NORMALS;
    EXPECT_FALSE(de__ed_view_render_mode_parse(NULL, 9, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
}

TEST(editor_view_modes, exact_nonterminated_spans) {
    const char raw[] = {'w', 'i', 'r', 'e', 'f', 'r', 'a', 'm', 'e'};
    de_view_render_mode mode = DE_VIEW_RENDER_NORMALS;
    ASSERT_TRUE(de__ed_view_render_mode_parse(raw, sizeof raw, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_WIREFRAME);
    EXPECT_TRUE(de__ed_view_render_mode_parse("wireframe.trailing", 9, &mode));
    mode = DE_VIEW_RENDER_NORMALS;
    EXPECT_FALSE(de__ed_view_render_mode_parse("wireframe\0trailing", 18, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
    const char binary[] = {'w', 'i', 'r', 'e', '\0', 'r', 'a', 'm', 'e'};
    EXPECT_FALSE(de__ed_view_render_mode_parse(binary, sizeof binary, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
}

TEST(editor_view_modes, game_views_keep_canonical_lit_output) {
    for (u32 i = 0; i < DE_VIEW_RENDER_MODE_COUNT; ++i) {
        const char *name = de__ed_view_render_names[i];
        de_view_render_mode mode = DE_VIEW_RENDER_MODE_COUNT;
        ASSERT_TRUE(de__ed_view_render_mode_request(name, strlen(name), false, &mode));
        EXPECT_EQ(mode, (de_view_render_mode)i);
        mode = DE_VIEW_RENDER_MODE_COUNT;
        if (i == DE_VIEW_RENDER_LIT) {
            ASSERT_TRUE(de__ed_view_render_mode_request(name, strlen(name), true, &mode));
            EXPECT_EQ(mode, DE_VIEW_RENDER_LIT);
        } else {
            EXPECT_FALSE(de__ed_view_render_mode_request(name, strlen(name), true, &mode));
            EXPECT_EQ(mode, DE_VIEW_RENDER_MODE_COUNT);
        }
    }
}

TEST(editor_view_modes, view_requests_preserve_span_and_default_contracts) {
    const char wire[] = {'w', 'i', 'r', 'e', 'f', 'r', 'a', 'm', 'e'};
    const char binary[] = {'l', 'i', 't', '\0'};
    for (u32 game = 0; game < 2; ++game) {
        de_view_render_mode mode = DE_VIEW_RENDER_NORMALS;
        ASSERT_TRUE(de__ed_view_render_mode_request(NULL, 0, game != 0, &mode));
        EXPECT_EQ(mode, DE_VIEW_RENDER_LIT);
        mode = DE_VIEW_RENDER_NORMALS;
        EXPECT_FALSE(de__ed_view_render_mode_request("litWireframe", 12, game != 0, &mode));
        EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
        EXPECT_FALSE(de__ed_view_render_mode_request(binary, sizeof binary, game != 0, &mode));
        EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
        EXPECT_FALSE(de__ed_view_render_mode_request(NULL, 1, game != 0, &mode));
        EXPECT_EQ(mode, DE_VIEW_RENDER_NORMALS);
        EXPECT_FALSE(de__ed_view_render_mode_request("lit", 3, game != 0, NULL));
    }
    de_view_render_mode mode = DE_VIEW_RENDER_NORMALS;
    ASSERT_TRUE(de__ed_view_render_mode_request(wire, sizeof wire, false, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_WIREFRAME);
    EXPECT_FALSE(de__ed_view_render_mode_request(wire, sizeof wire, true, &mode));
    EXPECT_EQ(mode, DE_VIEW_RENDER_WIREFRAME);
}
