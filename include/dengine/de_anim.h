/* de_anim.h — skeletal animation, blend trees, state machines, IK, 2D sprite animation. SPEC.md
 * §21.5. */
#ifndef DE_ANIM_H
#define DE_ANIM_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

typedef struct de_anim_system de_anim_system;
DE_HANDLE(de_anim_graph); /* graph asset, or transient programmatic definition below */

DE_API de_result de_anim_create(const de_allocator *a, de_asset_system *assets,
                                de_anim_system **out);
DE_API void de_anim_destroy(de_anim_system *s);
/** Registers variable-time evaluation in PostUpdate before transform propagation,
 * then model-pose finalization and skinning upload after propagation. The internal
 * pipeline reserves ordered sample, blend, root-motion, IK and finalize stages.
 * Deterministic animators evaluate on fixed simulation ticks, including model
 * poses for gameplay queries; PostUpdate publishes their last fixed palettes.
 * Deterministic mode excludes IK in v1.
 *
 * Main thread, between schedules. One animation system may own a world; attaching
 * the same system to that world again is a no-op. On failure an initially
 * unattached system stays unattached, de_anim_component() is invalid and the error
 * context describes the failure. Retry is allowed; engine type/component/event
 * definitions may remain registered, but no partial systems or resource remain. */
DE_API void de_anim_attach(de_anim_system *s, de_world *w);

/* ---------- Programmatic graph definitions ---------- */
#define DE_ANIM_MAX_PARAMS 32u
typedef enum de_anim_param_kind {
    DE_ANIM_PARAM_FLOAT,
    DE_ANIM_PARAM_BOOL,
    DE_ANIM_PARAM_TRIGGER
} de_anim_param_kind;
typedef struct de_anim_param_desc {
    de_name name;
    de_anim_param_kind kind;
    f32 float_default;
    bool bool_default;
} de_anim_param_desc;
typedef enum de_anim_node_kind {
    DE_ANIM_NODE_CLIP,
    DE_ANIM_NODE_BLEND1D,
    DE_ANIM_NODE_BLEND2D,
    DE_ANIM_NODE_ADDITIVE,
    DE_ANIM_NODE_LAYER,
    DE_ANIM_NODE_STATE_MACHINE /* reserved; rejected until DENG-184 */
} de_anim_node_kind;
typedef struct de_anim_blend_point_desc {
    de_vec2 position;
    u32 node;
} de_anim_blend_point_desc;
typedef struct de_anim_bone_mask_desc {
    de_name bone;
    f32 weight;
    bool include_children;
} de_anim_bone_mask_desc;
typedef enum de_anim_layer_mode {
    DE_ANIM_LAYER_OVERRIDE,
    DE_ANIM_LAYER_ADDITIVE
} de_anim_layer_mode;
typedef struct de_anim_layer_desc {
    u32 node;
    de_anim_layer_mode mode;
    f32 weight, root_weight; /* [0,1]; root slot is independently masked */
    de_name weight_param;    /* optional float; zero uses weight */
    const de_anim_bone_mask_desc *mask;
    u32 mask_count;         /* zero means all bones */
    de_anim_clip reference; /* additive reference; zero means input clip frame0 */
} de_anim_layer_desc;
typedef struct de_anim_node_desc {
    de_anim_node_kind kind;
    union {
        struct {
            de_anim_clip clip;
            f32 speed; /* finite; set 1 for normal playback */
            bool loop;
            de_name sync_group;
        } clip;
        struct {
            de_name x_param, y_param;
            const de_anim_blend_point_desc *points;
            u32 point_count;
        } blend;
        struct {
            u32 base, input; /* non-CLIP input requires a reference clip */
            f32 weight;
            de_name weight_param;
            de_anim_clip reference;
        } additive;
        struct {
            u32 base, first_layer, layer_count;
        } layer;
    } data;
} de_anim_node_desc;
typedef struct de_anim_graph_desc {
    const de_anim_param_desc *params;
    u32 param_count;
    const de_anim_node_desc *nodes;
    u32 node_count, output_node;
    const de_anim_layer_desc *layers;
    u32 layer_count;
} de_anim_graph_desc;
/** Copies a validated graph, including nested points/masks, and retains all clip
 * assets. Nodes are in evaluation order: every child must precede its parent.
 * Limits: 32 parameters, 1024 nodes, 256 layers, 65535 total points/mask entries.
 * Clip assets may be loading, but must be declared skeletal anim_clip assets.
 * Programmatic handles belong to s and are transient, not serializable assets;
 * they cannot collide with asset-slot handles. Main thread between schedules.
 * Caller-owned descriptor memory can be released after return. */
DE_API de_result de_anim_graph_create(de_anim_system *s, const de_anim_graph_desc *desc,
                                      de_anim_graph *out);
/** Invalidates this programmatic graph. Foreign/stale handles are ignored.
 * Main thread between schedules. Destroying s also destroys its graphs. */
DE_API void de_anim_graph_destroy(de_anim_system *s, de_anim_graph graph);

/* ---------- Parameters (drive transitions and blend weights) ---------- */
/** Bind outside schedules, creating snapshot parameter storage and applying
 * defaults. Rebinding the same graph preserves values; graph zero unbinds.
 * Setters never add components: systems must declare write access to the
 * component returned by de_anim_params_component(). Unknown/wrong-type setter
 * names warn once per binding. Nonfinite float writes are rejected. */
DE_API de_result de_anim_graph_bind(de_world *w, de_entity e, de_anim_graph graph);
DE_API de_component de_anim_params_component(const de_anim_system *s);
DE_API void de_anim_set_float(de_world *w, de_entity e, de_name param, f32 v);
DE_API void de_anim_set_bool(de_world *w, de_entity e, de_name param, bool v);
DE_API void de_anim_trigger(de_world *w, de_entity e, de_name trigger);
/** Read access to the parameter component is sufficient. Missing/wrong-type
 * parameters return false and leave out untouched. */
DE_API bool de_anim_get_float(de_world *w, de_entity e, de_name param, f32 *out);
DE_API bool de_anim_get_bool(de_world *w, de_entity e, de_name param, bool *out);
/** Write access; returns and clears a latched trigger. */
DE_API bool de_anim_consume_trigger(de_world *w, de_entity e, de_name trigger);
DE_API de_name de_anim_current_state(de_world *w, de_entity e, u32 layer);

/* ---------- Components ---------- */
typedef struct DeAnimator {
    de_skeleton skeleton;
    de_anim_graph graph; /* zero = single clip playback */
    de_anim_clip clip;   /* single clip mode */
    f32 speed, time;
    bool playing, loop;
    bool root_motion;   /* apply root motion to the entity transform */
    bool deterministic; /* sample in FixedUpdate with de_dmath */
} DeAnimator;

typedef enum de_ik_kind {
    DE_IK_TWO_BONE,
    DE_IK_FABRIK,
    DE_IK_LOOK_AT,
    DE_IK_FOOT_GROUND
} de_ik_kind;
typedef struct de_ik_chain {
    de_ik_kind kind;
    de_name root_bone, tip_bone;
    de_entity target; /* or target_position if null */
    de_dvec3 target_position;
    de_vec3 pole;
    f32 weight;
    u32 iterations; /* FABRIK */
} de_ik_chain;

typedef struct DeIK {
    de_ik_chain chains[8];
    u32 chain_count;
} DeIK;

/* Read-only output pose (model space), for gameplay queries such as weapon sockets. */
DE_API bool de_anim_bone_transform(de_world *w, de_entity e, de_name bone, de_transform *out_world);

/* ---------- 2D sprite animation (from Aseprite tags or sprite sheets) ---------- */
typedef struct DeSpriteAnimator {
    de_anim_clip clip; /* frame list + durations */
    de_name tag;       /* Aseprite tag; zero = whole clip */
    f32 speed, time;
    bool playing, loop, ping_pong;
} DeSpriteAnimator;

typedef enum de_anim_event_kind {
    DE_ANIM_EV_CLIP_END,
    DE_ANIM_EV_MARKER,
    DE_ANIM_EV_STATE_ENTER,
    DE_ANIM_EV_STATE_EXIT
} de_anim_event_kind;
typedef struct de_anim_event {
    de_anim_event_kind kind;
    de_entity entity;
    de_name name;
} de_anim_event;

/* S19 visual single-clip runtime bindings. NULL renderer supports headless playback. */
typedef struct de_renderer de_renderer;
typedef struct de_task_system de_task_system;
DE_API void de_anim_set_renderer(de_anim_system *, de_renderer *);
DE_API void de_anim_set_task_system(de_anim_system *, de_task_system *);
DE_API de_component de_anim_component(const de_anim_system *);
DE_API de_event_type de_anim_event_type(const de_anim_system *);
/** XXH3 checksum of deterministic actors, ordered by entity index: integer tick,
 * state identity (reserved for state machines), and local pose bit patterns.
 * Main thread between schedules. Scratch grows on demand. Snapshot restore
 * restores this checksum immediately; cached bone queries resume after the next
 * fixed evaluation if the restored pose differs from the cached pose. */
DE_API u64 de_anim_checksum(de_world *);
DE_API de_skeleton de_anim_skeleton(de_anim_system *, de_asset_id);
DE_API de_anim_clip de_anim_clip_asset(de_anim_system *, de_asset_id);
typedef enum de_anim_clip_kind {
    DE_ANIM_CLIP_NONE,
    DE_ANIM_CLIP_SKELETAL,
    DE_ANIM_CLIP_SPRITE
} de_anim_clip_kind;
/** Clip asset kind (also available before loading); NONE for an invalid/wrong-type handle. */
DE_API de_anim_clip_kind de_anim_clip_kind_get(de_anim_system *, de_anim_clip);

DE_EXTERN_C_END
#endif
