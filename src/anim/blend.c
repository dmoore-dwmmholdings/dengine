#include "anim/blend.h"
#include "anim/anim_math.h"

static bool matching(const de__anim_local_pose *out, const de__anim_local_pose *a,
                     const de__anim_local_pose *b) {
    return de__anim_pose_valid(out) && de__anim_pose_valid(a) && de__anim_pose_valid(b) &&
           out->count == a->count && out->count == b->count;
}
bool de__anim_pose_delta(de__anim_local_pose *out, const de__anim_local_pose *sample,
                         const de__anim_local_pose *reference, bool deterministic) {
    if (!matching(out, sample, reference))
        return false;
    for (u32 j = 0; j < out->count; ++j) {
        out->rotations[j] =
            de__anim_rotation_delta(sample->rotations[j], reference->rotations[j], deterministic);
        out->translations[j] =
            de__anim_difference(sample->translations[j], reference->translations[j]);
        out->scales[j] = de__anim_difference(sample->scales[j], reference->scales[j]);
    }
    out->root_rotation =
        de__anim_rotation_delta(sample->root_rotation, reference->root_rotation, deterministic);
    out->root_translation =
        de__anim_difference(sample->root_translation, reference->root_translation);
    return true;
}
bool de__anim_pose_layer(de__anim_local_pose *out, const de__anim_local_pose *base,
                         const de__anim_local_pose *layer, f32 weight, const f32 *mask,
                         f32 root_mask, bool additive, bool deterministic) {
    if (!matching(out, base, layer) || !(weight >= 0 && weight <= 1) ||
        !(root_mask >= 0 && root_mask <= 1))
        return false;
    if (mask)
        for (u32 j = 0; j < out->count; ++j)
            if (!(mask[j] >= 0 && mask[j] <= 1))
                return false;
    for (u32 j = 0; j < out->count; ++j) {
        f32 t = weight * (mask ? mask[j] : 1.f);
        if (t == 0) {
            out->rotations[j] = base->rotations[j];
            out->translations[j] = base->translations[j];
            out->scales[j] = base->scales[j];
        } else if (additive) {
            out->rotations[j] =
                de__anim_add_rotation(base->rotations[j], layer->rotations[j], t, deterministic);
            out->translations[j] =
                de__anim_add_delta(base->translations[j], layer->translations[j], t);
            out->scales[j] = de__anim_add_delta(base->scales[j], layer->scales[j], t);
        } else if (t == 1) {
            out->rotations[j] = layer->rotations[j];
            out->translations[j] = layer->translations[j];
            out->scales[j] = layer->scales[j];
        } else {
            out->rotations[j] =
                de__anim_nlerp(base->rotations[j], layer->rotations[j], t, deterministic);
            out->translations[j] =
                de__anim_lerp(base->translations[j], layer->translations[j], t, deterministic);
            out->scales[j] = de__anim_lerp(base->scales[j], layer->scales[j], t, deterministic);
        }
    }
    f32 t = weight * root_mask;
    if (t == 0) {
        out->root_rotation = base->root_rotation;
        out->root_translation = base->root_translation;
    } else if (additive) {
        out->root_rotation =
            de__anim_add_rotation(base->root_rotation, layer->root_rotation, t, deterministic);
        out->root_translation =
            de__anim_add_delta(base->root_translation, layer->root_translation, t);
    } else if (t == 1) {
        out->root_rotation = layer->root_rotation;
        out->root_translation = layer->root_translation;
    } else {
        out->root_rotation =
            de__anim_nlerp(base->root_rotation, layer->root_rotation, t, deterministic);
        out->root_translation =
            de__anim_lerp(base->root_translation, layer->root_translation, t, deterministic);
    }
    return true;
}
bool de__anim_mask_resolve(const de_cooked_skeleton_view *skeleton, const de__anim_bone_mask *rules,
                           u32 rule_count, f32 *mask) {
    if (!skeleton || !mask || !skeleton->name_hashes || !skeleton->parents ||
        (rule_count && !rules))
        return false;
    for (u32 i = 0; i < rule_count; ++i) {
        if (!(rules[i].weight >= 0 && rules[i].weight <= 1))
            return false;
        bool found = false;
        for (u32 j = 0; j < skeleton->joint_count; ++j)
            found |= de_canim_get_u64(skeleton->name_hashes + (size_t)j * 8) == rules[i].bone.hash;
        if (!found)
            return false;
    }
    for (u32 j = 0; j < skeleton->joint_count; ++j) {
        f32 weight = rule_count ? 0.f : 1.f;
        for (u32 i = 0; i < rule_count; ++i) {
            u32 ancestor = j;
            for (;;) {
                if (de_canim_get_u64(skeleton->name_hashes + (size_t)ancestor * 8) ==
                    rules[i].bone.hash) {
                    if (rules[i].weight > weight)
                        weight = rules[i].weight;
                    break;
                }
                if (!rules[i].include_children)
                    break;
                u16 parent = de_canim_get_u16(skeleton->parents + (size_t)ancestor * 2);
                if (parent == DE_COOKED_SKEL_NO_PARENT || parent >= ancestor)
                    break;
                ancestor = parent;
            }
        }
        mask[j] = weight;
    }
    return true;
}
