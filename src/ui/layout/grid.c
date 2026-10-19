#include "layout_internal.h"
#include <math.h>
#include <string.h>
typedef struct grid_item {
    u32 index, column, row, columns, rows;
    bool explicit;
} grid_item;
typedef struct grid_plan {
    grid_item *items;
    u32 count, columns, rows;
    f32 *widths, *heights, column_gap, row_gap;
} grid_plan;
static bool occupied(const u8 *cells, u32 columns, u32 row, u32 column, u32 rows, u32 span) {
    for (u32 y = 0; y < rows; ++y)
        for (u32 x = 0; x < span; ++x)
            if (cells[(row + y) * columns + column + x])
                return true;
    return false;
}
static void occupy(u8 *cells, u32 columns, const grid_item *item) {
    for (u32 y = 0; y < item->rows; ++y)
        for (u32 x = 0; x < item->columns; ++x)
            cells[(item->row + y) * columns + item->column + x] = 1;
}
static f32 sum(const f32 *tracks, u32 count) {
    f32 value = 0;
    for (u32 i = 0; i < count; ++i)
        value += tracks[i];
    return value;
}
static de_ui_len track(const DeUiGrid *grid, bool rows, u32 index) {
    if (grid) {
        u32 count = rows ? grid->row_count : grid->column_count;
        if (count > 8)
            count = 8;
        if (index < count)
            return rows ? grid->rows[index] : grid->columns[index];
    }
    return (de_ui_len){rows ? DE_UI_AUTO : DE_UI_FR, 1};
}
static void grow_intrinsic(f32 *values, const DeUiGrid *grid, bool rows, u32 start, u32 count,
                           f32 preferred, f32 gap) {
    f32 current = (count - 1) * gap;
    u32 flexible = 0;
    for (u32 i = 0; i < count; ++i) {
        current += values[start + i];
        if (track(grid, rows, start + i).unit == DE_UI_AUTO)
            ++flexible;
    }
    if (preferred <= current || !flexible)
        return;
    f32 add = (preferred - current) / flexible;
    for (u32 i = 0; i < count; ++i)
        if (track(grid, rows, start + i).unit == DE_UI_AUTO)
            values[start + i] += add;
}
static void fractions(f32 *values, const DeUiGrid *grid, bool rows, u32 count, f32 extent,
                      f32 gap) {
    f32 fixed = (count > 0 ? count - 1 : 0) * gap, weights = 0;
    for (u32 i = 0; i < count; ++i) {
        de_ui_len l = track(grid, rows, i);
        if (l.unit == DE_UI_FR)
            weights += fmaxf(0, l.value);
        else
            fixed += values[i];
    }
    f32 free = fmaxf(0, extent - fixed);
    for (u32 i = 0; i < count; ++i) {
        de_ui_len l = track(grid, rows, i);
        if (l.unit == DE_UI_FR)
            values[i] = weights > 0 ? free * fmaxf(0, l.value) / weights : 0;
    }
}
static bool plan(de_ui_layout *layout, u32 index, de_vec2 available, grid_plan *out) {
    de_ui *ui = layout->ui;
    de_ui_record *parent = &ui->records[index];
    const DeUiGrid *grid = de_entity_get_const(ui->world, parent->entity, ui->ids.grid);
    memset(out, 0, sizeof *out);
    out->columns = grid && grid->column_count ? grid->column_count : 1;
    if (out->columns > 8)
        out->columns = 8;
    out->column_gap = grid ? grid->column_gap * ui->scale : 0;
    out->row_gap = grid ? grid->row_gap * ui->scale : 0;
    u32 capacity = 0;
    for (u32 c = parent->first_child; c != DE_UI_NONE_INDEX; c = ui->records[c].next_sibling)
        ++capacity;
    if (!capacity)
        capacity = 1;
    out->items = DE_ALLOC(&layout->allocator, (u64)capacity * sizeof *out->items,
                          _Alignof(grid_item), DE_MEM_UI);
    u32 max_rows = capacity * 8 + 264;
    u8 *cells = DE_ALLOC(&layout->allocator, (u64)max_rows * out->columns, 1, DE_MEM_UI);
    f32 *tracks = DE_ALLOC(&layout->allocator, (u64)(out->columns + max_rows) * sizeof *tracks,
                           _Alignof(f32), DE_MEM_UI);
    if (!out->items || !cells || !tracks)
        return false;
    memset(cells, 0, (size_t)max_rows * out->columns);
    memset(tracks, 0, (size_t)(out->columns + max_rows) * sizeof *tracks);
    out->widths = tracks;
    out->heights = tracks + out->columns;
    out->rows = grid ? grid->row_count : 0;
    if (out->rows > 8)
        out->rows = 8;
    for (u32 c = parent->first_child; c != DE_UI_NONE_INDEX; c = ui->records[c].next_sibling) {
        de_ui_record *r = &ui->records[c];
        if (r->node.display == DE_UI_NONE || de_entity_has(ui->world, r->entity, ui->ids.anchor) ||
            de_entity_has(ui->world, r->entity, ui->ids.popup))
            continue;
        grid_item item = {.index = c, .columns = 1, .rows = 1};
        const DeUiGridItem *explicit = de_entity_get_const(ui->world, r->entity, ui->ids.grid_item);
        if (explicit) {
            item.explicit = true;
            item.column = explicit->column < out->columns ? explicit->column : out->columns - 1;
            item.row = explicit->row;
            item.columns = explicit->column_span ? explicit->column_span : 1;
            item.rows = explicit->row_span ? explicit->row_span : 1;
            if (item.columns > out->columns - item.column)
                item.columns = out->columns - item.column;
            if (item.rows > 8)
                item.rows = 8;
            occupy(cells, out->columns, &item);
        }
        out->items[out->count++] = item;
    }
    u32 cursor = 0;
    for (u32 i = 0; i < out->count; ++i) {
        grid_item *item = &out->items[i];
        if (!item->explicit) {
            while (
                occupied(cells, out->columns, cursor / out->columns, cursor % out->columns, 1, 1))
                ++cursor;
            item->row = cursor / out->columns;
            item->column = cursor % out->columns;
            occupy(cells, out->columns, item);
            ++cursor;
        }
        if (item->row + item->rows > out->rows)
            out->rows = item->row + item->rows;
    }
    for (u32 col = 0; col < out->columns; ++col)
        out->widths[col] =
            fmaxf(0, de_ui__length(track(grid, false, col), available.x, ui->scale, 0));
    for (u32 row = 0; row < out->rows; ++row)
        out->heights[row] =
            fmaxf(0, de_ui__length(track(grid, true, row), available.y, ui->scale, 0));
    for (u32 i = 0; i < out->count; ++i) {
        grid_item *item = &out->items[i];
        de_vec2 size = de_ui__measure(layout, item->index, available);
        grow_intrinsic(out->widths, grid, false, item->column, item->columns, size.x,
                       out->column_gap);
    }
    fractions(out->widths, grid, false, out->columns, available.x, out->column_gap);
    for (u32 i = 0; i < out->count; ++i) {
        grid_item *item = &out->items[i];
        f32 width =
            sum(out->widths + item->column, item->columns) + (item->columns - 1) * out->column_gap;
        de_vec2 size = de_ui__measure(layout, item->index, (de_vec2){width, available.y});
        grow_intrinsic(out->heights, grid, true, item->row, item->rows, size.y, out->row_gap);
    }
    fractions(out->heights, grid, true, out->rows, available.y, out->row_gap);
    return true;
}
de_vec2 de_ui__grid_measure(de_ui_layout *layout, u32 index, de_vec2 available) {
    grid_plan p;
    if (!plan(layout, index, available, &p))
        return (de_vec2){0};
    return (de_vec2){p.columns ? sum(p.widths, p.columns) + (p.columns - 1) * p.column_gap : 0,
                     p.rows ? sum(p.heights, p.rows) + (p.rows - 1) * p.row_gap : 0};
}
void de_ui__grid(de_ui_layout *layout, u32 index, de_vec4 content) {
    grid_plan p;
    if (!plan(layout, index, (de_vec2){content.z, content.w}, &p))
        return;
    de_ui *ui = layout->ui;
    DeUiNode *parent = &ui->records[index].node;
    for (u32 c = ui->records[index].first_child; c != DE_UI_NONE_INDEX;
         c = ui->records[c].next_sibling) {
        de_ui_record *r = &ui->records[c];
        if (r->node.display == DE_UI_NONE)
            de_ui__arrange(layout, c, (de_vec4){0}, false);
        else if (!de_entity_has(ui->world, r->entity, ui->ids.popup)) {
            de_vec4 absolute;
            if (de_ui__absolute(layout, c, content, &absolute))
                de_ui__arrange(layout, c, absolute, false);
        }
    }
    for (u32 i = 0; i < p.count; ++i) {
        grid_item *item = &p.items[i];
        DeUiNode *node = &ui->records[item->index].node;
        f32 x = content.x + sum(p.widths, item->column) + item->column * p.column_gap,
            y = content.y + sum(p.heights, item->row) + item->row * p.row_gap,
            width =
                sum(p.widths + item->column, item->columns) + (item->columns - 1) * p.column_gap,
            height = sum(p.heights + item->row, item->rows) + (item->rows - 1) * p.row_gap;
        f32 ml = de_ui__length(node->margin[3], width, ui->scale, 0),
            mr = de_ui__length(node->margin[1], width, ui->scale, 0),
            mt = de_ui__length(node->margin[0], height, ui->scale, 0),
            mb = de_ui__length(node->margin[2], height, ui->scale, 0);
        x += ml;
        y += mt;
        width = fmaxf(0, width - ml - mr);
        height = fmaxf(0, height - mt - mb);
        de_vec2 preferred = de_ui__measure(layout, item->index, (de_vec2){width, height});
        de_ui_align align =
            node->align_self == DE_UI_ALIGN_AUTO ? parent->align_items : node->align_self;
        f32 w = node->width.unit == DE_UI_AUTO && align == DE_UI_ALIGN_STRETCH ? width
                                                                               : preferred.x,
            h = node->height.unit == DE_UI_AUTO && align == DE_UI_ALIGN_STRETCH ? height
                                                                                : preferred.y;
        if (align == DE_UI_ALIGN_CENTER) {
            x += (width - w) * .5f;
            y += (height - h) * .5f;
        } else if (align == DE_UI_ALIGN_END) {
            x += width - w;
            y += height - h;
        }
        de_ui__arrange(layout, item->index, (de_vec4){x, y, w, h}, false);
    }
}
