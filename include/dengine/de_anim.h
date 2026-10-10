/* de_anim.h — skeletal animation, blend trees, state machines, IK, 2D sprite animation. SPEC.md §21.5. */
#ifndef DE_ANIM_H
#define DE_ANIM_H
#include "de_base.h"
#include "de_math.h"
#include "de_ecs.h"
#include "de_asset.h"
DE_EXTERN_C_BEGIN

typedef struct de_anim_system de_anim_system;
DE_HANDLE(de_anim_graph);     /* asset: blend trees + state machines, authored as data */

DE_API de_result de_anim_create(const de_allocator *a, de_asset_system *assets, de_anim_system **out);
DE_API void      de_anim_destroy(de_anim_system *s);
/* Registers components and systems: sample → blend → IK → pose → GPU skinning upload. */
DE_API void      de_anim_attach(de_anim_system *s, de_world *w);

/* ---------- Parameters (drive transitions and blend weights) ---------- */
DE_API void de_anim_set_float(de_world *w, de_entity e, de_name param, f32 v);
DE_API void de_anim_set_bool (de_world *w, de_entity e, de_name param, bool v);
DE_API void de_anim_trigger  (de_world *w, de_entity e, de_name trigger);
DE_API de_name de_anim_current_state(de_world *w, de_entity e, u32 layer);

/* ---------- Components ---------- */
typedef struct DeAnimator {
    de_skeleton   skeleton;
    de_anim_graph graph;          /* zero = single clip playback */
    de_anim_clip  clip;           /* single clip mode */
    f32  speed, time;
    bool playing, loop;
    bool root_motion;             /* apply root motion to the entity transform */
    bool deterministic;           /* sample in FixedUpdate with de_dmath */
} DeAnimator;

typedef enum de_ik_kind { DE_IK_TWO_BONE, DE_IK_FABRIK, DE_IK_LOOK_AT, DE_IK_FOOT_GROUND } de_ik_kind;
typedef struct de_ik_chain {
    de_ik_kind kind;
    de_name    root_bone, tip_bone;
    de_entity  target;            /* or target_position if null */
    de_dvec3   target_position;
    de_vec3    pole;
    f32        weight;
    u32        iterations;        /* FABRIK */
} de_ik_chain;

typedef struct DeIK { de_ik_chain chains[8]; u32 chain_count; } DeIK;

/* Read-only output pose (model space), for gameplay queries such as weapon sockets. */
DE_API bool de_anim_bone_transform(de_world *w, de_entity e, de_name bone, de_transform *out_world);

/* ---------- 2D sprite animation (from Aseprite tags or sprite sheets) ---------- */
typedef struct DeSpriteAnimator {
    de_anim_clip clip;            /* frame list + durations */
    de_name      tag;             /* Aseprite tag; zero = whole clip */
    f32  speed, time;
    bool playing, loop, ping_pong;
} DeSpriteAnimator;

typedef enum de_anim_event_kind { DE_ANIM_EV_CLIP_END, DE_ANIM_EV_MARKER, DE_ANIM_EV_STATE_ENTER, DE_ANIM_EV_STATE_EXIT } de_anim_event_kind;
typedef struct de_anim_event { de_anim_event_kind kind; de_entity entity; de_name name; } de_anim_event;

DE_EXTERN_C_END
#endif
