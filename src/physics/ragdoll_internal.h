/*
 * ragdoll_internal.h: ragdolls of the 3D physics world (DENG-224; SPEC §13.4, §13.2, §5.7).
 * Internal: include as "physics/ragdoll_internal.h". The public contract is the Ragdolls block of
 * de_physics.h.
 *
 * State. A pool of ragdoll records in the world's DE__PHYS_EXT_RAGDOLL slot, created by the first
 * de_ragdoll_create: records with generations (handles pack index + 1 and a generation like
 * bodies), a LIFO free list, and per record one allocation with its bones. The pool grows by
 * doubling at create time only.
 *
 * A ragdoll owns its shapes, bodies and joints and only addresses them: it adds no step work. Its
 * bones keep what the getters and setters need: the shape offset (bone -> body) and its inverse,
 * the joint frame rotations in the parent's and the bone's frames (motor targets are converted to
 * joint coordinates with them), and the current motor target.
 *
 * Order (SPEC §5.7): create builds shapes, then bodies, then joints, each in bone order (a bone's
 * joint, then its motor joint). Destroy releases them in the reverse order, so a destroy followed
 * by the same create gives the same body and joint indices.
 */
#ifndef DE_PHYSICS_RAGDOLL_INTERNAL_H
#define DE_PHYSICS_RAGDOLL_INTERNAL_H

#include "physics/physics_internal.h"

DE_EXTERN_C_BEGIN

/** One bone of a live ragdoll. */
typedef struct de__ragdoll_bone {
    de_body body;
    de_joint joint;       /**< to the parent; invalid for the root */
    de_joint motor;       /**< the 6-DOF motor joint of a powered cone-twist bone, else invalid */
    de_shape shape;       /**< owned */
    u32 parent;           /**< DE_RAGDOLL_NO_PARENT for the root */
    u8 kind;              /**< de_ragdoll_joint */
    u8 powered;           /**< motor_max_force > 0 (and not the root) */
    de_transform offset;  /**< body pose in the bone frame (shape_offset, normalized) */
    de_transform inv_off; /**< bone pose in the body frame */
    de_quat frame_parent; /**< joint frame rotation in the parent bone's frame (bind pose) */
    de_quat frame_bone;   /**< joint frame rotation in this bone's frame */
    de_quat target;       /**< motor target relative to the parent (normalized) */
} de__ragdoll_bone;

/** One ragdoll slot. */
typedef struct de__ragdoll {
    u32 generation; /**< of the current (or next) ragdoll in this slot, 1..4095 */
    u32 alive;
    u32 bone_count;
    u32 group; /**< collision group of the bones (0 with self_collision and no group) */
    de__ragdoll_bone *bones;
} de__ragdoll;

/** The pool (DE__PHYS_EXT_RAGDOLL). */
typedef struct de__ragdolls {
    const de_allocator *alloc;
    de__ragdoll *items; /**< [capacity] */
    u32 *free_list;     /**< [capacity] LIFO */
    u32 capacity;
    u32 high_water;
    u32 free_count;
    u32 live_count;
} de__ragdolls;

/** The ragdoll pool, or NULL before the first ragdoll. */
static inline de__ragdolls *de__ragdolls_get(const de_physics_world *p) {
    return (de__ragdolls *)de__phys_ext_get(p, DE__PHYS_EXT_RAGDOLL);
}

/** The live ragdoll of handle `r`, or NULL. */
de__ragdoll *de__ragdoll_resolve(const de_physics_world *p, de_ragdoll r);

DE_EXTERN_C_END

#endif /* DE_PHYSICS_RAGDOLL_INTERNAL_H */
