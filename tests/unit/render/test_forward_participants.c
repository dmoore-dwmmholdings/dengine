/* Public forward-participant contract; fixture internals only drive/read real renderer frames. */
#include "pixel_fixture.h"
#include "render/passes/forward/forward.h"
#include "render/passes/forward/forward_participants.h"
#include "cook/importers/shader/slang_compile.h"

static u32 empty_gather(const de_forward_context *c, de_forward_item *out, u32 cap, void *user) {
    (void)c;
    (void)out;
    (void)cap;
    (void)user;
    return 0;
}
static de_result empty_build(de_rg *g, const de_forward_context *c, const de_forward_item *item,
                             void *user) {
    (void)g;
    (void)c;
    (void)item;
    (void)user;
    return DE_OK;
}
TEST_F(forward_participants, registry_bounds_and_removal, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 480, 270, 1920, 1080, false);
    de_forward_participant_desc desc = {
        .max_items = 1, .gather = empty_gather, .build = empty_build};
    EXPECT_EQ(de_render_add_forward_participant(NULL, &desc), DE_ERR_INVALID_ARGUMENT);
    desc.max_items = 0;
    EXPECT_EQ(de_render_add_forward_participant(f->r, &desc), DE_ERR_INVALID_ARGUMENT);
    desc.max_items = 65536;
    EXPECT_EQ(de_render_add_forward_participant(f->r, &desc), DE_OK);
    EXPECT_EQ(de_render_add_forward_participant(f->r, &desc), DE_ERR_INVALID_ARGUMENT);
    desc.user = f;
    EXPECT_EQ(de_render_add_forward_participant(f->r, &desc), DE_ERR_OUT_OF_MEMORY);
    EXPECT_EQ(de_render_remove_forward_participant(f->r, empty_gather, NULL), DE_OK);
    EXPECT_EQ(de_render_remove_forward_participant(f->r, empty_gather, NULL), DE_ERR_NOT_FOUND);
    desc.max_items = 1;
    for (u32 i = 0; i < 16; ++i) {
        desc.user = (void *)(uintptr_t)(i + 1);
        EXPECT_EQ(de_render_add_forward_participant(f->r, &desc), DE_OK);
    }
    desc.user = (void *)(uintptr_t)17;
    EXPECT_EQ(de_render_add_forward_participant(f->r, &desc), DE_ERR_OUT_OF_MEMORY);
    for (u32 i = 0; i < 16; ++i)
        EXPECT_EQ(
            de_render_remove_forward_participant(f->r, empty_gather, (void *)(uintptr_t)(i + 1)),
            DE_OK);
}
TEST(forward_participants, shared_sort_ties_layers_overlay) {
    de__transparent_item items[] = {{.entity = 30, .depth = 4, .index = 0},
                                    {.entity = 10, .depth = 6, .index = 1, .participant = 1},
                                    {.entity = 20, .depth = 2, .index = 2},
                                    {.entity = 10, .depth = 6, .index = 3, .participant = 2},
                                    {.entity = 1, .depth = 100, .index = 4, .always_on_top = true},
                                    {.entity = 99, .depth = 1, .index = 5, .layer = -1}},
                         scratch[6];
    de__transparent_sort(items, 6, scratch);
    const u32 order[] = {5, 1, 3, 0, 2, 4};
    for (u32 i = 0; i < 6; ++i)
        EXPECT_EQ(items[i].index, order[i]);
    for (u32 i = 1; i < 6; ++i)
        EXPECT_FALSE(de__transparent_before(&items[i], &items[i - 1]));
}
typedef struct metadata_observer {
    pixel_fixture *f;
    de_view_frame_info seen[2];
    u32 sequence[2], serial;
} metadata_observer;
static void pixel_hook(de_rg *g, de_view v, void *user) {
    (void)g;
    metadata_observer *m = user;
    ASSERT_TRUE(de_view_frame_info_get(m->f->r, v, &m->seen[0]));
    u32 w, h;
    de_view_size(m->f->r, v, &w, &h);
    ASSERT_EQ(w, 480u);
    ASSERT_EQ(h, 270u);
    m->sequence[0] = ++m->serial;
}
static void output_hook(de_rg *g, de_view v, void *user) {
    (void)g;
    metadata_observer *m = user;
    ASSERT_TRUE(de_view_frame_info_get(m->f->r, v, &m->seen[1]));
    u32 w, h;
    de_view_size(m->f->r, v, &w, &h);
    ASSERT_EQ(w, 1920u);
    ASSERT_EQ(h, 1080u);
    m->sequence[1] = ++m->serial;
}
static void observe_metadata(pixel_fixture *f, bool native) {
    start_pixel(f, native ? DE_GPU_METAL : DE_GPU_NULL, 480, 270, 1920, 1080, false);
    metadata_observer m = {.f = f};
    de_render_add_hook(f->r, DE_HOOK_PIXEL_UI, pixel_hook, &m);
    de_render_add_hook(f->r, DE_HOOK_UI, output_hook, &m);
    make_pattern(f);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    DeTransform *cam = de_entity_get(f->world, f->camera, bi.transform);
    cam->position = (de_dvec3){100000, 100000, 0};
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_POST_UPDATE, .016f, 0), DE_OK);
    ASSERT_EQ(de_world_run_schedule(f->world, DE_SCHEDULE_EXTRACT, .016f, 0), DE_OK);
    de_view_frame_info frozen;
    ASSERT_TRUE(de_renderer_extract_view_info(f->r, f->view, &frozen));
    ASSERT_EQ(frozen.frame_index, UINT64_MAX);
    ASSERT_TRUE(fabs(frozen.render_origin.x - 100000) < 1024);
    /* Mutate next-frame config after Extract: this submitted frame must retain N's grid. */
    de_pixel_art_settings next = {
        .internal_width = 320, .internal_height = 180, .integer_scale_only = true};
    de_view_set_pixel_art(f->r, f->view, &next);
    ASSERT_EQ(de__render_extract_and_kick(f->r, f->world), DE_OK);
    de_gpu_wait_idle(f->d);
    EXPECT_EQ(m.seen[0].extract_frame, frozen.extract_frame);
    EXPECT_EQ(m.seen[1].internal_width, 480u);
    EXPECT_EQ(m.seen[0].pixel.internal_width, 480u);
    EXPECT_TRUE(memcmp(&m.seen[0].view_projection, &frozen.view_projection, sizeof(de_mat4)) == 0);
    EXPECT_TRUE(m.seen[0].content_px.z == 1920 && m.seen[0].content_px.w == 1080);
    EXPECT_TRUE(m.sequence[0] < m.sequence[1]);
    ASSERT_EQ(de_render_remove_hook(f->r, DE_HOOK_PIXEL_UI, pixel_hook, &m), DE_OK);
    ASSERT_EQ(de_render_remove_hook(f->r, DE_HOOK_UI, output_hook, &m), DE_OK);
    if (native) {
        /* Exact same integer upscale path as the scene; every output pixel is checked. */
        const u8 *data = read_output(f);
        u32 bad = 0;
        for (u32 y = 0; y < 1080; ++y)
            for (u32 x = 0; x < 1920; ++x) {
                const u8 *p = data + ((size_t)y * 1920 + x) * 4;
                u32 ix = x / 4, iy = y / 4;
                bad += p[0] != (u8)ix || p[1] != (u8)iy || p[2] != (((ix + iy) & 1) ? 255 : 0);
            }
        printf("Pixel hook exact4x mismatches: %u\n", bad);
        EXPECT_EQ(bad, 0u);
    }
}
TEST_F(forward_participants, extract_freezes_grid_origin_and_hook_order, pixel_fixture) {
    observe_metadata(f, false);
}
TEST_F(forward_participants, pixel_hook_native_exact_four, pixel_fixture) {
    observe_metadata(f, true);
}

extern const de__slang_module de__shader_common_modules[];
extern const u32 de__shader_common_module_count;
typedef struct import_files {
    void *data[1024];
    u32 count;
} import_files;
static de_result participant_read(void *user, const char *path, const void **data, u64 *size) {
    import_files *files = user;
    if (files->count == 1024) {
        fprintf(stderr, "import capacity %s\n", path);
        return DE_ERR_OUT_OF_MEMORY;
    }
    char full[2048];
    int n = snprintf(full, sizeof full, "%s/%s", DE_PARTICIPANT_SHADER_ROOT, path);
    if (n < 0 || (size_t)n >= sizeof full)
        return DE_ERR_NOT_FOUND;
    FILE *file = fopen(full, "rb");
    if (!file)
        return DE_ERR_NOT_FOUND;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return DE_ERR_IO;
    }
    long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return DE_ERR_IO;
    }
    char *bytes = malloc((size_t)length + 1);
    if (!bytes) {
        fclose(file);
        return DE_ERR_OUT_OF_MEMORY;
    }
    size_t got = fread(bytes, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(bytes);
        return DE_ERR_IO;
    }
    bytes[length] = 0;
    files->data[files->count++] = bytes;
    *data = bytes;
    *size = (u64)length;
    return DE_OK;
}
static const char participant_shader[] =
    "import bindless; import \"scene/scene_types\"; import \"lighting/lighting_common\";"
    "import \"materials/standard/standard\";"
    "struct P { uint scene,slot,lv,entry; DeClusterLists cl; float4 center; float4 color;"
    "uint lit; uint3 pad; };"
    "struct V { float4 pos:SV_Position; float3 world:TEXCOORD0; };"
    "[shader(\"vertex\")] V vs_panel(uint id:SV_VertexID,uniform P p) {"
    "float2 v[6]={float2(-1,-1),float2(1,-1),float2(1,1),float2(-1,-1),float2(1,1),float2(-1,1)};"
    "V o;o.world=p.center.xyz+float3(v[id]*p.center.w,0);"
    "DeSceneConstants s=de_robuffer<DeSceneConstants>(p.scene)[p.slot];float4 w=float4(o.world,1);"
    "o.pos=float4(dot(s.view_proj[0],w),dot(s.view_proj[1],w),dot(s.view_proj[2],w),dot(s.view_"
    "proj[3],w));return o;}"
    "[shader(\"fragment\")] float4 ps_panel(V i,uniform P p):SV_Target {"
    "DeLightingView l=de_robuffer<DeLightingView>(p.lv)[p.entry];float3 rgb=p.color.rgb;"
    "if(p.lit!=0) { DeSceneConstants sc=de_robuffer<DeSceneConstants>(p.scene)[p.slot];"
    "SurfaceData "
    "s=de_surface_default();s.base_color=rgb;s.specular=0;s.normal=float3(0,0,1);s.brdf_lut=l.brdf_"
    "lut;"
    "float3 v=normalize(sc.camera_position.xyz-i.world);uint2 px=uint2(i.pos.xy);"
    "float depth=-dot(sc.view[2],float4(i.world,1));uint c=de_cluster_index(l,px,depth);"
    "DeDirect d=evaluate_direct_clustered<StandardModel>(s,i.world,v,px,l,p.cl,c,false);"
    "rgb=d.diffuse+d.specular; } return float4(rgb*l.pre_exposure*p.color.a,p.color.a); }";
typedef struct participant_push {
    de_forward_shader_bindings bindings;
    de_vec4 center, color;
    u32 lit, pad[3];
} participant_push;
_Static_assert(sizeof(participant_push) == 96, "test shader push ABI");
typedef struct participant_state {
    pixel_fixture *f;
    de_gpu_pipeline pipeline[2];
    de_gpu_buffer readback;
    const de_forward_context *context;
    de_vec4 center, color;
    u32 lit, gathers, builds;
    bool enabled, depth;
    struct panel_pass {
        struct participant_state *owner;
        const de_forward_context *context;
        participant_push push;
        bool depth;
    } passes[3];
    de_view_frame_info seen;
} participant_state;
static void participant_pipeline(participant_state *s) {
    import_files files = {0};
    char source[4096];
    size_t source_size = 0;
    for (size_t i = 0; i < sizeof participant_shader - 1; ++i) {
        source[source_size++] = participant_shader[i];
        if (participant_shader[i] == ';')
            source[source_size++] = '\n';
    }
    source[source_size] = 0;
    const de__slang_entry entries[] = {{"vs_panel", 0}, {"ps_panel", 0}};
    de__slang_compile_result result = {0};
    de_result r =
        de__slang_compile(&(de__slang_compile_desc){.target = DE__SLANG_METALLIB,
                                                    .path = "participant_test.slang",
                                                    .source = source,
                                                    .source_size = source_size,
                                                    .modules = de__shader_common_modules,
                                                    .module_count = de__shader_common_module_count,
                                                    .preload_count = 1,
                                                    .read = participant_read,
                                                    .read_user = &files,
                                                    .entries = entries,
                                                    .entry_count = 2,
                                                    .allocator = de_heap_allocator()},
                          &result);
    for (u32 i = 0; i < files.count; ++i)
        free(files.data[i]);
    if (r != DE_OK)
        fprintf(stderr, "participant shader: %s\n", result.diagnostics);
    ASSERT_EQ(r, DE_OK);
    for (u32 i = 0; i < 2; ++i) {
        s->pipeline[i] = de_gpu_pipeline_create(
            s->f->d, &(de_gpu_pipeline_desc){
                         .kind = DE_PIPE_GRAPHICS,
                         .vs = {result.entries[0].code, result.entries[0].code_size, "vs_panel"},
                         .ps = {result.entries[1].code, result.entries[1].code_size, "ps_panel"},
                         .color_formats = {DE_FMT_RGBA16_FLOAT},
                         .color_count = 1,
                         .blend = {DE_BLEND_PREMUL},
                         .depth_format = i ? DE_FMT_D32_FLOAT : DE_FMT_UNKNOWN,
                         .depth_test = i != 0,
                         .depth_write = false,
                         .cull = DE_CULL_NONE,
                         .push_constant_bytes = result.entries[0].push_constant_bytes,
                         .name = "public forward participant diagnostic"});
        ASSERT_TRUE(s->pipeline[i].id);
    }
    de__slang_compile_result_free(&result);
}
static u32 panel_gather(const de_forward_context *c, de_forward_item *out, u32 cap, void *user) {
    participant_state *s = user;
    ++s->gathers;
    ASSERT_TRUE(de_render_forward_view_info(c, &s->seen));
    s->context = c;
    if (!s->enabled)
        return 0;
    ASSERT_TRUE(cap >= 1);
    out[0] = (de_forward_item){.center_relative = {s->center.x, s->center.y, s->center.z},
                               .entity = 9999,
                               .item_index = 42,
                               .always_on_top = !s->depth};
    return 1;
}
static void panel_execute(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    struct panel_pass *p = user;
    participant_state *s = p->owner;
    de_gpu_attachment color = {.texture = de_rg_texture(pass, de_render_forward_color(p->context)),
                               .store = true};
    de_gpu_attachment depth = {
        .texture = p->depth ? de_rg_texture(pass, de_render_forward_depth(p->context))
                            : (de_gpu_texture){0},
        .store = true};
    ASSERT_TRUE(de_render_forward_bindings(p->context, pass, &p->push.bindings));
    ASSERT_TRUE(p->push.bindings.lighting_constants && p->push.bindings.lights &&
                p->push.bindings.grid);
    de_gpu_cmd_begin_render(cmd, &color, 1, p->depth ? &depth : NULL);
    de_gpu_cmd_set_pipeline(cmd, s->pipeline[p->depth ? 1 : 0]);
    de_gpu_cmd_viewport(cmd, 0, 0, (f32)s->f->iw, (f32)s->f->ih);
    de_gpu_cmd_scissor(cmd, 0, 0, s->f->iw, s->f->ih);
    de_gpu_cmd_push_constants(cmd, &p->push, sizeof p->push);
    de_gpu_cmd_draw(cmd, 6, 1, 0, 0);
    de_gpu_cmd_end_render(cmd);
}
static de_result panel_build(de_rg *g, const de_forward_context *c, const de_forward_item *item,
                             void *user) {
    participant_state *s = user;
    ++s->builds;
    ASSERT_EQ(item->item_index, 42u);
    de_view_frame_info info;
    ASSERT_TRUE(de_render_forward_view_info(c, &info));
    ASSERT_TRUE(info.ring_index < 3);
    struct panel_pass *p = &s->passes[info.ring_index];
    *p = (struct panel_pass){.owner = s,
                             .context = c,
                             .depth = s->depth,
                             .push = {.center = s->center, .color = s->color, .lit = s->lit}};
    de_rg_pass *pass = de_rg_add_pass(g, "test.forward.panel", DE_QUEUE_GRAPHICS, panel_execute, p);
    if (!pass)
        return DE_ERR_OUT_OF_MEMORY;
    de_render_forward_declare(pass, c, s->depth);
    return DE_OK;
}
static void panel_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    participant_state *s = user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, s->readback, 0, s->f->iw * 8,
                                      de_rg_texture(pass, de_render_forward_color(s->context)), 0,
                                      0);
}
static void panel_capture(de_rg *g, de_view v, void *user) {
    participant_state *s = user;
    (void)v;
    ASSERT_TRUE(s->context);
    de_rg_pass *p = de_rg_add_pass(g, "test.forward.capture", DE_QUEUE_GRAPHICS, panel_copy, s);
    de_rg_read(p, de_render_forward_color(s->context), DE_ACCESS_COPY_SRC);
    de_rg_write(p, de_rg_import_buffer(g, s->readback, DE_ACCESS_NONE), DE_ACCESS_COPY_DST);
    de_rg_pass_side_effect(p);
}
static f32 participant_half(u16 h) {
    u32 sign = (u32)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, bits;
    if (!e)
        return (h & 32768 ? -1.f : 1.f) * (f32)m / 16777216.f;
    bits = sign | ((e + 112) << 23) | (m << 13);
    f32 f;
    memcpy(&f, &bits, 4);
    return f;
}
static de_vec4 panel_pixel(participant_state *s) {
    pixel_frame(s->f);
    pixel_frame(s->f);
    const u16 *p = de_gpu_buffer_map(s->f->d, s->readback);
    p += ((size_t)(s->f->ih / 2) * s->f->iw + s->f->iw / 2) * 4;
    return (de_vec4){participant_half(p[0]), participant_half(p[1]), participant_half(p[2]),
                     participant_half(p[3])};
}
static void panel_start(participant_state *s, pixel_fixture *f) {
    start_pixel(f, DE_GPU_METAL, 480, 270, 1920, 1080, false);
    de__view_unregister_stage(f->r, pattern_stage, f);
    de_render_set_2d_plane(f->r, DE_PLANE_TOP_XY);
    de_entity_despawn(f->world, f->sprite);
    *s = (participant_state){
        .f = f, .center = {0, 0, -6, 50}, .color = {0, 1, 0, .5f}, .enabled = true, .depth = true};
    participant_pipeline(s);
    s->readback =
        de_gpu_buffer_create(f->d, &(de_gpu_buffer_desc){.size = 480 * 270 * 8,
                                                         .usage = DE_USAGE_COPY_DST,
                                                         .memory = DE_MEM_READBACK,
                                                         .name = "participant HDR readback"});
    ASSERT_TRUE(s->readback.id);
    const de_forward_participant_desc desc = {
        .max_items = 1, .gather = panel_gather, .build = panel_build, .user = s};
    ASSERT_EQ(de_render_add_forward_participant(f->r, &desc), DE_OK);
    de_render_add_hook(f->r, DE_HOOK_BEFORE_TONEMAP, panel_capture, s);
}
static void panel_stop(participant_state *s) {
    ASSERT_EQ(de_render_remove_hook(s->f->r, DE_HOOK_BEFORE_TONEMAP, panel_capture, s), DE_OK);
    ASSERT_EQ(de_render_remove_forward_participant(s->f->r, panel_gather, s), DE_OK);
    u32 builds = s->builds;
    pixel_frame(s->f);
    EXPECT_EQ(s->builds, builds);
    de_gpu_pipeline_destroy(s->f->d, s->pipeline[0]);
    de_gpu_pipeline_destroy(s->f->d, s->pipeline[1]);
    de_gpu_buffer_destroy(s->f->d, s->readback);
}
static de_entity panel_sprite(pixel_fixture *f, f32 z, de_vec4 color, bool translucent) {
    const DeRenderComponents *ids = de__render_components(f->world);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    de_component components[] = {bi.transform, ids->sprite};
    de_entity e;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &e), DE_OK);
    DeTransform t = de_transform_identity();
    t.position.z = z;
    *(DeTransform *)de_entity_get(f->world, e, bi.transform) = t;
    *(DeSprite *)de_entity_get(f->world, e, ids->sprite) = (DeSprite){.size = {100, 100},
                                                                      .pivot = {.5f, .5f},
                                                                      .uv_rect = {0, 0, 1, 1},
                                                                      .color = color,
                                                                      .translucent = translucent};
    return e;
}
TEST_F(forward_participants, native_empty_scene_real_clusters_lit_unlit_and_depth, pixel_fixture) {
    participant_state s;
    panel_start(&s, f);
    s.color = (de_vec4){1, 1, 1, 1};
    de_vec4 unlit = panel_pixel(&s);
    ASSERT_TRUE(unlit.x > .5f && unlit.y > .5f);
    const DeRenderComponents *ids = de__render_components(f->world);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    de_component components[] = {bi.transform, ids->light};
    de_entity light;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &light), DE_OK);
    DeTransform t = de_transform_identity();
    t.position.z = -3;
    *(DeTransform *)de_entity_get(f->world, light, bi.transform) = t;
    *(DeLight *)de_entity_get(f->world, light, ids->light) =
        (DeLight){.kind = DE_LIGHT_POINT, .color = {1, .1f, .02f}, .intensity = 3000};
    s.lit = 1;
    de_vec4 lit = panel_pixel(&s);
    printf("Participant-only point-lit %.5f %.5f %.5f; unlit %.5f\n", lit.x, lit.y, lit.z, unlit.x);
    ASSERT_TRUE(lit.x > lit.y * 4 && lit.y > lit.z * 2 && lit.x > .1f);
    s.lit = 0;
    s.color = (de_vec4){0, 1, 0, 1};
    (void)panel_sprite(f, -4, (de_vec4){1, 0, 0, 1}, false);
    s.enabled = false;
    de_vec4 wall = panel_pixel(&s);
    s.enabled = true;
    de_vec4 occluded = panel_pixel(&s);
    printf("Depth panel %.5f %.5f %.5f wall %.5f %.5f %.5f\n", occluded.x, occluded.y, occluded.z,
           wall.x, wall.y, wall.z);
    for (u32 i = 0; i < 3; ++i)
        ASSERT_TRUE(fabsf((&occluded.x)[i] - (&wall.x)[i]) < .004f);
    s.depth = false;
    de_vec4 overlay = panel_pixel(&s);
    ASSERT_TRUE(overlay.y > .5f && overlay.x < .01f);
    panel_stop(&s);
}
TEST_F(forward_participants, native_shared_mesh_panel_mesh_blend, pixel_fixture) {
    participant_state s;
    panel_start(&s, f);
    const DeRenderComponents *ids = de__render_components(f->world);
    const de_builtin_component_ids bi = de_builtin_components(f->world);
    de_component components[] = {bi.transform, ids->light};
    de_entity light;
    ASSERT_EQ(de_entity_spawn_with(f->world, components, 2, &light), DE_OK);
    DeTransform t = de_transform_identity();
    t.position.z = -2;
    *(DeTransform *)de_entity_get(f->world, light, bi.transform) = t;
    *(DeLight *)de_entity_get(f->world, light, ids->light) =
        (DeLight){.kind = DE_LIGHT_POINT, .color = {1, 1, 1}, .intensity = 3000};
    /* Measure each lit mesh's own radiance before constructing the independent blend oracle. */
    s.enabled = false;
    de_entity back = panel_sprite(f, -8, (de_vec4){1, 0, 0, .5f}, true);
    de_vec4 back_only = panel_pixel(&s);
    de_entity_despawn(f->world, back);
    de_entity front = panel_sprite(f, -4, (de_vec4){0, 0, 1, .5f}, true);
    de_vec4 front_only = panel_pixel(&s);
    de_entity_despawn(f->world, front);
    s.enabled = true;
    de_vec4 middle = panel_pixel(&s);
    (void)panel_sprite(f, -8, (de_vec4){1, 0, 0, .5f}, true);
    (void)panel_sprite(f, -4, (de_vec4){0, 0, 1, .5f}, true);
    de_vec4 got = panel_pixel(&s);
    de_vec4 want = {back_only.x * .25f + middle.x * .5f + front_only.x,
                    back_only.y * .25f + middle.y * .5f + front_only.y,
                    back_only.z * .25f + middle.z * .5f + front_only.z, 0};
    printf("mesh/panel/mesh got %.5f %.5f %.5f oracle %.5f %.5f %.5f\n", got.x, got.y, got.z,
           want.x, want.y, want.z);
    for (u32 i = 0; i < 3; ++i)
        ASSERT_TRUE(fabsf((&got.x)[i] - (&want.x)[i]) < .004f);
    ASSERT_TRUE(middle.y > .1f && back_only.x > back_only.z && front_only.z > front_only.x);
    panel_stop(&s);
}

static u32 invalid_gather(const de_forward_context *c, de_forward_item *out, u32 cap, void *user) {
    (void)c;
    if (user) {
        out[0].center_relative.x = NAN;
        return 1;
    }
    return cap + 1;
}
TEST_F(forward_participants, gathered_overflow_and_nonfinite_refused, pixel_fixture) {
    start_pixel(f, DE_GPU_NULL, 480, 270, 1920, 1080, false);
    de_arena *arena = NULL;
    ASSERT_EQ(de_arena_create(&(de_arena_desc){.name = "participant bounds"}, &arena), DE_OK);
    de__vis_context vc = {.arena = arena};
    de_forward_context c = {.renderer = f->r, .vis = &vc};
    EXPECT_TRUE(de_render_forward_alloc(NULL, 64, 16) == NULL);
    EXPECT_TRUE(de_render_forward_alloc(&c, 0, 16) == NULL);
    EXPECT_TRUE(de_render_forward_alloc(&c, 64, 3) == NULL);
    EXPECT_TRUE(de_render_forward_alloc(&c, SIZE_MAX, 16) == NULL);
    void *storage = de_render_forward_alloc(&c, 64, 64);
    ASSERT_TRUE(storage != NULL);
    EXPECT_EQ((uintptr_t)storage % 64, 0u);
    memset(storage, 0, 64);
    de_forward_participant_desc desc = {
        .max_items = 1, .gather = invalid_gather, .build = empty_build};
    ASSERT_EQ(de_render_add_forward_participant(f->r, &desc), DE_OK);
    de__transparent_item *items = NULL;
    u32 count = 0;
    EXPECT_EQ(de__forward_collect(&c, &items, &count), DE_ERR_INVALID_ARGUMENT);
    EXPECT_EQ(count, 0u);
    ASSERT_EQ(de_render_remove_forward_participant(f->r, invalid_gather, NULL), DE_OK);
    desc.user = f;
    ASSERT_EQ(de_render_add_forward_participant(f->r, &desc), DE_OK);
    EXPECT_EQ(de__forward_collect(&c, &items, &count), DE_ERR_INVALID_ARGUMENT);
    ASSERT_EQ(de_render_remove_forward_participant(f->r, invalid_gather, f), DE_OK);
    de_arena_destroy(arena);
}
