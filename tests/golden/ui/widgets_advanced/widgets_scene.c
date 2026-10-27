#include "../glyph_wait.h"
/* DENG-319: actual default-styled controls and states through the retained renderer.
 * Test-only host kick/readback follows the S10/S29 golden convention. */
#include "golden.h"
#include "cooked_assets.h"
#include <dengine/dengine.h>
#include "ui/widgets/advanced_internal.h"
#include "../../../unit/input/input_test_util.h"
#include <stdio.h>
#include "ui/style/stylesheet_internal.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#if DE_HAS_UI && DE_HAS_RENDER && defined(DE_GOLDEN_COOK)
#define UI_W 1280u
#define UI_H 720u
static const de_vec4 background = {.025f, .035f, .05f, 1};
static const de_vec4 ink = {.88f, .92f, .96f, 1};
typedef struct ui_scene {
    de_golden_ctx *ctx;
    de_task_system *tasks;
    de_type_registry *types;
    de_world *world;
    de_asset_system *assets;
    char asset_pack[256];
    de_renderer *renderer;
    de_text_system *text;
    de_ui *ui;
    de_font font;
    de_input *input;
    de_event_reader bindings;
    de_texture panel;
    de_load_handle loads[3];
    de_entity root;
    de_result error;
} ui_scene;
static de_component component(ui_scene *s, const char *name) {
    de_component c = de_component_find(s->world, name);
    if (!c.id)
        s->error = DE_FAIL(DE_ERR_NOT_FOUND, "UI golden component missing: %s", name);
    return c;
}
static void *add(ui_scene *s, de_entity entity, const char *name) {
    if (s->error != DE_OK)
        return NULL;
    void *p = de_entity_add(s->world, entity, component(s, name));
    if (!p)
        s->error = DE_ERR_INTERNAL;
    return p;
}
static de_entity node(ui_scene *s, de_entity parent, f32 x, f32 y, f32 width, f32 height,
                      de_vec4 color, f32 border, f32 radius) {
    de_entity e = de_entity_spawn(s->world);
    DeUiNode *n = add(s, e, "DeUiNode");
    if (!n)
        return e;
    n->width = (de_ui_len){DE_UI_PX, width};
    n->height = (de_ui_len){DE_UI_PX, height};
    n->shrink = 0;
    if (parent.bits) {
        s->error = de_entity_set_parent(s->world, e, parent);
        DeUiAnchor *a = add(s, e, "DeUiAnchor");
        if (a)
            *a = (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + width, y + height}};
    }
    DeUiStyle *style = add(s, e, "DeUiStyle");
    if (style)
        *style = (DeUiStyle){.background = color,
                             .border_color = ink,
                             .border_width = border,
                             .corner_radius = radius,
                             .opacity = 1,
                             .text_color = ink};
    return e;
}
static void label(ui_scene *s, de_entity parent, f32 x, f32 y, f32 width, f32 height,
                  const char *value, f32 size) {
    de_entity e = node(s, parent, x, y, width, height, (de_vec4){0}, 0, 0);
    const DeUiNode *parent_node = de_entity_get_const(s->world, parent, component(s, "DeUiNode"));
    if (parent_node)
        ((DeUiNode *)de_entity_get(s->world, e, component(s, "DeUiNode")))->z_order =
            parent_node->z_order;
    DeUiText *text = add(s, e, "DeUiText");
    if (text) {
        text->literal = de_str_from_cstr(value);
        text->style =
            (de_text_style){.font = s->font, .size_px = size, .color = ink, .language = "en"};
    }
}
static de_result pump(ui_scene *s) {
    de_asset_system_update(s->assets, s->world);
    de_text_update(s->text);
    DE_TRY(de_world_run_schedule(s->world, DE_SCHEDULE_POST_UPDATE, 1.f / 60, 0));
    const de_ui_event *ev;
    while ((ev = de_event_next(s->world, &s->bindings)))
        if (ev->kind == DE_UI_EV_BIND_ITEM) {
            DeUiText *text = de_entity_add(s->world, ev->target, component(s, "DeUiText"));
            snprintf(text->text, sizeof text->text, "Item %u", (u32)ev->value);
            text->style = (de_text_style){.font = s->font, .size_px = 20, .color = ink};
        }
    DE_TRY(de_world_run_schedule(s->world, DE_SCHEDULE_EXTRACT, 1.f / 60, 0));
    DE_TRY(de__render_extract_and_kick(s->renderer, s->world));
    de__render_world_wait_idle(s->renderer);
    return DE_OK;
}
static de_result setup(ui_scene *s, de_golden_ctx *ctx, f32 scale) {
    s->ctx = ctx;
    de_fp_env_init_thread();
    DE_TRY(de_fs_mount("uigolden", DE_GOLDEN_UI_ROOT, DE_MOUNT_READ_ONLY, 0));
    DE_TRY(de_fs_mount("uicache", DE_GOLDEN_UI_CACHE, DE_MOUNT_WRITABLE, 0));
    DE_TRY(de_task_system_create(&(de_task_system_desc){.worker_count = 2}, &s->tasks));
    DE_TRY(de_type_registry_create(NULL, &s->types));
    DE_TRY(de_world_create(&(de_world_desc){.types = s->types, .tasks = s->tasks}, &s->world));
    DE_TRY(de_asset_system_create(&(de_asset_system_desc){.mode = DE_GOLDEN_ASSET_MODE,
                                                          .tasks = s->tasks,
                                                          .types = s->types,
                                                          .cache_dir = "uicache:/decache"},
                                  &s->assets));
    DE_TRY(de_text_create(NULL, s->assets, &s->text));
    DE_TRY(de_renderer_create(
        &(de_renderer_desc){.device = ctx->device, .assets = s->assets, .types = s->types},
        &s->renderer));
    de_renderer_attach(s->renderer, s->world);
    DE_TRY(de_cvar_set(de_cvar_find("r.pipelined"), "false"));
    de_render_settings settings = de_render_settings_preset(DE_TIER_BASE, DE_QUALITY_LOW);
    settings.render_scale = 1;
    settings.ao = settings.reflections = settings.gi = settings.sss = settings.shadows =
        DE_QUALITY_OFF;
    de_renderer_set_settings(s->renderer, &settings);
    DE_TRY(de_text_attach(s->text, s->renderer, s->world));
    DE_TRY(de_asset_manifest_load(s->assets,
                                  "uigolden:/tests/golden/ui/widgets_advanced/assets.toml"));
    DE_TRY(de_golden_pack_assets(s->assets, "uigolden:/", s->asset_pack));
    const char *ids[] = {"ui/font", "ui/panel", "ui/cjk"};
    for (u32 i = 0; i < 3; ++i) {
        de_asset_id id = de_asset_id_from(ids[i]);
        s->loads[i] = de_asset_load(s->assets, id, 0);
        const u64 started = de_time_now_ns();
        de_load_state state = DE_LOAD_PENDING;
        while (de_time_now_ns() - started < 120ULL * 1000000000) {
            state = de_asset_wait(s->assets, s->loads[i], 20ULL * 1000000);
            if (state >= DE_LOAD_READY)
                break;
            /* Runtime texture readiness waits for the uploader's normal frame flush. */
            DE_TRY(pump(s));
        }
        if (state != DE_LOAD_READY) {
            de_load_info info = {0};
            (void)de_asset_status(s->assets, s->loads[i], &info);
            de_golden_fail(ctx, "UI golden asset %s: %s", ids[i], info.first_error_msg);
            return DE_ERR_IO;
        }
    }
    s->font = de_asset_font(s->assets, de_asset_id_from("ui/font"));
    s->panel = de_asset_texture(s->assets, de_asset_id_from("ui/panel"));
    de_font cjk = de_asset_font(s->assets, de_asset_id_from("ui/cjk"));
    de_text_set_fallbacks(s->text, s->font, &cjk, 1);
    s->input = input_test_create(NULL);
    if (!s->input)
        return DE_ERR_OUT_OF_MEMORY;
    DE_TRY(de_ui_create(&(de_ui_desc){.renderer = s->renderer,
                                      .input = s->input,
                                      .text = s->text,
                                      .assets = s->assets,
                                      .reference_height = UI_H / scale,
                                      .max_nodes = 256},
                        &s->ui));
    de_ui_set_viewport(s->ui, UI_W, UI_H, (de_window_insets){0});
    de_ui_attach(s->ui, s->world);
    s->bindings = de_event_reader_create(s->world, s->ui->event);
    de_builtin_component_ids builtins = de_builtin_components(s->world);
    de_component camera_components[] = {builtins.transform, component(s, "DeCamera")};
    de_entity camera;
    DE_TRY(de_entity_spawn_with(s->world, camera_components, 2, &camera));
    *(DeTransform *)de_entity_get(s->world, camera, builtins.transform) = de_transform_identity();
    ((DeCamera *)de_entity_get(s->world, camera, camera_components[1]))->exposure_ev100 = 0;
    s->root = node(s, (de_entity){0}, 0, 0, UI_W / scale, UI_H / scale, background, 0, 0);
    return s->error;
}
static void cleanup(ui_scene *s) {
    if (s->renderer)
        de__render_world_wait_idle(s->renderer);
    de_ui_destroy(s->ui);
    de_text_destroy(s->text);
    de_input_destroy(s->input);
    if (s->assets) {
        for (u32 i = 0; i < 3; ++i)
            if (s->loads[i].id)
                de_asset_release(s->assets, s->loads[i]);
        de_asset_system_destroy(s->assets);
    }
    if (s->renderer)
        de__render_world_destroy(s->renderer);
    de_world_destroy(s->world);
    de_renderer_destroy(s->renderer);
    de_type_registry_destroy(s->types);
    de_task_system_destroy(s->tasks);
    de_result packed_cleanup = de_golden_pack_cleanup(s->asset_pack);
    if (packed_cleanup != DE_OK)
        de_golden_fail(s->ctx, "golden pack cleanup: %s", de_result_str(packed_cleanup));
    de_fs_unmount("uicache", DE_GOLDEN_UI_CACHE);
    de_fs_unmount("uigolden", DE_GOLDEN_UI_ROOT);
}
static de_result render(ui_scene *s, f32 expected_scale) {
    DE_TRY(s->error);
    const u64 started = de_time_now_ns();
    for (u32 frame = 0;; ++frame) {
        DE_TRY(pump(s));
        /* Shaping can request an uncooked ligature. Let its real background glyph generation
         * and atlas upload finish before a reference is captured. */
        de_ui_golden_wait_status wait = de_ui_golden_glyph_wait(
            frame, de_text_get_stats(s->text).runtime_pending, de_time_now_ns() - started);
        if (wait == DE_UI_GOLDEN_READY)
            break;
        if (wait == DE_UI_GOLDEN_TIMEOUT)
            return DE_FAIL(DE_ERR_TIMEOUT, "UI golden runtime glyph generation did not settle");
    }
    if (fabsf(de_ui_get_layout_stats(s->ui).scale - expected_scale) > .001f)
        return DE_FAIL(DE_ERR_INTERNAL, "UI golden did not apply requested scale");
    de_ui_render_stats stats = de_ui_get_render_stats(s->ui);
    if (!stats.quads || !stats.draw_calls || stats.truncated)
        return DE_FAIL(DE_ERR_INTERNAL, "UI golden empty/truncated list: %u quads/%u draws/%u lost",
                       stats.quads, stats.draw_calls, stats.truncated);
    return DE_OK;
}
static de_result capture(ui_scene *s, u8 *pixels) {
    const u32 pitch = UI_W * 4;
    de_gpu_buffer rb =
        de_gpu_buffer_create(s->ctx->device, &(de_gpu_buffer_desc){.size = (u64)pitch * UI_H,
                                                                   .usage = DE_USAGE_COPY_DST,
                                                                   .memory = DE_MEM_READBACK});
    if (!rb.id)
        return DE_ERR_OUT_OF_MEMORY;
    de_gpu_cmd *cmd = de_gpu_cmd_begin(s->ctx->device, DE_QUEUE_GRAPHICS, "UI golden capture");
    de_gpu_cmd_copy_texture_to_buffer(cmd, rb, 0, pitch, de__renderer_offscreen(s->renderer), 0, 0);
    de_gpu_fence fence = de_gpu_fence_create(s->ctx->device, 0);
    de_gpu_submit submit = {.cmds = &cmd, .cmd_count = 1, .signal = fence, .signal_value = 1};
    de_result r = de_gpu_queue_submit(s->ctx->device, DE_QUEUE_GRAPHICS, &submit);
    if (r == DE_OK)
        r = de_gpu_fence_wait(s->ctx->device, fence, 1, 120ULL * 1000000000);
    if (r == DE_OK)
        memcpy(pixels, de_gpu_buffer_map(s->ctx->device, rb), (size_t)pitch * UI_H);
    de_gpu_fence_destroy(s->ctx->device, fence);
    de_gpu_buffer_destroy(s->ctx->device, rb);
    return r;
}
static de_entity widget(ui_scene *s, const char *name, f32 x, f32 y, f32 width, f32 height,
                        u32 state) {
    de_entity e = de_entity_spawn(s->world);
    DeUiNode *n = add(s, e, "DeUiNode");
    if (!n)
        return e;
    n->width = (de_ui_len){DE_UI_PX, width};
    n->height = (de_ui_len){DE_UI_PX, height};
    n->shrink = 0;
    s->error = de_entity_set_parent(s->world, e, s->root);
    DeUiAnchor *a = add(s, e, "DeUiAnchor");
    if (a)
        *a = (DeUiAnchor){.offset_min = {x, y}, .offset_max = {x + width, y + height}};
    if (!add(s, e, name))
        return e;
    DeUiInteractionState *interaction = add(s, e, "DeUiInteractionState");
    if (interaction) {
        interaction->hovered = state == 1;
        interaction->pressed = state == 2;
        interaction->disabled = state == 3;
    }
    if (!strcmp(name, "DeUiButton")) {
        DeUiButton *button = de_entity_get(s->world, e, component(s, "DeUiButton"));
        button->disabled = state == 3;
    }
    return e;
}
static void font(ui_scene *s, de_entity e, f32 size) {
    DeUiText *t = add(s, e, "DeUiText");
    if (t)
        t->style =
            (de_text_style){.font = s->font, .size_px = size, .color = ink, .line_height = 1};
}
static void content(ui_scene *s, bool editing) {
    label(s, s->root, 40, 24, 1160, 48, editing ? "Text editing and IME" : "Advanced game UI", 30);
    label(s, s->root, 40, 76, 1160, 30,
          editing ? "Selection / preedit underline / password / multiline"
                  : "Dropdown / virtual scrolling / tabs / tooltip",
          18);
    if (editing) {
        label(s, s->root, 40, 140, 1160, 28,
              "Committed buffer: Player   |   IME preedit: world (Japanese kanji)", 18);
        de_entity input = widget(s, "DeUiTextInput", 40, 184, 720, 56, 0);
        font(s, input, 28);
        DeUiTextInput *in = de_entity_get(s->world, input, component(s, "DeUiTextInput"));
        memcpy(in->buffer, "Player ", sizeof("Player "));
        in->length = in->caret = 7;
        de_world_run_schedule(s->world, DE_SCHEDULE_POST_UPDATE, .016f, 0);
        de_ui__input_submit(s->ui, input);
        de_ui_editor *state = de_entity_get(s->world, input, s->ui->advanced->editor);
        state->anchor = 0;
        input_test_text_edit(s->input, "世界", 2, 0);
        input_test_frame(s->input);
        de_world_run_schedule(s->world, DE_SCHEDULE_PRE_UPDATE, .016f, 0);
        label(s, s->root, 40, 292, 1160, 28,
              "Password uses grapheme bullets; committed text stays private", 18);
        de_entity password = widget(s, "DeUiTextInput", 40, 332, 480, 56, 0);
        font(s, password, 28);
        in = de_entity_get(s->world, password, component(s, "DeUiTextInput"));
        memcpy(in->buffer, "secret123", sizeof("secret123"));
        in->length = in->caret = 9;
        in->password = true;
        label(s, s->root, 40, 444, 1160, 28, "Multiline input wraps within the clipped control",
              18);
        de_entity multi = widget(s, "DeUiTextInput", 40, 484, 720, 132, 0);
        font(s, multi, 24);
        in = de_entity_get(s->world, multi, component(s, "DeUiTextInput"));
        memcpy(in->buffer, "Build a real game.\nStart small, then add a second level.",
               sizeof("Build a real game.\nStart small, then add a second level."));
        in->length = in->caret = (u32)strlen(in->buffer);
        in->multiline = true;
    } else {
        label(s, s->root, 40, 136, 360, 28, "Difficulty", 20);
        de_entity drop = widget(s, "DeUiDropdown", 40, 176, 280, 48, 0);
        font(s, drop, 22);
        DeUiDropdown *d = de_entity_get(s->world, drop, component(s, "DeUiDropdown"));
        d->option_count = 3;
        d->selected = 1;
        d->options[0] = de_name_from("easy");
        d->options[1] = de_name_from("normal");
        d->options[2] = de_name_from("hard");
        label(s, s->root, 440, 136, 360, 28, "100,000 items / seven visible rows", 20);
        de_entity list = widget(s, "DeUiList", 440, 176, 360, 240, 0);
        DeUiList *l = de_entity_get(s->world, list, component(s, "DeUiList"));
        l->item_count = 100000;
        l->item_height = 36;
        label(s, s->root, 40, 444, 360, 28, "Tabs keep one page active", 20);
        de_entity tabs = widget(s, "DeUiTabs", 40, 484, 760, 132, 0);
        for (u32 i = 0; i < 2; ++i) {
            de_entity page = node(s, tabs, 0, 0, 760, 90, (de_vec4){.05f, .08f, .12f, 1}, 0, 0);
            font(s, page, 20);
            DeUiText *t = de_entity_get(s->world, page, component(s, "DeUiText"));
            snprintf(t->text, sizeof t->text, "%s", i ? "Audio" : "Video");
        }
        de_entity help = widget(s, "DeUiButton", 920, 400, 240, 48, 1);
        font(s, help, 20);
        DeUiText *t = de_entity_get(s->world, help, component(s, "DeUiText"));
        memcpy(t->text, "Hover for help", sizeof("Hover for help"));
        DeUiTooltip *tip = add(s, help, "DeUiTooltip");
        tip->loc_id = de_name_from("help");
        tip->delay = .05f;
        for (u32 i = 0; i < 4; ++i)
            de_world_run_schedule(s->world, DE_SCHEDULE_POST_UPDATE, .016f, 0);
        de_ui_scroll_offset(s->ui, list, (de_vec2){0, 3600});
        de_ui__dropdown_submit(s->ui, drop);
    }
}
static void output(de_golden_ctx *ctx, const u8 *pixels) {
    u32 bright = 0;
    for (u64 i = 0; i < (u64)UI_W * UI_H * 4; i += 4)
        if ((u32)pixels[i] + pixels[i + 1] + pixels[i + 2] > 600)
            ++bright;
    if (bright < 800)
        de_golden_fail(ctx, "default widget labels missing: %u bright pixels", bright);
    de_gpu_buffer upload = de_golden_upload_buffer(ctx, pixels, (u64)UI_W * UI_H * 4,
                                                   DE_USAGE_COPY_SRC, "default widget sheet");
    de_golden_target_barrier(ctx, DE_ACCESS_COPY_DST);
    de_gpu_cmd_copy_buffer_to_texture(ctx->cmd, ctx->target, 0, 0, upload, 0, UI_W * 4);
}
static void scene(de_golden_ctx *ctx, bool editing) {
    if (ctx->width != UI_W || ctx->height != UI_H) {
        de_golden_fail(ctx, "widget sheet requires1280x720");
        return;
    }
    ui_scene s = {0};
    const de_allocator *allocator = de_heap_allocator();
    u8 *pixels = DE_ALLOC(allocator, (u64)UI_W * UI_H * 4, 16, DE_MEM_GENERAL);
    if (!pixels) {
        de_golden_fail(ctx, "widget capture allocation failed");
        return;
    }
    de_result result = setup(&s, ctx, 1);
    if (result == DE_OK) {
        content(&s, editing);
        result = render(&s, 1);
    }
    if (result == DE_OK)
        result = capture(&s, pixels);
    if (result == DE_OK)
        output(ctx, pixels);
    else
        de_golden_fail(ctx, "default widget scene: %s", de_result_str(result));
    cleanup(&s);
    DE_FREE(allocator, pixels, (u64)UI_W * UI_H * 4);
}
DE_GOLDEN_SCENE(ui_widgets_advanced_text) {
    scene(ctx, true);
}
DE_GOLDEN_SCENE(ui_widgets_advanced_controls) {
    scene(ctx, false);
}

#else
DE_GOLDEN_SCENE(ui_widgets_advanced_text) {
    de_golden_skip(ctx, "widgets require UI/render and source cooking");
}
DE_GOLDEN_SCENE(ui_widgets_advanced_controls) {
    de_golden_skip(ctx, "requires UI/render cooking");
}
#endif
