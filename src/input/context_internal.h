/* context_internal.h: the context stack behind de_action_map_push / pop and the named context
 * sets (DENG-85). context_stack.c implements it; action_internal.h embeds de__context_state in
 * struct de__actions, and action_eval.c reaches it only through the de__context_* hooks declared
 * there.
 *
 * Two stacks. `requested` changes at once, in call order, when the game pushes or pops (so errors
 * such as an overflow are returned to the caller). `applied` is what evaluation uses; it becomes
 * a copy of `requested` at the start of the next frame evaluation (de__context_frame_begin), and
 * the maps that joined or left the stack get their transitions then. So a push or pop from inside
 * a system never changes what other systems of the same frame read.
 *
 * Everything is in fixed arrays: no heap after the action block, no hash map. Maps are evaluated
 * from the top of the applied stack down, so the order is the stack order (SPEC §5.7). */
#ifndef DE_INPUT_CONTEXT_INTERNAL_H
#define DE_INPUT_CONTEXT_INTERNAL_H
#include "input_internal.h"

DE_EXTERN_C_BEGIN

/** Index meaning "no named context" (a de__context_entry pushed by de_action_map_push). */
#define DE__CONTEXT_NONE 0xFFFFFFFFu
/** Codes per binding source that the consumed-input bitsets cover (keys are the largest). */
#define DE__CONTEXT_CODES      DE_KEY_COUNT_
#define DE__CONTEXT_CODE_WORDS (DE__CONTEXT_CODES / 64)

DE_STATIC_ASSERT(DE__CONTEXT_CODES % 64 == 0, "whole words");
DE_STATIC_ASSERT(DE_ACTION_MAX_BINDINGS <= 16, "a u16 holds one bit per binding of an action");

/** One entry of a stack: a map, and the named context that pushed it. */
typedef struct de__context_entry {
    u32 map;     /**< map index */
    u32 context; /**< index into de__context_state.sets, or DE__CONTEXT_NONE */
} de__context_entry;

/** A named context set (de_input_context_define). */
typedef struct de__context_set {
    char name[DE_ACTION_NAME_MAX + 1];
    de_name id;
    u32 flags; /**< DE_INPUT_CONTEXT_* */
    u32 map_count;
    u32 maps[DE_INPUT_CONTEXT_MAX_MAPS]; /**< map indices, bottom to top */
} de__context_set;

/** Raw inputs as (source, code) bits. */
typedef struct de__context_inputs {
    u64 bits[DE_BIND_SOURCE_COUNT_][DE__CONTEXT_CODE_WORDS];
} de__context_inputs;

typedef struct de__context_state {
    de__context_entry requested[DE_INPUT_CONTEXT_STACK_MAX]; /**< bottom to top */
    u32 requested_count;
    de__context_entry applied[DE_INPUT_CONTEXT_STACK_MAX]; /**< bottom to top */
    u32 applied_count;

    de__context_set sets[DE_INPUT_MAX_CONTEXTS]; /**< in definition order */
    u32 set_count;

    u32 map_flags[DE_ACTION_MAX_MAPS]; /**< de_action_map_set_flags */
    /* Per frame, from de__context_frame_begin. */
    bool on_stack[DE_ACTION_MAX_MAPS];  /**< on the applied stack */
    bool blocks[DE_ACTION_MAX_MAPS];    /**< its used inputs are consumed for lower maps */
    bool releasing[DE_ACTION_MAX_MAPS]; /**< left the stack; evaluated with every input consumed
                                           until its actions are idle and their triggers ticked */
    /** Bit j of latched[a]: binding j of action a ignores its input until it is released (it was
     * held when the map joined the stack, or while a higher map consumed it). */
    u16 latched[DE_ACTION_MAX_ACTIONS];

    de__context_inputs consumed; /**< used by blocking maps above the one being evaluated */
    de__context_inputs pending;  /**< used by the blocking map being evaluated */
    u32 eval_map;                /**< the map `pending` belongs to, or DE__CONTEXT_NONE */

    u64 applies;   /**< de__context_frame_begin calls that changed the applied stack */
    u64 overflows; /**< pushes refused because the stack was full */
} de__context_state;

/* ---------- Queries for the other input tasks ---------- */

/**
 * True when maps `a` and `b` (map indices) are the same map, or are both members of one named
 * context set (de_input_context_define), whether or not that set is on the stack. DENG-86 uses
 * it for rebinding conflicts ("another action in the same map or the same context set").
 */
bool de__context_maps_share_set(const de_input *in, u32 a, u32 b);
/** Index of the named context `id`, or DE__CONTEXT_NONE. */
u32 de__context_find(const de_input *in, de_name id);
/** Number of entries on the requested stack (what the next frame update applies). */
u32 de__context_requested_depth(const de_input *in);

DE_EXTERN_C_END
#endif
