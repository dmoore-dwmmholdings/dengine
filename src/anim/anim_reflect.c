#include "anim_internal.h"
#include <dengine/de_reflect.h>
#include <string.h>

#define AUTH   DE_FIELD_DEFAULT
#define REF    (DE_FIELD_DEFAULT | DE_FIELD_ASSET_REF)
#define HIDDEN (DE_FIELD_DEFAULT | DE_FIELD_EDITOR_HIDDEN)

static void construct_animator(void *object) {
    DeAnimator *a = object;
    memset(a, 0, sizeof *a);
    a->speed = 1;
    a->playing = a->loop = true;
}
static void construct_sprite(void *object) {
    DeSpriteAnimator *a = object;
    memset(a, 0, sizeof *a);
    a->speed = 1;
    a->playing = a->loop = true;
}
static void construct_chain(void *object) {
    de_ik_chain *c = object;
    memset(c, 0, sizeof *c);
    c->weight = 1;
    c->iterations = 10;
    c->pole.y = 1; /* Engine forward is +Y; +Z is up. */
}
static void construct_ik(void *object) {
    DeIK *ik = object;
    memset(ik, 0, sizeof *ik);
    for (u32 i = 0; i < 8; ++i)
        construct_chain(&ik->chains[i]);
}

/* S19 v1 wrote these as raw u32 handles. Preserve those bits when reading an
 * older record; they cannot be reconstructed into portable asset IDs. New v2
 * records use ASSET fields and the serializer's stable-ID context instead. */
static bool migrate_animator(void *object, u32 from_version, const void *old_data,
                             const de_field_info *fields, u32 count) {
    if (from_version != 1)
        return false;
    for (u32 i = 0; i < count; ++i) {
        const de_field_info *f = &fields[i];
        u32 offset;
        if (strcmp(f->name, "skeleton") == 0)
            offset = (u32)offsetof(DeAnimator, skeleton);
        else if (strcmp(f->name, "graph") == 0)
            offset = (u32)offsetof(DeAnimator, graph);
        else if (strcmp(f->name, "clip") == 0)
            offset = (u32)offsetof(DeAnimator, clip);
        else
            continue;
        u64 value = 0;
        if (f->kind == DE_FK_U32 && f->size == sizeof(u32)) {
            u32 v;
            memcpy(&v, (const u8 *)old_data + f->offset, sizeof v);
            value = v;
        } else if (f->kind == DE_FK_U64 && f->size == sizeof(u64)) {
            memcpy(&value, (const u8 *)old_data + f->offset, sizeof value);
        } else if (f->kind == DE_FK_I64 && f->size == sizeof(i64)) {
            i64 v;
            memcpy(&v, (const u8 *)old_data + f->offset, sizeof v);
            if (v < 0)
                return false;
            value = (u64)v;
        } else
            continue; /* A named asset already decoded through the new context. */
        if (value > UINT32_MAX)
            return false;
        u32 handle = (u32)value;
        memcpy((u8 *)object + offset, &handle, sizeof handle);
    }
    return true;
}

static de_result type(de_type_registry *r, de_type_desc desc, de_type_id *out) {
    de_type_id existing = de_type_find(r, desc.name);
    const de_type_desc *old = de_type_get(r, existing);
    if (existing.id &&
        (!old || old->size != desc.size || old->align != desc.align || old->version > desc.version))
        return DE_FAIL(DE_ERR_ALREADY_EXISTS, "animation reflection conflict: %s", desc.name);
    return de_type_register_ex(r, &desc, out, NULL);
}

de_result de__anim_register_components(de_anim_system *s, de_world *w) {
    de_type_registry *r = de_world_types(w);
    static const de_enum_value kinds[] = {{"DE_IK_TWO_BONE", DE_IK_TWO_BONE},
                                          {"DE_IK_FABRIK", DE_IK_FABRIK},
                                          {"DE_IK_LOOK_AT", DE_IK_LOOK_AT},
                                          {"DE_IK_FOOT_GROUND", DE_IK_FOOT_GROUND}};
    DE_TRY(de_enum_register(r, "de_ik_kind", kinds, 4, false));
    static const de_field_info animator_fields[] = {
        DE_FIELD_EX(DeAnimator, skeleton, DE_FK_ASSET, REF,
                    {0, 0, 0, "Skeleton to animate", "skeleton", 0}),
        DE_FIELD_EX(DeAnimator, graph, DE_FK_ASSET, REF,
                    {0, 0, 0, "Animation graph; zero selects a single clip", "anim_graph", 0}),
        DE_FIELD_EX(DeAnimator, clip, DE_FK_ASSET, REF,
                    {0, 0, 0, "Single playback clip", "anim_clip", 0}),
        DE_FIELD_EX(DeAnimator, speed, DE_FK_F32, AUTH,
                    {0, 0, "x", "Playback multiplier; negative plays in reverse", 0, 0}),
        DE_FIELD_EX(DeAnimator, time, DE_FK_F32, HIDDEN, {0, 0, "s", "Current clip time", 0, 0}),
        DE_FIELD_EX(DeAnimator, playing, DE_FK_BOOL, AUTH,
                    {0, 0, 0, "Advance playback time", 0, 0}),
        DE_FIELD_EX(DeAnimator, loop, DE_FK_BOOL, AUTH, {0, 0, 0, "Wrap at clip boundaries", 0, 0}),
        DE_FIELD_EX(DeAnimator, root_motion, DE_FK_BOOL, AUTH,
                    {0, 0, 0, "Root-motion application requires DENG-186", 0, 0}),
        DE_FIELD_EX(DeAnimator, deterministic, DE_FK_BOOL, AUTH,
                    {0, 0, 0, "FixedUpdate evaluation requires DENG-192", 0, 0})};
    de_type_desc animator = DE_TYPE_DESC(DeAnimator, 2, animator_fields);
    animator.hooks.construct = construct_animator;
    animator.hooks.migrate = migrate_animator;
    de_type_id id;
    DE_TRY(type(r, animator, &id));
    de_component animator_component = de_component_register(w, id, DE_COMP_SERIALIZE);
    if (!animator_component.id)
        return DE_ERR_INTERNAL;

    static const de_field_info chain_fields[] = {
        DE_FIELD_EX(de_ik_chain, kind, DE_FK_ENUM, AUTH, {0, 0, 0, "IK solver", "de_ik_kind", 0}),
        DE_FIELD_EX(de_ik_chain, root_bone, DE_FK_NAME, AUTH,
                    {0, 0, 0, "First joint in the chain", 0, 0}),
        DE_FIELD_EX(de_ik_chain, tip_bone, DE_FK_NAME, AUTH, {0, 0, 0, "End-effector joint", 0, 0}),
        DE_FIELD_EX(de_ik_chain, target, DE_FK_ENTITY, AUTH,
                    {0, 0, 0, "Target entity; zero uses target_position", 0, 0}),
        DE_FIELD_EX(de_ik_chain, target_position, DE_FK_DVEC3, AUTH,
                    {0, 0, "m", "World-space target position", 0, 0}),
        DE_FIELD_EX(de_ik_chain, pole, DE_FK_VEC3, AUTH, {0, 0, 0, "Bend-plane direction", 0, 0}),
        DE_FIELD_EX(de_ik_chain, weight, DE_FK_F32, AUTH,
                    {0, 1, 0, "Blend from the unmodified pose to the IK result", 0, 0}),
        DE_FIELD_EX(de_ik_chain, iterations, DE_FK_U32, AUTH,
                    {1, 1024, 0, "FABRIK iteration budget", 0, 0})};
    de_type_desc chain = DE_TYPE_DESC(de_ik_chain, 1, chain_fields);
    chain.hooks.construct = construct_chain;
    DE_TRY(type(r, chain, &id));
    static const de_field_info ik_fields[] = {
        DE_FIELD_EX(DeIK, chains, DE_FK_FIXED_ARRAY, AUTH,
                    {0, 0, 0, "Chains in evaluation order", "de_ik_chain", 8}),
        DE_FIELD_EX(DeIK, chain_count, DE_FK_U32, AUTH,
                    {0, 8, 0, "Number of active chains", 0, 0})};
    de_type_desc ik = DE_TYPE_DESC(DeIK, 1, ik_fields);
    ik.hooks.construct = construct_ik;
    DE_TRY(type(r, ik, &id));
    de_component ik_component = de_component_register(w, id, DE_COMP_SERIALIZE);
    if (!ik_component.id)
        return DE_ERR_INTERNAL;

    static const de_field_info sprite_fields[] = {
        DE_FIELD_EX(DeSpriteAnimator, clip, DE_FK_ASSET, REF,
                    {0, 0, 0, "Sprite frame-list clip", "anim_clip", 0}),
        DE_FIELD_EX(DeSpriteAnimator, tag, DE_FK_NAME, AUTH,
                    {0, 0, 0, "Aseprite tag; zero selects the complete clip", 0, 0}),
        DE_FIELD_EX(DeSpriteAnimator, speed, DE_FK_F32, AUTH,
                    {0, 0, "x", "Playback multiplier", 0, 0}),
        DE_FIELD_EX(DeSpriteAnimator, time, DE_FK_F32, HIDDEN,
                    {0, 0, "s", "Current sprite playback time", 0, 0}),
        DE_FIELD(DeSpriteAnimator, playing, DE_FK_BOOL, AUTH),
        DE_FIELD(DeSpriteAnimator, loop, DE_FK_BOOL, AUTH),
        DE_FIELD(DeSpriteAnimator, ping_pong, DE_FK_BOOL, AUTH)};
    de_type_desc sprite = DE_TYPE_DESC(DeSpriteAnimator, 1, sprite_fields);
    sprite.hooks.construct = construct_sprite;
    DE_TRY(type(r, sprite, &id));
    de_component sprite_component = de_component_register(w, id, DE_COMP_SERIALIZE);
    if (!sprite_component.id)
        return DE_ERR_INTERNAL;
    s->component = animator_component;
    s->ik_component = ik_component;
    s->sprite_component = sprite_component;
    return DE_OK;
}
