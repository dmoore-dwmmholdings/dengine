// DENG-579: material graph to Slang code generation (SPEC §12.6, DIDE-R161).
#include "de_test.h"
#include "cook/importers/material_graph/material_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const de__slang_module de__shader_common_modules[];
extern const u32 de__shader_common_module_count;

static de_result generate(const char *graph, de__mg_result *out) {
    memset(out, 0, sizeof *out);
    return de__mg_generate(&(de__mg_desc){.graph = graph,
                                          .graph_size = strlen(graph),
                                          .name = "test",
                                          .allocator = de_heap_allocator()},
                           out);
}

static const de__mg_diag *find_diag(const de__mg_result *r, const char *node, const char *text) {
    for (u32 i = 0; i < r->diag_count; ++i)
        if (strcmp(r->diags[i].node, node) == 0 && strstr(r->diags[i].message, text))
            return &r->diags[i];
    return NULL;
}

static void dump_diags(const de__mg_result *r) {
    for (u32 i = 0; i < r->diag_count; ++i)
        fprintf(stderr, "  [%s] %s.%s: %s\n", r->diags[i].severity == DE__MG_ERROR ? "E" : "W",
                r->diags[i].node, r->diags[i].pin, r->diags[i].message);
}

/* The first line of the source that contains `text`, 1-based; 0 if none. */
static u32 line_of(const char *source, const char *text) {
    const char *at = strstr(source, text);
    if (!at)
        return 0;
    u32 line = 1;
    for (const char *p = source; p < at; ++p)
        line += *p == '\n';
    return line;
}

#define HEADER "kind = \"material\"\nversion = 1\nnext_id = 64\n"

static const char basic_graph[] =
    HEADER "[[node]]\nid = \"n1\"\ntype = \"param.color\"\npos = [0.0, 0.0]\n"
           "values = { name = \"tint\", r = 0.5, g = 0.25, b = 1.0 }\n"
           "[[node]]\nid = \"n2\"\ntype = \"output.base\"\npos = [200.0, 0.0]\n"
           "values = { roughness = 0.75 }\n"
           "[[link]]\nfrom = \"n1.out\"\nto = \"n2.base_color\"\n";

TEST(material_graph, catalog_lists_nodes_and_conversions) {
    de_strbuf b;
    de_strbuf_init(&b, de_heap_allocator());
    ASSERT_EQ(de__mg_catalog_json(&b), DE_OK);
    const char *j = de_strbuf_cstr(&b);
    EXPECT_TRUE(strstr(j, "\"kind\":\"material\"") != NULL);
    EXPECT_TRUE(strstr(j, "[\"float\",\"vec3\"]") != NULL);
    EXPECT_TRUE(strstr(j, "[\"uv\",\"vec2\"]") != NULL);
    const char *ids[] = {"\"texture.triplanar\"", "\"texture.sample\"", "\"blend.height\"",
                         "\"output.base\"",       "\"param.float\"",    "\"param.color\"",
                         "\"noise.gradient\"",    "\"code.float\"",     "\"reroute\"",
                         "\"subgraph\"",          "\"math.add\"",       "\"output.clearcoat\""};
    for (u32 i = 0; i < sizeof ids / sizeof ids[0]; ++i)
        EXPECT_TRUE(strstr(j, ids[i]) != NULL);
    de_strbuf_free(&b);
}

TEST(material_graph, basic_graph_generates_a_model) {
    de__mg_result r;
    de_result res = generate(basic_graph, &r);
    if (res != DE_OK)
        dump_diags(&r);
    ASSERT_EQ(res, DE_OK);
    ASSERT_TRUE(r.source != NULL);
    EXPECT_EQ(r.error_count, 0u);
    EXPECT_TRUE(strstr(r.source, "struct MaterialGraphModel : IMaterialModel") != NULL);
    EXPECT_TRUE(strstr(r.source, "void cs_resolve_model(") != NULL);
    EXPECT_TRUE(strstr(r.source, "de_resolve_tiles<MaterialGraphModel>") != NULL);
    ASSERT_EQ(r.param_count, 1u);
    EXPECT_EQ(r.params[0].kind, DE__MG_PARAM_COLOR);
    EXPECT_STREQ(r.params[0].name, "tint");
    EXPECT_STREQ(r.params[0].node, "n1");
    EXPECT_EQ(r.params[0].row, 0u);
    EXPECT_EQ(r.params[0].component, 0u);
    EXPECT_FLOAT_EQ(r.params[0].value[1], 0.25f, 1e-6f);
    u32 line = line_of(r.source, "_n1_out");
    ASSERT_GT(line, 0u);
    ASSERT_TRUE(de__mg_node_at_line(&r, line) != NULL);
    EXPECT_STREQ(de__mg_node_at_line(&r, line), "n1");
    EXPECT_TRUE(de__mg_node_at_line(&r, 1) == NULL);
    EXPECT_TRUE(de__mg_node_at_line(&r, 1000000) == NULL);
    de__mg_result_free(&r);
    EXPECT_TRUE(r.source == NULL);
}

TEST(material_graph, output_does_not_depend_on_file_order) {
    static const char a[] =
        HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\nvalues = { value = 0.25 }\n"
               "[[node]]\nid = \"n2\"\ntype = \"value.float\"\nvalues = { value = 0.5 }\n"
               "[[node]]\nid = \"n10\"\ntype = \"math.add\"\n"
               "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n"
               "[[link]]\nfrom = \"n1.out\"\nto = \"n10.a\"\n"
               "[[link]]\nfrom = \"n2.out\"\nto = \"n10.b\"\n"
               "[[link]]\nfrom = \"n10.out\"\nto = \"n3.roughness\"\n";
    static const char b[] =
        HEADER "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n"
               "[[node]]\nid = \"n10\"\ntype = \"math.add\"\n"
               "[[node]]\nid = \"n2\"\ntype = \"value.float\"\nvalues = { value = 0.5 }\n"
               "[[node]]\nid = \"n1\"\ntype = \"value.float\"\nvalues = { value = 0.25 }\n"
               "[[link]]\nfrom = \"n10.out\"\nto = \"n3.roughness\"\n"
               "[[link]]\nfrom = \"n2.out\"\nto = \"n10.b\"\n"
               "[[link]]\nfrom = \"n1.out\"\nto = \"n10.a\"\n";
    de__mg_result ra, rb;
    ASSERT_EQ(generate(a, &ra), DE_OK);
    ASSERT_EQ(generate(b, &rb), DE_OK);
    EXPECT_STREQ(ra.source, rb.source);
    /* n2 comes before n10: numeric order, not string order. */
    EXPECT_LT(line_of(ra.source, "_n2_out ="), line_of(ra.source, "_n10_out ="));
    de__mg_result_free(&ra);
    de__mg_result_free(&rb);
}

TEST(material_graph, type_mismatch_names_node_and_pin) {
    static const char g[] = HEADER "[[node]]\nid = \"n1\"\ntype = \"input.uv\"\n"
                                   "[[node]]\nid = \"n2\"\ntype = \"math.add\"\n"
                                   "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n"
                                   "[[link]]\nfrom = \"n1.out\"\nto = \"n2.a\"\n"
                                   "[[link]]\nfrom = \"n2.out\"\nto = \"n3.roughness\"\n";
    de__mg_result r;
    EXPECT_EQ(generate(g, &r), DE_ERR_PARSE);
    EXPECT_TRUE(r.source == NULL);
    EXPECT_GE(r.error_count, 1u);
    const de__mg_diag *d = find_diag(&r, "n2", "uv");
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->pin, "a");
    EXPECT_EQ(d->severity, DE__MG_ERROR);
    EXPECT_GT(d->graph_line, 0u);
    de__mg_result_free(&r);
}

TEST(material_graph, structural_errors_are_diagnosed) {
    de__mg_result r;
    /* Unknown node type. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"bogus.node\"\n"
                              "[[node]]\nid = \"n2\"\ntype = \"output.base\"\n",
                       &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "n1", "unknown node type") != NULL);
    de__mg_result_free(&r);
    /* No output.base. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "", "output.base") != NULL);
    de__mg_result_free(&r);
    /* Two output.base nodes. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"output.base\"\n"
                              "[[node]]\nid = \"n2\"\ntype = \"output.base\"\n",
                       &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "n2", "more than one") != NULL);
    de__mg_result_free(&r);
    /* A cycle. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"math.add\"\n"
                              "[[node]]\nid = \"n2\"\ntype = \"math.add\"\n"
                              "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n"
                              "[[link]]\nfrom = \"n1.out\"\nto = \"n2.a\"\n"
                              "[[link]]\nfrom = \"n2.out\"\nto = \"n1.a\"\n"
                              "[[link]]\nfrom = \"n2.out\"\nto = \"n3.roughness\"\n",
                       &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "n1", "cycle") != NULL);
    de__mg_result_free(&r);
    /* An unknown pin and an unknown node in links. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\n"
                              "[[node]]\nid = \"n2\"\ntype = \"output.base\"\n"
                              "[[link]]\nfrom = \"n1.zzz\"\nto = \"n2.roughness\"\n"
                              "[[link]]\nfrom = \"n9.out\"\nto = \"n2.metallic\"\n",
                       &r),
              DE_ERR_PARSE);
    const de__mg_diag *d = find_diag(&r, "n1", "no output pin");
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->pin, "zzz");
    EXPECT_TRUE(find_diag(&r, "n9", "no node") != NULL);
    de__mg_result_free(&r);
    /* Two links into one input. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\n"
                              "[[node]]\nid = \"n2\"\ntype = \"output.base\"\n"
                              "[[link]]\nfrom = \"n1.out\"\nto = \"n2.roughness\"\n"
                              "[[link]]\nfrom = \"n1.out\"\nto = \"n2.roughness\"\n",
                       &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "n2", "more than one link") != NULL);
    de__mg_result_free(&r);
    /* Wrong kind and bad TOML. */
    EXPECT_EQ(generate("kind = \"script\"\nversion = 1\n", &r), DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "", "kind") != NULL);
    de__mg_result_free(&r);
    EXPECT_EQ(generate("kind = = 3\n", &r), DE_ERR_PARSE);
    EXPECT_EQ(r.error_count, 1u);
    de__mg_result_free(&r);
    /* code.float rejects statements before Slang sees them. */
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"code.float\"\n"
                              "values = { code = \"a; }\" }\n"
                              "[[node]]\nid = \"n2\"\ntype = \"output.base\"\n"
                              "[[link]]\nfrom = \"n1.out\"\nto = \"n2.roughness\"\n",
                       &r),
              DE_ERR_PARSE);
    d = find_diag(&r, "n1", "code");
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->pin, "code");
    de__mg_result_free(&r);
    /* Invalid arguments. */
    EXPECT_EQ(de__mg_generate(NULL, &r), DE_ERR_INVALID_ARGUMENT);
}

/* DENG-773: DIDE names both subgraph boundary pins "value"; "in"/"out" stay valid. */
TEST(material_graph, subgraph_boundary_accepts_value_pin_alias) {
    static const char g[] =
        HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\nvalues = { value = 0.25 }\n"
               "[[node]]\nid = \"n4\"\ntype = \"subgraph\"\n"
               "[node.graph]\n"
               "[[node.graph.node]]\nid = \"n1\"\ntype = \"subgraph.input\"\n"
               "[[node.graph.node]]\nid = \"n2\"\ntype = \"math.one_minus\"\n"
               "[[node.graph.node]]\nid = \"n3\"\ntype = \"subgraph.output\"\n"
               "[[node.graph.link]]\nfrom = \"n1.value\"\nto = \"n2.x\"\n"
               "[[node.graph.link]]\nfrom = \"n2.out\"\nto = \"n3.value\"\n"
               "[[node]]\nid = \"n6\"\ntype = \"output.base\"\n"
               "[[link]]\nfrom = \"n1.out\"\nto = \"n4.n1\"\n"
               "[[link]]\nfrom = \"n4.n3\"\nto = \"n6.roughness\"\n";
    de__mg_result r;
    de_result res = generate(g, &r);
    if (res != DE_OK)
        dump_diags(&r);
    ASSERT_EQ(res, DE_OK);
    de__mg_result_free(&r);
}

TEST(material_graph, subgraph_and_reroute_inline_with_paths) {
    static const char g[] =
        HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\nvalues = { value = 0.25 }\n"
               "[[node]]\nid = \"n4\"\ntype = \"subgraph\"\n"
               "[node.graph]\n"
               "[[node.graph.node]]\nid = \"n1\"\ntype = \"subgraph.input\"\n"
               "[[node.graph.node]]\nid = \"n2\"\ntype = \"math.one_minus\"\n"
               "[[node.graph.node]]\nid = \"n3\"\ntype = \"subgraph.output\"\n"
               "[[node.graph.link]]\nfrom = \"n1.out\"\nto = \"n2.x\"\n"
               "[[node.graph.link]]\nfrom = \"n2.out\"\nto = \"n3.in\"\n"
               "[[node]]\nid = \"n5\"\ntype = \"reroute\"\n"
               "[[node]]\nid = \"n6\"\ntype = \"output.base\"\n"
               "[[link]]\nfrom = \"n1.out\"\nto = \"n4.n1\"\n"
               "[[link]]\nfrom = \"n4.n3\"\nto = \"n5.in\"\n"
               "[[link]]\nfrom = \"n5.out\"\nto = \"n6.roughness\"\n";
    de__mg_result r;
    de_result res = generate(g, &r);
    if (res != DE_OK)
        dump_diags(&r);
    ASSERT_EQ(res, DE_OK);
    u32 line = line_of(r.source, "_n4_n2_out =");
    ASSERT_GT(line, 0u);
    EXPECT_STREQ(de__mg_node_at_line(&r, line), "n4/n2");
    de__mg_result_free(&r);

    /* A type error inside the subgraph names the member. */
    static const char bad[] =
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
    EXPECT_EQ(generate(bad, &r), DE_ERR_PARSE);
    const de__mg_diag *d = find_diag(&r, "n4/n2", "uv");
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->pin, "x");
    d = find_diag(&r, "n4", "subgraph.output");
    ASSERT_TRUE(d != NULL);
    EXPECT_STREQ(d->pin, "n9");
    de__mg_result_free(&r);
}

TEST(material_graph, params_pack_rows_and_texture_slots) {
    static const char g[] = HEADER
        "[[node]]\nid = \"n1\"\ntype = \"param.color\"\nvalues = { name = \"c1\" }\n"
        "[[node]]\nid = \"n2\"\ntype = \"param.float\"\n"
        "values = { name = \"f1\", value = 0.5, min = 0.0, max = 4.0 }\n"
        "[[node]]\nid = \"n3\"\ntype = \"param.color\"\nvalues = { name = \"c2\" }\n"
        "[[node]]\nid = \"n4\"\ntype = \"param.float\"\nvalues = { name = \"f2\" }\n"
        "[[node]]\nid = \"n5\"\ntype = \"param.float\"\nvalues = { name = \"f3\" }\n"
        "[[node]]\nid = \"n6\"\ntype = \"texture.sample\"\nvalues = { texture = \"albedo\" }\n"
        "[[node]]\nid = \"n7\"\ntype = \"texture.sample\"\nvalues = { texture = \"rough\" }\n"
        "[[node]]\nid = \"n8\"\ntype = \"texture.sample\"\nvalues = { texture = \"albedo\" }\n"
        "[[node]]\nid = \"n9\"\ntype = \"output.base\"\n";
    de__mg_result r;
    de_result res = generate(g, &r);
    if (res != DE_OK)
        dump_diags(&r);
    ASSERT_EQ(res, DE_OK);
    ASSERT_EQ(r.param_count, 7u);
    struct {
        const char *name;
        u32 kind, row, component, slot;
    } want[] = {{"c1", DE__MG_PARAM_COLOR, 0, 0, 0},     {"f1", DE__MG_PARAM_FLOAT, 0, 3, 0},
                {"c2", DE__MG_PARAM_COLOR, 1, 0, 0},     {"f2", DE__MG_PARAM_FLOAT, 1, 3, 0},
                {"f3", DE__MG_PARAM_FLOAT, 2, 0, 0},     {"albedo", DE__MG_PARAM_TEXTURE, 0, 0, 0},
                {"rough", DE__MG_PARAM_TEXTURE, 0, 0, 1}};
    for (u32 i = 0; i < 7; ++i) {
        EXPECT_STREQ(r.params[i].name, want[i].name);
        EXPECT_EQ((u32)r.params[i].kind, want[i].kind);
        if (want[i].kind == DE__MG_PARAM_TEXTURE) {
            EXPECT_EQ(r.params[i].slot, want[i].slot);
        } else {
            EXPECT_EQ(r.params[i].row, want[i].row);
            EXPECT_EQ(r.params[i].component, want[i].component);
        }
    }
    EXPECT_FLOAT_EQ(r.params[1].max, 4.0f, 1e-6f);
    EXPECT_FLOAT_EQ(r.params[1].value[0], 0.5f, 1e-6f);
    de__mg_result_free(&r);

    /* Too many textures, and a duplicate parameter name. */
    char big[8192];
    int n =
        snprintf(big, sizeof big, "%s", HEADER "[[node]]\nid = \"n1\"\ntype = \"output.base\"\n");
    for (int i = 0; i < 20; ++i)
        n += snprintf(big + n, sizeof big - (size_t)n,
                      "[[node]]\nid = \"n%d\"\ntype = \"texture.sample\"\n"
                      "values = { texture = \"t%d\" }\n",
                      i + 2, i);
    EXPECT_EQ(generate(big, &r), DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "n21", "texture slots") != NULL);
    de__mg_result_free(&r);
    EXPECT_EQ(generate(HEADER "[[node]]\nid = \"n1\"\ntype = \"param.float\"\n"
                              "values = { name = \"x\" }\n"
                              "[[node]]\nid = \"n2\"\ntype = \"param.color\"\n"
                              "values = { name = \"x\" }\n"
                              "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n",
                       &r),
              DE_ERR_PARSE);
    EXPECT_TRUE(find_diag(&r, "n2", "already") != NULL);
    de__mg_result_free(&r);
}

TEST(material_graph, allocation_failures_do_not_leak) {
    de_tracking_allocator tracking;
    de_allocator a = de_tracking_allocator_init(&tracking, NULL, DE_MEM_ASSET);
    de__mg_result r;
    for (i64 allowed = 0; allowed < 400; ++allowed) {
        de_tracking_allocator_fail_after(&tracking, allowed);
        memset(&r, 0, sizeof r);
        de_result res = de__mg_generate(&(de__mg_desc){.graph = basic_graph,
                                                       .graph_size = strlen(basic_graph),
                                                       .allocator = &a},
                                        &r);
        de__mg_result_free(&r);
        EXPECT_EQ(de_tracking_allocator_stats(&tracking).blocks_live, 0u);
        if (res == DE_OK)
            break;
        EXPECT_EQ(res, DE_ERR_OUT_OF_MEMORY);
    }
    de_tracking_allocator_fail_after(&tracking, -1);
}

/* ---- Slang compiles (DIDE-R161.AC1, static part) ---- */

typedef struct import_files {
    void *data[1024];
    u32 count;
} import_files;

static de_result shader_read(void *user, const char *path, const void **data, u64 *size) {
    import_files *files = user;
    if (files->count == 1024)
        return DE_ERR_OUT_OF_MEMORY;
    char full[2048];
    int n = snprintf(full, sizeof full, "%s/%s", DE_MG_SHADER_ROOT, path);
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

static de_result compile(de__mg_result *g, de__slang_target target) {
    import_files files = {0};
    de__slang_compile_result out = {0};
    de_result r =
        de__mg_compile(g,
                       &(de__mg_compile_desc){.target = target,
                                              .path = "material_graph_test.slang",
                                              .read = shader_read,
                                              .read_user = &files,
                                              .modules = de__shader_common_modules,
                                              .module_count = de__shader_common_module_count,
                                              .preload_count = 1},
                       &out);
    for (u32 i = 0; i < files.count; ++i)
        free(files.data[i]);
    if (r != DE_OK)
        fprintf(stderr, "%s: %s\n", de__slang_target_name(target), out.diagnostics);
    de__slang_compile_result_free(&out);
    return r;
}

/* Triplanar rock blended with moss by height on the up-facing parts, tinted, with params. */
static const char rock_moss_graph[] = HEADER
    "[[node]]\nid = \"n1\"\ntype = \"input.position\"\n"
    "[[node]]\nid = \"n2\"\ntype = \"input.normal\"\n"
    "[[node]]\nid = \"n3\"\ntype = \"texture.triplanar\"\n"
    "values = { texture = \"rock_albedo\", scale = 0.5 }\n"
    "[[node]]\nid = \"n4\"\ntype = \"texture.triplanar\"\n"
    "values = { texture = \"rock_height\", scale = 0.5 }\n"
    "[[node]]\nid = \"n5\"\ntype = \"input.uv\"\n"
    "[[node]]\nid = \"n6\"\ntype = \"uv.transform\"\nvalues = { scale = [4.0, 4.0] }\n"
    "[[node]]\nid = \"n7\"\ntype = \"texture.sample\"\nvalues = { texture = \"moss_albedo\" }\n"
    "[[node]]\nid = \"n8\"\ntype = \"texture.sample\"\nvalues = { texture = \"moss_height\" }\n"
    "[[node]]\nid = \"n9\"\ntype = \"vector.split\"\n"
    "[[node]]\nid = \"n10\"\ntype = \"math.remap\"\n"
    "values = { inMin = 0.3, inMax = 0.9, outMin = 0.0, outMax = 1.0 }\n"
    "[[node]]\nid = \"n11\"\ntype = \"math.clamp\"\n"
    "[[node]]\nid = \"n12\"\ntype = \"blend.height\"\nvalues = { contrast = 0.2 }\n"
    "[[node]]\nid = \"n13\"\ntype = \"param.float\"\n"
    "values = { name = \"moss_roughness\", value = 0.8 }\n"
    "[[node]]\nid = \"n14\"\ntype = \"value.float\"\nvalues = { value = 0.9 }\n"
    "[[node]]\nid = \"n15\"\ntype = \"math.lerp\"\n"
    "[[node]]\nid = \"n16\"\ntype = \"output.base\"\n"
    "[[node]]\nid = \"n17\"\ntype = \"noise.gradient\"\nvalues = { scale = 3.0 }\n"
    "[[node]]\nid = \"n18\"\ntype = \"math.multiply\"\nvalues = { b = 0.2 }\n"
    "[[node]]\nid = \"n19\"\ntype = \"param.color\"\nvalues = { name = \"tint\", r = 1.0, g = 1.0, "
    "b = 1.0 }\n"
    "[[node]]\nid = \"n20\"\ntype = \"vec3.multiply\"\n"
    "[[node]]\nid = \"n21\"\ntype = \"output.sheen\"\nvalues = { roughness = 0.6 }\n"
    "[[link]]\nfrom = \"n1.out\"\nto = \"n3.position\"\n"
    "[[link]]\nfrom = \"n2.out\"\nto = \"n3.normal\"\n"
    "[[link]]\nfrom = \"n1.out\"\nto = \"n4.position\"\n"
    "[[link]]\nfrom = \"n2.out\"\nto = \"n4.normal\"\n"
    "[[link]]\nfrom = \"n5.out\"\nto = \"n6.uv\"\n"
    "[[link]]\nfrom = \"n6.out\"\nto = \"n7.uv\"\n"
    "[[link]]\nfrom = \"n6.out\"\nto = \"n8.uv\"\n"
    "[[link]]\nfrom = \"n2.out\"\nto = \"n9.v\"\n"
    "[[link]]\nfrom = \"n9.y\"\nto = \"n10.x\"\n"
    "[[link]]\nfrom = \"n10.out\"\nto = \"n11.x\"\n"
    "[[link]]\nfrom = \"n3.rgb\"\nto = \"n12.a\"\n"
    "[[link]]\nfrom = \"n7.rgb\"\nto = \"n12.b\"\n"
    "[[link]]\nfrom = \"n4.r\"\nto = \"n12.height_a\"\n"
    "[[link]]\nfrom = \"n8.r\"\nto = \"n12.height_b\"\n"
    "[[link]]\nfrom = \"n11.out\"\nto = \"n12.t\"\n"
    "[[link]]\nfrom = \"n14.out\"\nto = \"n15.a\"\n"
    "[[link]]\nfrom = \"n13.out\"\nto = \"n15.b\"\n"
    "[[link]]\nfrom = \"n12.mask\"\nto = \"n15.t\"\n"
    "[[link]]\nfrom = \"n12.out\"\nto = \"n20.a\"\n"
    "[[link]]\nfrom = \"n19.out\"\nto = \"n20.b\"\n"
    "[[link]]\nfrom = \"n20.out\"\nto = \"n16.base_color\"\n"
    "[[link]]\nfrom = \"n15.out\"\nto = \"n16.roughness\"\n"
    "[[link]]\nfrom = \"n1.out\"\nto = \"n17.position\"\n"
    "[[link]]\nfrom = \"n17.out\"\nto = \"n18.a\"\n"
    "[[link]]\nfrom = \"n18.out\"\nto = \"n16.metallic\"\n"
    "[[link]]\nfrom = \"n12.mask\"\nto = \"n21.weight\"\n";

TEST(material_graph, rock_moss_compiles_for_every_target) {
    char why[256];
    if (!de__slang_available(why, sizeof why))
        SKIP(why);
    de__mg_result g;
    de_result res = generate(rock_moss_graph, &g);
    if (res != DE_OK)
        dump_diags(&g);
    ASSERT_EQ(res, DE_OK);
    EXPECT_EQ(g.features, 0x2u); /* DE_MATERIAL_FEATURE_SHEEN */
    const de__slang_target targets[] = {DE__SLANG_SPIRV, DE__SLANG_METAL, DE__SLANG_DXIL};
    u32 compiled = 0;
    for (u32 i = 0; i < 3; ++i) {
        if (!de__slang_target_supported(targets[i]))
            continue;
        EXPECT_EQ(compile(&g, targets[i]), DE_OK);
        ++compiled;
    }
    EXPECT_GT(compiled, 0u);
    if (g.diag_count)
        dump_diags(&g);
    de__mg_result_free(&g);
}

TEST(material_graph, slang_errors_map_to_nodes) {
    char why[256];
    if (!de__slang_available(why, sizeof why))
        SKIP(why);
    static const char g[] =
        HEADER "[[node]]\nid = \"n1\"\ntype = \"value.float\"\nvalues = { value = 2.0 }\n"
               "[[node]]\nid = \"n7\"\ntype = \"code.float\"\n"
               "values = { code = \"a * undefined_thing\" }\n"
               "[[node]]\nid = \"n3\"\ntype = \"output.base\"\n"
               "[[link]]\nfrom = \"n1.out\"\nto = \"n7.a\"\n"
               "[[link]]\nfrom = \"n7.out\"\nto = \"n3.roughness\"\n";
    de__mg_result r;
    ASSERT_EQ(generate(g, &r), DE_OK);
    EXPECT_EQ(compile(&r, DE__SLANG_SPIRV), DE_ERR_PARSE);
    const de__mg_diag *d = find_diag(&r, "n7", "undefined_thing");
    if (!d)
        dump_diags(&r);
    ASSERT_TRUE(d != NULL);
    EXPECT_EQ(d->severity, DE__MG_ERROR);
    EXPECT_GE(r.error_count, 1u);
    de__mg_result_free(&r);
}
