/* hierarchy.c: the ChildOf / Children hierarchy: an intrusive, insertion-ordered child list
 * (DENG-351, SPEC §7.1 Relationships).
 *
 * DeChildOf {parent, next_sibling, prev_sibling} links the children of one parent into a doubly
 * linked list; the parent's DeChildren {first, last, count} holds its ends. All four built-in
 * components are POD, so tables move them with memcpy and snapshots copy them as bytes.
 *
 * Invariants (checked by hier.random_ops_invariants): an entity has DeChildOf exactly when it
 * has a parent, which is alive and lists it; it has DeChildren exactly when count >= 1; walking
 * first -> next_sibling visits `count` children whose prev_sibling links mirror the walk and whose
 * parent is the list's owner, ending at `last`; no entity is its own ancestor.
 *
 * Every function here is single-threaded (structural). Pointers into tables are fetched again
 * after each structural change (an add or remove moves rows). */
#include "hierarchy_internal.h"

#include "change_internal.h" /* ecs_entity_mark_changed */

#if defined(DE_DEBUG) || defined(DE_DEV)
static de_log_category hier_log_category(void) {
    return de_log_register_category("ecs", DE_LOG_INFO);
}
#define HIER_WARN(...) DE_WARN(hier_log_category(), __VA_ARGS__)
#else
#define HIER_WARN(...) ((void)0)
#endif

static DeChildOf *child_of_of(const de_world *w, de_entity e) {
    const ecs_record *rec = ecs_entity_record(w, e);
    return rec ? (DeChildOf *)ecs_hier_data(w, rec, w->hier->ids.child_of.id) : NULL;
}

static DeChildren *children_of(const de_world *w, de_entity e) {
    const ecs_record *rec = ecs_entity_record(w, e);
    return rec ? (DeChildren *)ecs_hier_data(w, rec, w->hier->ids.children.id) : NULL;
}

static bool same(de_entity a, de_entity b) {
    return a.bits == b.bits;
}

/* true if `ancestor` is `e` or one of its ancestors. The walk is bounded by the entity count, so
 * corrupted links cannot loop forever. */
static bool is_self_or_ancestor(const de_world *w, de_entity ancestor, de_entity e) {
    u64 steps = (u64)w->records.count + 1u;
    while (e.bits != 0 && steps-- > 0) {
        if (same(e, ancestor))
            return true;
        const DeChildOf *co = child_of_of(w, e);
        e = co ? co->parent : DE_ENTITY_NULL;
    }
    return false;
}

/* Removes `child` (which has DeChildOf) from its parent's list and clears its links. The parent
 * loses DeChildren with its last child. */
static void unlink_child(de_world *w, de_entity child) {
    const de_builtin_component_ids *ids = &w->hier->ids;
    DeChildOf *co = child_of_of(w, child);
    DE_ASSERT(co != NULL);
    const de_entity parent = co->parent, prev = co->prev_sibling, next = co->next_sibling;
    co->parent = DE_ENTITY_NULL;
    co->prev_sibling = DE_ENTITY_NULL;
    co->next_sibling = DE_ENTITY_NULL;
    if (prev.bits != 0) {
        DeChildOf *p = child_of_of(w, prev);
        if (p != NULL)
            p->next_sibling = next;
    }
    if (next.bits != 0) {
        DeChildOf *n = child_of_of(w, next);
        if (n != NULL)
            n->prev_sibling = prev;
    }
    DeChildren *ch = children_of(w, parent);
    if (ch == NULL)
        return; /* the parent is gone (only after an out-of-memory detach); nothing to fix */
    if (prev.bits == 0)
        ch->first = next;
    if (next.bits == 0)
        ch->last = prev;
    DE_ASSERT(ch->count > 0);
    ch->count -= 1u;
    if (ch->count == 0) {
        /* Out of memory keeps an empty DeChildren; every reader treats count 0 as no children. */
        (void)ecs_entity_remove(w, parent, ids->children.id);
    } else {
        ecs_entity_mark_changed(w, parent, ids->children.id);
    }
}

/* Appends `child` (which has DeChildOf, unlinked) to the list of `parent` (which has
 * DeChildren). */
static void link_last(de_world *w, de_entity child, de_entity parent) {
    DeChildren *ch = children_of(w, parent);
    DeChildOf *co = child_of_of(w, child);
    DE_ASSERT(ch != NULL && co != NULL);
    co->parent = parent;
    co->prev_sibling = ch->last;
    co->next_sibling = DE_ENTITY_NULL;
    if (ch->last.bits != 0) {
        DeChildOf *last = child_of_of(w, ch->last);
        DE_ASSERT(last != NULL);
        last->next_sibling = child;
    } else {
        ch->first = child;
    }
    ch->last = child;
    ch->count += 1u;
}

de_result ecs_hier_set_parent(de_world *w, de_entity child, de_entity parent, bool from_command) {
    const de_builtin_component_ids *ids = &w->hier->ids;
    if (ecs_entity_record(w, child) == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: set_parent: child 0x%016llx is not alive",
                       (unsigned long long)child.bits);
    DeChildOf *co = child_of_of(w, child);

    if (parent.bits == 0) { /* detach */
        if (co == NULL)
            return DE_OK;
        unlink_child(w, child);
        const de_result r = ecs_entity_remove(w, child, ids->child_of.id);
        ecs_entity_mark_changed(w, child, ids->transform.id);
        return r;
    }
    if (ecs_entity_record(w, parent) == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "ecs: set_parent: parent 0x%016llx is not alive",
                       (unsigned long long)parent.bits);
    if (co != NULL && same(co->parent, parent))
        return DE_OK; /* already there: keep the child's place */
    if (is_self_or_ancestor(w, child, parent)) {
        if (from_command) {
            HIER_WARN("ecs: deferred set_parent of 0x%016llx under 0x%016llx would make a "
                      "cycle; dropped",
                      (unsigned long long)child.bits, (unsigned long long)parent.bits);
        } else {
#if defined(DE_DEBUG) || defined(DE_DEV)
            /* The Debug assert of the immediate call: a cycle is a bug in the caller. */
            DE_PANIC("de_entity_set_parent: 0x%016llx under 0x%016llx would make a cycle (the "
                     "parent is the child or one of its descendants)",
                     (unsigned long long)child.bits, (unsigned long long)parent.bits);
#endif
        }
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT,
                       "ecs: set_parent of 0x%016llx under 0x%016llx would make a cycle",
                       (unsigned long long)child.bits, (unsigned long long)parent.bits);
    }

    /* Components first, so a failure leaves the hierarchy as it was. */
    const bool parent_had_children = children_of(w, parent) != NULL;
    if (!parent_had_children)
        DE_TRY(ecs_entity_add(w, parent, ids->children.id, NULL));
    if (co == NULL) {
        const de_result r = ecs_entity_add(w, child, ids->child_of.id, NULL);
        if (r != DE_OK) {
            if (!parent_had_children)
                (void)ecs_entity_remove(w, parent, ids->children.id);
            return r;
        }
    } else {
        /* The old parent is not `parent`, so its possible loss of DeChildren does not touch the
         * list we append to. */
        unlink_child(w, child);
    }
    link_last(w, child, parent);
    ecs_entity_mark_changed(w, child, ids->child_of.id);
    ecs_entity_mark_changed(w, child, ids->transform.id);
    ecs_entity_mark_changed(w, parent, ids->children.id);
    return DE_OK;
}

void ecs_hier_on_despawn(de_world *w, de_entity e) {
    if (w->hier == NULL || ecs_entity_record(w, e) == NULL)
        return;
    const de_builtin_component_ids *ids = &w->hier->ids;
    if (child_of_of(w, e) != NULL)
        unlink_child(w, e);
    const DeChildren *ch = children_of(w, e);
    de_entity c = ch != NULL && ch->count > 0 ? ch->first : DE_ENTITY_NULL;
    /* Children become roots, first to last. */
    while (c.bits != 0) {
        DeChildOf *co = child_of_of(w, c);
        if (co == NULL)
            break; /* corrupted list: stop rather than guess */
        const de_entity next = co->next_sibling;
        co->parent = DE_ENTITY_NULL;
        co->prev_sibling = DE_ENTITY_NULL;
        co->next_sibling = DE_ENTITY_NULL;
        (void)ecs_entity_remove(w, c, ids->child_of.id);
        ecs_entity_mark_changed(w, c, ids->transform.id);
        c = next;
    }
}

void ecs_hier_despawn_recursive(de_world *w, de_entity e) {
    if (ecs_entity_record(w, e) == NULL)
        return;
    if (w->hier != NULL) {
        /* Post-order without a stack: go down first children to a leaf, despawn it (which
         * unlinks it, so its next sibling becomes the first child), continue from its parent.
         * Each step despawns one entity; the walk down is amortised by the climb. */
        de_entity n = e;
        for (;;) {
            for (;;) {
                const DeChildren *ch = children_of(w, n);
                if (ch == NULL || ch->count == 0 || ch->first.bits == 0)
                    break;
                n = ch->first;
            }
            if (same(n, e))
                break;
            const DeChildOf *co = child_of_of(w, n);
            const de_entity p = co != NULL ? co->parent : e;
            ecs_hier_on_despawn(w, n);
            (void)ecs_entity_despawn(w, n);
            n = ecs_entity_record(w, p) != NULL ? p : e;
        }
        ecs_hier_on_despawn(w, e);
    }
    (void)ecs_entity_despawn(w, e);
}

/* ---- Public API ------------------------------------------------------------------------------ */

de_result de_entity_set_parent(de_world *w, de_entity child, de_entity parent) {
    if (w == NULL)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de_entity_set_parent: world is NULL");
    ECS_ASSERT_STRUCTURAL(w);
    ECS_ASSERT_NOT_PENDING(child);
    ECS_ASSERT_NOT_PENDING(parent);
    DE_TRY(ecs_builtins_ensure(w));
    return ecs_hier_set_parent(w, child, parent, false);
}

de_entity de_entity_parent(const de_world *w, de_entity e) {
    if (w == NULL || w->hier == NULL)
        return DE_ENTITY_NULL;
    const DeChildOf *co = child_of_of(w, e);
    return co ? co->parent : DE_ENTITY_NULL;
}

de_entity de_entity_children_first(const de_world *w, de_entity parent) {
    if (w == NULL || w->hier == NULL)
        return DE_ENTITY_NULL;
    const DeChildren *ch = children_of(w, parent);
    return ch != NULL && ch->count > 0 ? ch->first : DE_ENTITY_NULL;
}

de_entity de_entity_children_next(const de_world *w, de_entity child) {
    if (w == NULL || w->hier == NULL)
        return DE_ENTITY_NULL;
    const DeChildOf *co = child_of_of(w, child);
    return co ? co->next_sibling : DE_ENTITY_NULL;
}

u32 de_entity_children_count(const de_world *w, de_entity parent) {
    if (w == NULL || w->hier == NULL)
        return 0;
    const DeChildren *ch = children_of(w, parent);
    return ch ? ch->count : 0u;
}

void de_entity_despawn_recursive(de_world *w, de_entity e) {
    if (w == NULL)
        return;
    ECS_ASSERT_STRUCTURAL(w);
    ECS_ASSERT_NOT_PENDING(e);
    if (ecs_entity_record(w, e) == NULL) {
        HIER_WARN("de_entity_despawn_recursive: entity 0x%016llx is not alive (ignored)",
                  (unsigned long long)e.bits);
        return;
    }
    ecs_hier_despawn_recursive(w, e);
}
