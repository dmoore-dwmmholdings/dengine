#ifndef DE_ANIM_GRAPH_INTERNAL_H
#define DE_ANIM_GRAPH_INTERNAL_H
#include "anim/anim_internal.h"
#include "anim/blend_tree.h"
typedef struct de__anim_graph {
    struct de__anim_graph *next;
    de_anim_graph handle;
    size_t bytes;
    u64 definition_hash;
    de_anim_graph_desc desc;
    de_anim_param_desc *params;
    de_anim_node_desc *nodes;
    de_anim_layer_desc *layers;
    de__anim_blend_space **spaces;
    de_load_handle *references;
    u32 reference_count;
    de_name *sync_groups;
    u32 *node_sync_groups;
    u32 sync_count;
    de_component sync_component;
    struct de__anim_reference_owner *reference_owners;
} de__anim_graph;
typedef struct DeAnimParams {
    u32 graph, count;
    de_name names[DE_ANIM_MAX_PARAMS];
    u32 kinds[DE_ANIM_MAX_PARAMS];
    f32 values[DE_ANIM_MAX_PARAMS];
    u32 bools, triggers;
    bool warned_unknown;
} DeAnimParams;
void de__anim_params_defaults(DeAnimParams *, const de__anim_graph *);
de_result de_anim__graph_init(de_anim_system *, de_world *);
de__anim_graph *de__anim_graph_get(const de_anim_system *, de_anim_graph);
void de__anim_graphs_shutdown(de_anim_system *);
bool de__anim_graph_sync_prepare(de_anim_system *, de_entity);
u64 de__anim_graph_sync_hash(const de_anim_system *, de_entity, const de__anim_graph *, u64);
i32 de__anim_graph_param(const de__anim_graph *, de_name, de_anim_param_kind);
typedef struct de__anim_reference_stats {
    u64 entries, users, owners, builds;
} de__anim_reference_stats;
de__anim_reference_stats de__anim_graph_reference_stats(const de_anim_system *);
void de__anim_graph_reference_owners_release(de_anim_system *, de__anim_graph *);
#endif
