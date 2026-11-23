#include "de_test.h"
#if DE_ED_MATERIAL_GRAPH
#include "cook/importers/shader/slang_compile.h"
#include "host/editor/editor_internal.h"
#include "host/host_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPECT_NEAR(a, b, eps) EXPECT_TRUE(fabsf((float)(a) - (float)(b)) <= (eps))

/* DENG-769: the material graph over the editor IPC (DIDE-R161; protocol 1.10, capability
 * assets.materialGraph). Each test encodes params with the generated protocol writers, runs the
 * registered handler and decodes the reply with the generated readers: a full round trip with no
 * transport. */

#define REPLY_CAP ((size_t)8 << 20)
#define ARENA_CAP ((size_t)4 << 20)

typedef struct mg_fx {
    de__editor ed;
    unsigned char *arena;   /* handler arena */
    unsigned char *decode;  /* arena for the decoded reply */
    char *params;
    char *reply;
    size_t params_size;
    size_t reply_size;
    uint32_t reply_left; /* reply map entries after `result`, for close_reply() */
} mg_fx;

static void fx_setup(mg_fx *f) {
    memset(f, 0, sizeof *f);
    f->arena = malloc(ARENA_CAP);
    f->decode = malloc(ARENA_CAP);
    f->params = malloc(REPLY_CAP);
    f->reply = malloc(REPLY_CAP);
    ASSERT_TRUE(f->arena && f->decode && f->params && f->reply);
    f->ed.h = calloc(1, sizeof *f->ed.h);
    f->ed.e = calloc(1, sizeof *f->ed.e);
    ASSERT_TRUE(f->ed.h && f->ed.e);
    de__ed_material_graph_register(&f->ed);
}
static void fx_teardown(mg_fx *f) {
    free(f->arena);
    free(f->decode);
    free(f->params);
    free(f->reply);
    free(f->ed.h);
    free(f->ed.e);
}

static void run(mg_fx *f, de_ep_method method) {
    static const char empty_map[] = "\x80";
    const de_ep_request req = {.id = 769,
                               .method = de_ep_cstr(de_ep_method_name(method)),
                               .params = f->params_size ? (de_ep_raw){f->params, f->params_size}
                                                        : (de_ep_raw){empty_map, 1}};
    de_ep_arena arena = {f->arena, ARENA_CAP, 0};
    mpack_writer_t w;
    mpack_writer_init(&w, f->reply, REPLY_CAP);
    ASSERT_TRUE(f->ed.handlers[method] != NULL);
    f->ed.handlers[method](&f->ed, &req, &arena, &w);
    f->reply_size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
}

static void encode_generate(mg_fx *f, const char *graph, size_t len, const char *name) {
    de_ep_material_graph_params p;
    memset(&p, 0, sizeof p);
    p.graph = (de_ep_str){graph, (uint32_t)len};
    if (name) {
        p.has_name = true;
        p.name = de_ep_cstr(name);
    }
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, REPLY_CAP);
    de_ep_write_material_graph_params(&w, &p);
    f->params_size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
}

static void encode_compile(mg_fx *f, const char *graph, const char *const *targets, u32 count) {
    de_ep_str items[8];
    de_ep_material_graph_compile_params p;
    memset(&p, 0, sizeof p);
    p.graph = de_ep_cstr(graph);
    for (u32 i = 0; i < count; ++i)
        items[i] = de_ep_cstr(targets[i]);
    p.targets.items = items;
    p.targets.count = count;
    mpack_writer_t w;
    mpack_writer_init(&w, f->params, REPLY_CAP);
    de_ep_write_material_graph_compile_params(&w, &p);
    f->params_size = mpack_writer_buffer_used(&w);
    ASSERT_EQ(mpack_writer_destroy(&w), mpack_ok);
}

/* Opens the reply and positions `r` on the result value. Returns the error code, or 0 for a
 * result (then the caller reads the value and calls close_reply()). DENG-805: every map is
 * closed before destroy, so the helpers pass under MPACK_DEBUG read tracking. */
static int open_reply(mg_fx *f, mpack_reader_t *r) {
    mpack_reader_init_data(r, f->reply, f->reply_size);
    const uint32_t n = mpack_expect_map(r);
    int code = -1;
    for (uint32_t i = 0; i < n && mpack_reader_error(r) == mpack_ok; ++i) {
        char key[16];
        mpack_expect_cstr(r, key, sizeof key);
        if (strcmp(key, "id") == 0) {
            EXPECT_EQ(mpack_expect_u64(r), 769u);
        } else if (strcmp(key, "result") == 0) {
            f->reply_left = n - i - 1;
            return 0;
        } else if (strcmp(key, "error") == 0) {
            const uint32_t m = mpack_expect_map(r);
            for (uint32_t j = 0; j < m; ++j) {
                mpack_expect_cstr(r, key, sizeof key);
                if (strcmp(key, "code") == 0)
                    code = (int)mpack_expect_i32(r);
                else
                    mpack_discard(r);
            }
            mpack_done_map(r);
        } else {
            mpack_discard(r);
        }
    }
    mpack_done_map(r);
    mpack_reader_destroy(r);
    EXPECT_TRUE(code > 0);
    return code;
}

/* Skips the reply entries after `result`, closes the reply map and destroys `r`. */
static mpack_error_t close_reply(mg_fx *f, mpack_reader_t *r) {
    for (uint32_t i = 0; i < f->reply_left * 2u && mpack_reader_error(r) == mpack_ok; ++i)
        mpack_discard(r);
    mpack_done_map(r);
    return mpack_reader_destroy(r);
}

static int error_of(mg_fx *f) {
    mpack_reader_t r;
    const int code = open_reply(f, &r);
    if (code == 0)
        close_reply(f, &r);
    return code;
}

static void read_generated(mg_fx *f, de_ep_material_graph_generated *out) {
    mpack_reader_t r;
    ASSERT_EQ(open_reply(f, &r), 0);
    de_ep_arena a = {f->decode, ARENA_CAP, 0};
    de_ep_read_material_graph_generated(&r, &a, out);
    ASSERT_EQ(close_reply(f, &r), mpack_ok);
}

static void read_compiled(mg_fx *f, de_ep_material_graph_compiled *out) {
    mpack_reader_t r;
    ASSERT_EQ(open_reply(f, &r), 0);
    de_ep_arena a = {f->decode, ARENA_CAP, 0};
    de_ep_read_material_graph_compiled(&r, &a, out);
    ASSERT_EQ(close_reply(f, &r), mpack_ok);
}

static bool str_is(de_ep_str s, const char *c) {
    return s.len == strlen(c) && memcmp(s.ptr, c, s.len) == 0;
}
static bool str_has(de_ep_str s, const char *needle) {
    const size_t n = strlen(needle);
    for (uint32_t i = 0; i + n <= s.len; ++i)
        if (memcmp(s.ptr + i, needle, n) == 0)
            return true;
    return false;
}
static const de_ep_material_graph_diagnostic *find_diag(const de_ep_material_graph_generated *g,
                                                        const char *node, const char *text) {
    for (uint32_t i = 0; i < g->diagnostics.count; ++i) {
        const de_ep_material_graph_diagnostic *d = &g->diagnostics.items[i];
        if (str_is(d->node, node) && (str_has(d->message, text) || str_is(d->pin, text)))
            return d;
    }
    return NULL;
}

#define HEADER "kind = \"material\"\nversion = 1\nnext_id = 64\n"

static const char basic_graph[] =
    HEADER "[[node]]\nid = \"n1\"\ntype = \"param.color\"\npos = [0.0, 0.0]\n"
           "values = { name = \"tint\", r = 0.5, g = 0.25, b = 1.0 }\n"
           "[[node]]\nid = \"n2\"\ntype = \"param.float\"\n"
           "values = { name = \"rough\", value = 0.75, min = 0.0, max = 1.0 }\n"
           "[[node]]\nid = \"n3\"\ntype = \"output.base\"\npos = [200.0, 0.0]\n"
           "[[link]]\nfrom = \"n1.out\"\nto = \"n3.base_color\"\n"
           "[[link]]\nfrom = \"n2.out\"\nto = \"n3.roughness\"\n";

/* A type error inside a subgraph, and a link from a missing subgraph output. */
static const char bad_graph[] =
    HEADER "[[node]]\nid = \"n4\"\ntype = \"subgraph\"\n"
           "[node.graph]\n"
           "[[node.graph.node]]\nid = \"n1\"\ntype = \"input.uv\"\n"
           "[[node.graph.node]]\nid = \"n2\"\ntype = \"math.one_minus\"\n"
           "[[node.graph.node]]\nid = \"n3\"\ntype = \"subgraph.output\"\n"
           "[[node.graph.link]]\nfrom = \"n1.out\"\nto = \"n2.x\"\n"
           "[[node.graph.link]]\nfrom = \"n2.out\"\nto = \"n3.in\"\n"
           "[[node]]\nid = \"n6\"\ntype = \"output.base\"\n"
           "[[link]]\nfrom = \"n4.n3\"\nto = \"n6.roughness\"\n"
           "[[link]]\nfrom = \"n4.n9\"\nto = \"n6.metallic\"\n";

/* Valid graph, but the code node names an undefined identifier: only Slang finds it. */
static const char slang_error_graph[] =
    HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\nvalues = { value = 2.0 }\n"
           "[[node]]\nid = \"n7\"\ntype = \"code.float\"\n"
           "values = { code = \"a * undefined_thing\" }\n"
           "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n"
           "[[link]]\nfrom = \"n1.out\"\nto = \"n7.a\"\n"
           "[[link]]\nfrom = \"n7.out\"\nto = \"n3.roughness\"\n";

/* ---------- registration ---------- */

TEST(editor_material_graph, registers_methods_and_capability) {
    mg_fx f;
    fx_setup(&f);
    EXPECT_TRUE(f.ed.handlers[DE_EP_METHOD_MATERIAL_GRAPH_CATALOG] != NULL);
    EXPECT_TRUE(f.ed.handlers[DE_EP_METHOD_MATERIAL_GRAPH_GENERATE] != NULL);
    EXPECT_TRUE(f.ed.handlers[DE_EP_METHOD_MATERIAL_GRAPH_COMPILE] != NULL);
    bool cap = false;
    for (u32 i = 0; i < f.ed.cap_count; ++i)
        cap |= strcmp(f.ed.caps[i], "assets.materialGraph") == 0;
    EXPECT_TRUE(cap);
    EXPECT_STREQ(de_ep_method_capability(DE_EP_METHOD_MATERIAL_GRAPH_COMPILE),
                 "assets.materialGraph");
    fx_teardown(&f);
}

/* Protocol 1.9+ hello replies carry the server's native version next to its schema hash. */
TEST(editor_material_graph, hello_reports_server_protocol) {
    mg_fx f;
    fx_setup(&f);
    de_ep_str caps[DE__ED_MAX_CAPS];
    de_ep_hello_result r;
    de__ed_hello_result(&f.ed, "1.9", caps, &r);
    EXPECT_TRUE(str_is(r.protocol, "1.9"));
    EXPECT_TRUE(r.has_server_protocol);
    EXPECT_TRUE(str_is(r.server_protocol, DE_EP_PROTOCOL_VERSION));
    EXPECT_TRUE(r.has_schema);
    EXPECT_TRUE(str_is(r.schema, DE_EP_SCHEMA_HASH));
    EXPECT_STREQ(DE_EP_PROTOCOL_VERSION, "1.21"); /* pinned dide e8fb2a88 (DENG-820) */
    fx_teardown(&f);
}

/* ---------- materialGraph.catalog ---------- */

TEST(editor_material_graph, catalog_round_trip) {
    mg_fx f;
    fx_setup(&f);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_CATALOG);
    mpack_reader_t r;
    ASSERT_EQ(open_reply(&f, &r), 0);
    de_ep_arena a = {f.decode, ARENA_CAP, 0};
    de_ep_material_graph_catalog c;
    de_ep_read_material_graph_catalog(&r, &a, &c);
    ASSERT_EQ(close_reply(&f, &r), mpack_ok);
    EXPECT_TRUE(str_has(c.json, "\"kind\":\"material\""));
    EXPECT_TRUE(str_has(c.json, "\"output.base\""));
    EXPECT_TRUE(str_has(c.json, "\"subgraph\""));
    fx_teardown(&f);
}

/* ---------- materialGraph.generate ---------- */

TEST(editor_material_graph, generate_round_trip) {
    mg_fx f;
    fx_setup(&f);
    encode_generate(&f, basic_graph, sizeof basic_graph - 1, "rock_moss");
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE);
    de_ep_material_graph_generated g;
    read_generated(&f, &g);
    EXPECT_TRUE(g.ok);
    ASSERT_TRUE(g.has_source);
    EXPECT_TRUE(str_has(g.source, "rock_moss"));
    EXPECT_EQ(g.diagnostics.count, 0u);
    ASSERT_EQ(g.params.count, 2u);
    const de_ep_material_graph_param *color = NULL, *rough = NULL;
    for (u32 i = 0; i < g.params.count; ++i) {
        if (str_is(g.params.items[i].name, "tint"))
            color = &g.params.items[i];
        if (str_is(g.params.items[i].name, "rough"))
            rough = &g.params.items[i];
    }
    ASSERT_TRUE(color != NULL && rough != NULL);
    EXPECT_EQ(color->kind, DE_EP_MATERIAL_GRAPH_PARAM_KIND_COLOR);
    EXPECT_TRUE(str_is(color->node, "n1"));
    ASSERT_EQ(color->value.count, 3u);
    EXPECT_NEAR(color->value.items[0], 0.5f, 1e-6f);
    EXPECT_NEAR(color->value.items[1], 0.25f, 1e-6f);
    EXPECT_NEAR(color->value.items[2], 1.0f, 1e-6f);
    EXPECT_EQ(rough->kind, DE_EP_MATERIAL_GRAPH_PARAM_KIND_FLOAT);
    ASSERT_EQ(rough->value.count, 1u);
    EXPECT_NEAR(rough->value.items[0], 0.75f, 1e-6f);
    EXPECT_NEAR(rough->max, 1.0f, 1e-6f);
    fx_teardown(&f);
}

/* Graph errors are a result (ok = false), not an error reply. Diagnostics carry node path and
 * pin. */
TEST(editor_material_graph, generate_errors_carry_node_path_and_pin) {
    mg_fx f;
    fx_setup(&f);
    encode_generate(&f, bad_graph, sizeof bad_graph - 1, NULL);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE);
    de_ep_material_graph_generated g;
    read_generated(&f, &g);
    EXPECT_FALSE(g.ok);
    EXPECT_GE(g.diagnostics.count, 2u);
    const de_ep_material_graph_diagnostic *d = find_diag(&g, "n4/n2", "x");
    ASSERT_TRUE(d != NULL);
    EXPECT_TRUE(str_is(d->pin, "x"));
    EXPECT_EQ(d->severity, DE_EP_MATERIAL_GRAPH_SEVERITY_ERROR);
    d = find_diag(&g, "n4", "n9");
    ASSERT_TRUE(d != NULL);
    EXPECT_TRUE(str_is(d->pin, "n9"));
    fx_teardown(&f);
}

TEST(editor_material_graph, generate_rejects_bad_params) {
    mg_fx f;
    fx_setup(&f);
    /* Missing graph. */
    f.params_size = 0;
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    /* Empty graph. */
    encode_generate(&f, "", 0, NULL);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    /* Over the 4 MiB graph limit. */
    const size_t big = ((size_t)4 << 20) + 1;
    char *text = malloc(big);
    ASSERT_TRUE(text != NULL);
    memset(text, '#', big);
    encode_generate(&f, text, big, NULL);
    free(text);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    /* Not a map. */
    f.params[0] = '\xc0'; /* nil */
    f.params_size = 1;
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_GENERATE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    fx_teardown(&f);
}

/* ---------- materialGraph.compile ---------- */

TEST(editor_material_graph, compile_rejects_bad_targets) {
    mg_fx f;
    fx_setup(&f);
    static const char *const unknown[] = {"spirv", "glsl"};
    encode_compile(&f, basic_graph, unknown, 2);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    static const char *const dup[] = {"spirv", "spirv"};
    encode_compile(&f, basic_graph, dup, 2);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    encode_compile(&f, basic_graph, NULL, 0);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    static const char *const seven[] = {"spirv", "dxil", "metallib", "metal",
                                        "spirv-asm", "dxil-asm", "spirv"};
    encode_compile(&f, basic_graph, seven, 7);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    EXPECT_EQ(error_of(&f), DE_EP_ERR_INVALID_PARAMS);
    fx_teardown(&f);
}

/* A graph with errors is not compiled: generated.ok is false and there are no target results. */
TEST(editor_material_graph, compile_skips_invalid_graph) {
    mg_fx f;
    fx_setup(&f);
    static const char *const t[] = {"spirv"};
    encode_compile(&f, bad_graph, t, 1);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    de_ep_material_graph_compiled c;
    read_compiled(&f, &c);
    EXPECT_FALSE(c.generated.ok);
    EXPECT_GE(c.generated.diagnostics.count, 1u);
    EXPECT_EQ(c.targets.count, 0u);
    fx_teardown(&f);
}

TEST(editor_material_graph, compile_round_trip) {
    char why[256];
    if (!de__slang_available(why, sizeof why))
        SKIP(why);
    mg_fx f;
    fx_setup(&f);
    static const char *const t[] = {"spirv", "metal"};
    encode_compile(&f, basic_graph, t, 2);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    de_ep_material_graph_compiled c;
    read_compiled(&f, &c);
    EXPECT_TRUE(c.generated.ok);
    EXPECT_TRUE(c.generated.has_source);
    ASSERT_EQ(c.targets.count, 2u);
    for (u32 i = 0; i < 2; ++i) {
        const de_ep_material_graph_target_result *tr = &c.targets.items[i];
        EXPECT_TRUE(str_is(tr->target, t[i]));
        if (tr->status == DE_EP_MATERIAL_GRAPH_TARGET_STATUS_UNSUPPORTED)
            continue; /* the host's Slang build has no backend for it */
        EXPECT_EQ(tr->status, DE_EP_MATERIAL_GRAPH_TARGET_STATUS_OK);
        if (tr->status == DE_EP_MATERIAL_GRAPH_TARGET_STATUS_ERROR && tr->has_message)
            fprintf(stderr, "%.*s\n", (int)tr->message.len, tr->message.ptr);
        EXPECT_TRUE(tr->has_code_size);
        EXPECT_GT(tr->code_size, 0u);
        EXPECT_EQ(tr->diagnostics.count, 0u);
    }
    EXPECT_EQ(c.targets.items[0].status, DE_EP_MATERIAL_GRAPH_TARGET_STATUS_OK);
    fx_teardown(&f);
}

/* Slang errors come back on the target, mapped to the node that holds the code. */
TEST(editor_material_graph, compile_maps_slang_errors_to_nodes) {
    char why[256];
    if (!de__slang_available(why, sizeof why))
        SKIP(why);
    mg_fx f;
    fx_setup(&f);
    static const char *const t[] = {"spirv"};
    encode_compile(&f, slang_error_graph, t, 1);
    run(&f, DE_EP_METHOD_MATERIAL_GRAPH_COMPILE);
    de_ep_material_graph_compiled c;
    read_compiled(&f, &c);
    EXPECT_TRUE(c.generated.ok);
    ASSERT_EQ(c.targets.count, 1u);
    const de_ep_material_graph_target_result *tr = &c.targets.items[0];
    EXPECT_EQ(tr->status, DE_EP_MATERIAL_GRAPH_TARGET_STATUS_ERROR);
    EXPECT_FALSE(tr->has_code_size);
    bool mapped = false;
    for (u32 i = 0; i < tr->diagnostics.count; ++i) {
        const de_ep_material_graph_diagnostic *d = &tr->diagnostics.items[i];
        if (str_is(d->node, "n7") && str_has(d->message, "undefined_thing")) {
            mapped = true;
            EXPECT_EQ(d->severity, DE_EP_MATERIAL_GRAPH_SEVERITY_ERROR);
        }
    }
    EXPECT_TRUE(mapped);
    fx_teardown(&f);
}

#else
TEST(editor_material_graph, needs_cooker_build) {
    SKIP("material graph IPC is Debug/Dev with the cooker and Slang only");
}
#endif
