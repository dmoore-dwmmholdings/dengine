#include "ui_internal.h"
#include "layout/layout_internal.h"
#include "style/stylesheet_internal.h"
#include "widgets/widgets_internal.h"
#include <math.h>
#include <string.h>
#include <xxhash.h>
static u32 hash_entity(de_entity entity) {
    u64 v = entity.bits;
    v ^= v >> 33;
    v *= 0xff51afd7ed558ccdULL;
    v ^= v >> 33;
    return (u32)v;
}
u32 de_ui__find(const de_ui *ui, de_entity entity) {
    if (!entity.bits)
        return DE_UI_NONE_INDEX;
    u32 mask = ui->hash_capacity - 1, slot = hash_entity(entity) & mask;
    for (u32 i = 0; i < ui->hash_capacity; ++i) {
        u32 value = ui->hash[slot];
        if (!value)
            return DE_UI_NONE_INDEX;
        if (value != UINT32_MAX && ui->records[value - 1].entity.bits == entity.bits)
            return value - 1;
        slot = (slot + 1) & mask;
    }
    return DE_UI_NONE_INDEX;
}
static u32 insert_record(de_ui *ui, de_entity entity) {
    u32 index = ui->free_count ? ui->free_records[--ui->free_count] : ui->record_count++;
    if (index >= ui->capacity) {
        if (ui->record_count > ui->capacity)
            ui->record_count = ui->capacity;
        DE_FAIL(DE_ERR_OUT_OF_MEMORY, "UI node capacity exceeded");
        return DE_UI_NONE_INDEX;
    }
    de_ui_record *record = &ui->records[index];
    memset(record, 0, sizeof *record);
    record->entity = entity;
    record->alive = true;
    record->dirty = record->measure_dirty = true;
    record->first_child = record->next_sibling = DE_UI_NONE_INDEX;
    u32 slot = hash_entity(entity) & (ui->hash_capacity - 1);
    while (ui->hash[slot] && ui->hash[slot] != UINT32_MAX)
        slot = (slot + 1) & (ui->hash_capacity - 1);
    ui->hash[slot] = index + 1;
    record->hash_slot = slot;
    return index;
}
void de_ui__dirty(de_ui *ui, u32 index) {
    u32 steps = 0;
    while (index != DE_UI_NONE_INDEX && steps++ < ui->capacity) {
        de_ui_record *record = &ui->records[index];
        record->dirty = record->measure_dirty = true;
        index = de_ui__find(ui, record->parent);
    }
}
de_result de_ui__register_measure(de_ui *ui, de_component component, de_ui_measure_fn fn) {
    if (!ui || !component.id || !fn)
        return DE_ERR_INVALID_ARGUMENT;
    for (u32 i = 0; i < ui->measure_count; ++i)
        if (ui->measures[i].component.id == component.id) {
            ui->measures[i].measure = fn;
            return DE_OK;
        }
    if (ui->measure_count == DE_UI_MAX_MEASURES)
        return DE_ERR_OUT_OF_MEMORY;
    ui->measures[ui->measure_count++] = (de_ui_measure_entry){component, fn};
    return DE_OK;
}
de_str de_ui__text_content(de_ui *ui, const DeUiText *text) {
    if (text->loc_id.hash && ui->desc.text) {
        de_str translated = de_loc(ui->desc.text, text->loc_id);
        if (translated.ptr)
            return translated;
    }
    if (text->literal.ptr)
        return text->literal;
    const char *end = memchr(text->text, 0, sizeof text->text);
    if (!end) {
        DE_FAIL(DE_ERR_INVALID_ARGUMENT, "DeUiText.text must terminate within512bytes");
        return (de_str){0};
    }
    return (de_str){text->text, (size_t)(end - text->text)};
}
static void gather(const de_query_chunk *chunk, void *user) {
    de_ui *ui = user;
    const DeUiNode *nodes = chunk->columns[0];
    for (u32 i = 0; i < chunk->count; ++i) {
        de_entity entity = chunk->entities[i];
        u32 index = de_ui__find(ui, entity);
        if (index == DE_UI_NONE_INDEX)
            index = insert_record(ui, entity);
        if (index == DE_UI_NONE_INDEX)
            continue;
        de_ui_record *record = &ui->records[index];
        record->seen = ui->frame;
        record->node = nodes[i];
        de_component components[] = {ui->ids.anchor, ui->ids.grid,  ui->ids.grid_item,
                                     ui->ids.popup,  ui->ids.style, ui->ids.image,
                                     ui->ids.text};
        u32 mask = 0;
        for (u32 component = 0; component < 7; ++component)
            if (de_entity_has(ui->world, entity, components[component]))
                mask |= 1u << component;
        if (mask != record->component_mask) {
            record->component_mask = mask;
            record->dirty = record->measure_dirty = true;
        }
        de_entity parent = de_entity_parent(ui->world, entity);
        if (parent.bits != record->parent.bits) {
            de_ui__dirty(ui, de_ui__find(ui, record->parent));
            record->parent = parent;
            record->dirty = record->measure_dirty = true;
        }
        record->first_child = record->next_sibling = DE_UI_NONE_INDEX;
        const DeUiText *text = de_entity_get_const(ui->world, entity, ui->ids.text);
        if (text) {
            u64 locale_generation =
                text->loc_id.hash && ui->desc.text ? de_loc_generation(ui->desc.text) : 0;
            if (locale_generation != record->locale_generation) {
                record->locale_generation = locale_generation;
                record->dirty = record->measure_dirty = true;
            }
            de_str str = de_ui__text_content(ui, text);
            u64 hash = XXH3_64bits(str.ptr, str.len);
            de_font_metrics metrics = {0};
            if (ui->desc.text && text->style.font.id)
                (void)de_text_font_metrics(ui->desc.text, text->style.font,
                                           text->style.size_px > 0 ? text->style.size_px : 16,
                                           &metrics);
            if (hash != record->text_hash || metrics.generation != record->font_generation) {
                record->text_hash = hash;
                record->font_generation = metrics.generation;
                record->dirty = record->measure_dirty = true;
            }
        }
    }
}
static void changed(const de_query_chunk *chunk, void *user) {
    de_ui *ui = user;
    for (u32 i = 0; i < chunk->count; ++i)
        de_ui__dirty(ui, de_ui__find(ui, chunk->entities[i]));
}
static void detach(de_ui *ui) {
    if (ui->world) {
        de_ui__widgets_detach(ui);
        de_ui__style_detach(ui);
#if defined(DE_UI_RETAINED_RENDER)
        de_ui__render_shutdown(ui);
#endif
        if (ui->layout_system.id)
            de_system_remove(ui->world, ui->layout_system);
        if (ui->all_nodes.id)
            de_query_destroy(ui->world, ui->all_nodes);
        if (ui->resource.id)
            de_resource_remove(ui->world, ui->resource);
    }
    ui->world = NULL;
    ui->layout_system = (de_system){0};
    ui->all_nodes = (de_query){0};
    ui->resource = (de_resource_id){0};
    ui->measure_count = 0;
    ui->record_count = ui->free_count = 0;
    if (ui->hash)
        memset(ui->hash, 0, (size_t)ui->hash_capacity * sizeof *ui->hash);
}
de_result de_ui_create(const de_ui_desc *desc, de_ui **out) {
    if (!out)
        return DE_ERR_INVALID_ARGUMENT;
    *out = NULL;
    de_ui_desc d = desc ? *desc : (de_ui_desc){0};
    if (!d.allocator)
        d.allocator = de_heap_allocator();
    if (!isfinite(d.reference_height) || d.reference_height < 0)
        return DE_ERR_INVALID_ARGUMENT;
    if (d.reference_height == 0)
        d.reference_height = 1080;
    u32 capacity = d.max_nodes ? d.max_nodes : 65536;
    if (capacity > 1048576)
        return DE_ERR_INVALID_ARGUMENT;
    de_ui *ui = DE_NEW(d.allocator, de_ui, DE_MEM_UI);
    if (!ui)
        return DE_ERR_OUT_OF_MEMORY;
    memset(ui, 0, sizeof *ui);
    ui->alloc = d.allocator;
    ui->desc = d;
    ui->capacity = capacity;
    ui->hash_capacity = 16;
    while (ui->hash_capacity < capacity * 2)
        ui->hash_capacity *= 2;
    ui->records =
        DE_ALLOC(ui->alloc, (u64)capacity * sizeof *ui->records, _Alignof(de_ui_record), DE_MEM_UI);
    ui->hash =
        DE_ALLOC(ui->alloc, (u64)ui->hash_capacity * sizeof *ui->hash, _Alignof(u32), DE_MEM_UI);
    ui->free_records =
        DE_ALLOC(ui->alloc, (u64)capacity * sizeof *ui->free_records, _Alignof(u32), DE_MEM_UI);
    if (!ui->records || !ui->hash || !ui->free_records ||
        de_arena_create(&(de_arena_desc){.reserve_bytes = 128 * 1024 * 1024}, &ui->scratch) !=
            DE_OK) {
        de_ui_destroy(ui);
        return DE_ERR_OUT_OF_MEMORY;
    }
    memset(ui->records, 0, (size_t)capacity * sizeof *ui->records);
    memset(ui->hash, 0, (size_t)ui->hash_capacity * sizeof *ui->hash);
    ui->scale_cvar =
        de_cvar_register(&(de_cvar_desc){.name = "ui.scale",
                                         .type = DE_CVAR_FLOAT,
                                         .default_number = 1,
                                         .min = .1,
                                         .max = 4,
                                         .help = "Retained UI reference-pixel scale multiplier"});
    ui->width = 1920;
    ui->height = 1080;
    ui->viewport_dirty = true;
    de_result style_result = de_ui__style_init(ui);
    if (style_result != DE_OK) {
        de_ui_destroy(ui);
        return style_result;
    }
#if defined(DE_UI_RETAINED_RENDER)
    de_result render_result = de_ui__render_init(ui);
    if (render_result != DE_OK) {
        de_ui_destroy(ui);
        return render_result;
    }
#endif
    de_result widgets_result = de_ui__widgets_init(ui);
    if (widgets_result != DE_OK) {
        de_ui_destroy(ui);
        return widgets_result;
    }
    *out = ui;
    return DE_OK;
}
void de_ui_destroy(de_ui *ui) {
    if (!ui)
        return;
#if defined(DE_UI_RETAINED_RENDER)
    if (!ui->world)
        de_ui__render_shutdown(ui);
#endif
    de_ui__widgets_shutdown(ui);
    de_ui__style_shutdown(ui);
    detach(ui);
    if (ui->scratch)
        de_arena_destroy(ui->scratch);
    if (ui->records)
        DE_FREE(ui->alloc, ui->records, (u64)ui->capacity * sizeof *ui->records);
    if (ui->hash)
        DE_FREE(ui->alloc, ui->hash, (u64)ui->hash_capacity * sizeof *ui->hash);
    if (ui->free_records)
        DE_FREE(ui->alloc, ui->free_records, (u64)ui->capacity * sizeof *ui->free_records);
    DE_FREE(ui->alloc, ui, sizeof *ui);
}
void de_ui_attach(de_ui *ui, de_world *world) {
    if (!ui || ui->world == world)
        return;
    detach(ui);
    if (!world)
        return;
    de_type_id existing_type = de_type_find(de_world_types(world), "de_ui_resource");
    if (existing_type.id) {
        de_resource_id existing = de_resource_find(world, existing_type);
        const de_ui_resource *resource = existing.id ? de_resource_get(world, existing) : NULL;
        if (resource && resource->ui != ui) {
            DE_FAIL(DE_ERR_ALREADY_EXISTS, "world already has a retained UI system");
            return;
        }
    }
    ui->hierarchy = de_builtin_components(world);
    if (!ui->hierarchy.child_of.id)
        return;
    ui->world = world;
    if (de_ui__reflect(ui, world) != DE_OK) {
        detach(ui);
        return;
    }
    de_query_term term = {ui->ids.node, DE_READ};
    de_query_desc all = {.terms = &term, .term_count = 1};
    ui->all_nodes = de_query_create(world, &all);
    de_component changes[] = {ui->ids.node,          ui->ids.anchor, ui->ids.grid,
                              ui->ids.grid_item,     ui->ids.popup,  ui->ids.style,
                              ui->ids.image,         ui->ids.text,   ui->hierarchy.child_of,
                              ui->hierarchy.children};
    de_query_desc queries[11];
    de_query_filter filters[10];
    queries[0] = all;
    for (u32 i = 0; i < 10; ++i) {
        filters[i] = (de_query_filter){changes[i], DE_CHANGED};
        queries[i + 1] = (de_query_desc){
            .terms = &term, .term_count = 1, .filters = &filters[i], .filter_count = 1};
    }
    de_resource_access resource = {ui->resource, DE_WRITE};
    ui->layout_system =
        de_system_add(world, &(de_system_desc){.name = DE_UI_LAYOUT_SYSTEM,
                                               .schedule = DE_SCHEDULE_POST_UPDATE,
                                               .fn = de_ui__layout_system,
                                               .user = ui,
                                               .flags = DE_SYSTEM_MAIN_THREAD | DE_SYSTEM_EXCLUSIVE,
                                               .queries = queries,
                                               .query_count = 11,
                                               .resources = &resource,
                                               .resource_count = 1});
    if (!ui->all_nodes.id || !ui->layout_system.id) {
        detach(ui);
        return;
    }
    (void)de_ui__register_measure(ui, ui->ids.text, de_ui__measure_text);
    (void)de_ui__register_measure(ui, ui->ids.image, de_ui__measure_image);
    de_ui__style_attach(ui);
#if defined(DE_UI_RETAINED_RENDER)
    de_result render_result = ui->render ? DE_OK : de_ui__render_init(ui);
    if (render_result == DE_OK)
        render_result = de_ui__render_attach(ui);
    if (render_result != DE_OK) {
        detach(ui);
        return;
    }
#endif
    de_ui__widgets_attach(ui);
    ui->viewport_dirty = true;
}
void de_ui_set_viewport(de_ui *ui, u32 width, u32 height, de_window_insets safe) {
    if (!ui)
        return;
    if (width != ui->width || height != ui->height || memcmp(&safe, &ui->safe, sizeof safe))
        ui->viewport_dirty = true;
    ui->width = width;
    ui->height = height;
    ui->safe = safe;
    ui->viewport_override = true;
}
de_ui_layout_stats de_ui_get_layout_stats(const de_ui *ui) {
    return ui ? ui->stats : (de_ui_layout_stats){0};
}
void de_ui__layout_system(de_system_ctx *ctx) {
    de_ui *ui = ctx->user;
    ++ui->frame;
    memset(&ui->stats, 0, sizeof ui->stats);
    if (!ui->viewport_override) {
        de_window window = ui->desc.window;
        if (window.id) {
            u32 width = 0, height = 0;
            de_window_size_pixels(window, &width, &height);
            de_window_insets safe = de_window_safe_area(window);
            if (width != ui->width || height != ui->height || memcmp(&safe, &ui->safe, sizeof safe))
                ui->viewport_dirty = true;
            ui->width = width;
            ui->height = height;
            ui->safe = safe;
        }
#if DE_HAS_RENDER
        else if (ui->desc.renderer) {
            u32 width = 0, height = 0;
            de_renderer_output_size(ui->desc.renderer, &width, &height);
            if (width != ui->width || height != ui->height)
                ui->viewport_dirty = true;
            ui->width = width;
            ui->height = height;
        }
#endif
    }
    f32 scale = (f32)ui->height / ui->desc.reference_height * (f32)de_cvar_float(ui->scale_cvar);
    if (!isfinite(scale) || scale <= 0)
        scale = 1;
    if (scale != ui->scale)
        ui->viewport_dirty = true;
    ui->scale = scale;
    ui->stats.scale = scale;
    de_query_each(ui->world, ctx->queries[0], gather, ui);
    for (u32 i = 0; i < ui->record_count; ++i) {
        de_ui_record *r = &ui->records[i];
        if (!r->alive)
            continue;
        if (r->seen != ui->frame) {
            de_ui__dirty(ui, de_ui__find(ui, r->parent));
            ui->hash[r->hash_slot] = UINT32_MAX;
            r->alive = false;
            ui->free_records[ui->free_count++] = i;
            continue;
        }
        ++ui->stats.total_nodes;
        if (ui->viewport_dirty)
            r->dirty = r->measure_dirty = true;
        u32 last = DE_UI_NONE_INDEX;
        for (de_entity child = de_entity_children_first(ui->world, r->entity); child.bits;
             child = de_entity_children_next(ui->world, child)) {
            u32 index = de_ui__find(ui, child);
            if (index == DE_UI_NONE_INDEX || ui->records[index].seen != ui->frame)
                continue;
            if (last == DE_UI_NONE_INDEX)
                r->first_child = index;
            else
                ui->records[last].next_sibling = index;
            last = index;
        }
    }
    for (u32 i = 1; i < 11; ++i)
        de_query_each(ui->world, ctx->queries[i], changed, ui);
    for (u32 i = 0; i < ui->record_count; ++i)
        if (ui->records[i].alive && ui->records[i].dirty)
            de_ui__dirty(ui, i);
    de_arena *arena = de_frame_arena();
    if (!arena) {
        de_arena_reset(ui->scratch);
        arena = ui->scratch;
    }
    de_ui_layout layout = {ui, de_arena_allocator(arena)};
    for (u32 i = 0; i < ui->record_count; ++i)
        if (ui->records[i].alive && ui->records[i].dirty)
            ++ui->stats.dirty_nodes;
    for (u32 i = 0; i < ui->record_count; ++i) {
        de_ui_record *r = &ui->records[i];
        if (!r->alive)
            continue;
        if (de_ui__find(ui, r->parent) == DE_UI_NONE_INDEX &&
            !de_entity_has(ui->world, r->entity, ui->ids.popup)) {
            de_vec4 rect = {0, 0, (f32)ui->width, (f32)ui->height}, absolute;
            if (de_ui__absolute(&layout, i, rect, &absolute))
                rect = absolute;
            de_ui__arrange(&layout, i, rect, ui->viewport_dirty);
        }
    }
    de_ui__popups(&layout);
    ui->viewport_dirty = false;
}
