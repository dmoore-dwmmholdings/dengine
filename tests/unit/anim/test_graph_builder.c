#include "anim_fixture.h"
#include "anim/graph_internal.h"
#include "asset/slots_internal.h"

typedef struct graph_description {
    de_anim_param_desc params[3];
    de_anim_node_desc nodes[5];
    de_anim_blend_point_desc points[2];
    de_anim_bone_mask_desc mask[1];
    de_anim_layer_desc layers[1];
    de_anim_graph_desc desc;
} graph_description;
static void description_init(graph_description *d, de_anim_clip clip) {
    memset(d, 0, sizeof *d);
    d->params[0] = (de_anim_param_desc){
        .name = de_name_from("speed"), .kind = DE_ANIM_PARAM_FLOAT, .float_default = 2};
    d->params[1] = (de_anim_param_desc){
        .name = de_name_from("armed"), .kind = DE_ANIM_PARAM_BOOL, .bool_default = true};
    d->params[2] =
        (de_anim_param_desc){.name = de_name_from("fire"), .kind = DE_ANIM_PARAM_TRIGGER};
    d->nodes[0] = (de_anim_node_desc){.kind = DE_ANIM_NODE_CLIP,
                                      .data.clip = {.clip = clip, .speed = 1, .loop = true}};
    d->nodes[1] = d->nodes[0];
    d->nodes[1].data.clip.speed = 2;
    d->points[0] = (de_anim_blend_point_desc){{0, 0}, 0};
    d->points[1] = (de_anim_blend_point_desc){{4, 0}, 1};
    d->nodes[2] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_BLEND1D,
        .data.blend = {.x_param = d->params[0].name, .points = d->points, .point_count = 2}};
    d->nodes[3] = (de_anim_node_desc){.kind = DE_ANIM_NODE_ADDITIVE,
                                      .data.additive = {.base = 2, .input = 1, .weight = .5f}};
    d->mask[0] = (de_anim_bone_mask_desc){de_name_from("joint1"), .75f, true};
    d->layers[0] = (de_anim_layer_desc){.node = 1,
                                        .mode = DE_ANIM_LAYER_OVERRIDE,
                                        .weight = .75f,
                                        .root_weight = 1,
                                        .mask = d->mask,
                                        .mask_count = 1};
    d->nodes[4] = (de_anim_node_desc){
        .kind = DE_ANIM_NODE_LAYER, .data.layer = {.base = 3, .first_layer = 0, .layer_count = 1}};
    d->desc = (de_anim_graph_desc){.params = d->params,
                                   .param_count = 3,
                                   .nodes = d->nodes,
                                   .node_count = 5,
                                   .output_node = 4,
                                   .layers = d->layers,
                                   .layer_count = 1};
}
TEST_F(anim_graph, builder_copies_nested_definitions_and_owns_clip_references, anim_fixture) {
    load_assets(f, 2);
    graph_description d;
    description_init(&d, f->clip);
    de_anim_graph handle = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim, &d.desc, &handle), DE_OK);
    de__anim_graph *g = de__anim_graph_get(f->anim, handle);
    ASSERT_TRUE(g != NULL);
    EXPECT_EQ(g->reference_count, 2u);
    EXPECT_TRUE(de__slot_get(f->assets, handle.id) == NULL);
    d.params[0].name.hash = 0;
    d.points[0].node = UINT32_MAX;
    d.mask[0].weight = 0;
    d.layers[0].weight = 0;
    d.nodes[0].kind = DE_ANIM_NODE_STATE_MACHINE;
    EXPECT_EQ(de__anim_graph_param(g, de_name_from("speed"), DE_ANIM_PARAM_FLOAT), 0);
    EXPECT_TRUE(g->nodes[0].kind == DE_ANIM_NODE_CLIP && g->nodes[0].data.clip.speed == 1);
    EXPECT_TRUE(g->layers[0].weight == .75f && g->layers[0].mask[0].weight == .75f);
    de__anim_blend_weights weights;
    ASSERT_TRUE(de__anim_blend_space_weights(g->spaces[2], (de_vec2){2, 0}, &weights));
    EXPECT_TRUE(weights.count == 2 && weights.node[0] == 0 && weights.node[1] == 1);
    EXPECT_TRUE(weights.weight[0] == .5f && weights.weight[1] == .5f);
    de_anim_graph_destroy(f->anim, handle);
    EXPECT_TRUE(de__anim_graph_get(f->anim, handle) == NULL);
    de_anim_graph_destroy(f->anim, handle);
}
static void rejected(anim_fixture *f, const de_anim_graph_desc *d) {
    de__anim_graph *before = f->anim->graphs;
    de_anim_graph out = {123};
    EXPECT_EQ(de_anim_graph_create(f->anim, d, &out), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(out.id, 0u);
    EXPECT_TRUE(f->anim->graphs == before);
}
TEST_F(anim_graph, builder_rejects_cycles_types_bad_ranges_and_reserved_nodes, anim_fixture) {
    load_assets(f, 2);
    graph_description d;
    description_init(&d, f->clip);
    d.points[0].node = 2; /* self cycle */
    rejected(f, &d.desc);
    d.points[0].node = 4; /* forward/cyclic reference */
    rejected(f, &d.desc);
    d.points[0].node = 0;
    d.nodes[2].data.blend.x_param = d.params[1].name; /* bool used as float */
    rejected(f, &d.desc);
    d.nodes[2].data.blend.x_param = d.params[0].name;
    d.params[1].name = d.params[0].name;
    rejected(f, &d.desc);
    d.params[1].name = de_name_from("armed");
    d.params[0].float_default = NAN;
    rejected(f, &d.desc);
    d.params[0].float_default = 2;
    d.nodes[0].data.clip.speed = INFINITY;
    rejected(f, &d.desc);
    d.nodes[0].data.clip.speed = 1;
    d.nodes[0].data.clip.clip.id = f->skeleton.id;
    rejected(f, &d.desc);
    d.nodes[0].data.clip.clip = f->clip;
    d.nodes[0].kind = DE_ANIM_NODE_STATE_MACHINE;
    rejected(f, &d.desc);
    d.nodes[0].kind = DE_ANIM_NODE_CLIP;
    d.nodes[4].data.layer.first_layer = UINT32_MAX;
    rejected(f, &d.desc);
    d.nodes[4].data.layer.first_layer = 0;
    d.layers[0].node = 4;
    rejected(f, &d.desc);
    d.layers[0].node = 1;
    d.layers[0].root_weight = -1;
    rejected(f, &d.desc);
    d.layers[0].root_weight = 1;
    d.mask[0].weight = NAN;
    rejected(f, &d.desc);
    d.mask[0].weight = .75f;
    d.points[1].position = d.points[0].position; /* nested builder failure */
    rejected(f, &d.desc);
    d.points[1].position.x = 4;
    d.desc.output_node = 5;
    rejected(f, &d.desc);
    d.desc.output_node = 4;
    d.desc.param_count = DE_ANIM_MAX_PARAMS + 1;
    rejected(f, &d.desc);
}
TEST_F(anim_graph, graph_ids_do_not_alias_foreign_or_destroyed_graphs, anim_fixture) {
    load_assets(f, 2);
    graph_description d;
    description_init(&d, f->clip);
    de_anim_system *other = NULL;
    ASSERT_EQ(de_anim_create(&f->allocator, f->assets, &other), DE_OK);
    de_anim_graph a = {0}, b = {0}, replacement = {0};
    ASSERT_EQ(de_anim_graph_create(f->anim, &d.desc, &a), DE_OK);
    ASSERT_EQ(de_anim_graph_create(other, &d.desc, &b), DE_OK);
    EXPECT_TRUE(a.id != b.id);
    EXPECT_TRUE(de__anim_graph_get(other, a) == NULL);
    de_anim_graph_destroy(other, a);
    EXPECT_TRUE(de__anim_graph_get(f->anim, a) != NULL);
    de_anim_graph_destroy(f->anim, a);
    ASSERT_EQ(de_anim_graph_create(f->anim, &d.desc, &replacement), DE_OK);
    EXPECT_TRUE(replacement.id != a.id && de__anim_graph_get(f->anim, a) == NULL);
    de_anim_graph_destroy(f->anim, replacement);
    de_anim_destroy(other); /* also releases its remaining graph */
}
TEST_F(anim_graph, every_owned_allocation_failure_unwinds_and_system_destroy_frees_graphs,
       anim_fixture) {
    load_assets(f, 2);
    graph_description d;
    description_init(&d, f->clip);
    de_tracking_allocator tracking = {0};
    de_allocator allocator =
        de_tracking_allocator_init(&tracking, de_heap_allocator(), DE_MEM_GENERAL);
    de_anim_system *s = NULL;
    ASSERT_EQ(de_anim_create(&allocator, f->assets, &s), DE_OK);
    u64 baseline = de_tracking_allocator_stats(&tracking).bytes_live;
    u32 failures = 0, successes = 0;
    for (i64 i = 0; i < 7; ++i) {
        de_tracking_allocator_fail_after(&tracking, i);
        de_anim_graph g = {0};
        de_result r = de_anim_graph_create(s, &d.desc, &g);
        de_tracking_allocator_fail_after(&tracking, -1);
        if (r == DE_OK) {
            ++successes;
            de_anim_graph_destroy(s, g);
        } else {
            ++failures;
            EXPECT_EQ(r, DE_ERR_OUT_OF_MEMORY);
            EXPECT_EQ(g.id, 0u);
        }
        EXPECT_TRUE(s->graphs == NULL);
        EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, baseline);
    }
    EXPECT_GE(failures, 3u);
    EXPECT_GT(successes, 0u);
    for (u32 i = 0; i < 3; ++i) {
        de_anim_graph g = {0};
        ASSERT_EQ(de_anim_graph_create(s, &d.desc, &g), DE_OK);
    }
    de_anim_destroy(s);
    EXPECT_EQ(de_tracking_allocator_stats(&tracking).bytes_live, 0u);
}
