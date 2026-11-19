/*
 * tools/cook/importers/material_graph/material_graph.c: material graph to Slang (DENG-579).
 * See material_graph.h for the file format, the parameter layout and the guarantees.
 *
 * Stages: parse the TOML and flatten subgraphs into one node list (paths "n4/n2"); resolve the
 * links, through subgraph boundaries, to (source node, output) per input pin; order the nodes
 * (dependencies first, ties by node id) and find cycles; infer and check the pin types; lay out
 * the exposed parameters; then emit one line or more per reachable node, recording which node
 * each line came from so that Slang's errors map back to node ids.
 */
#include "cook/importers/material_graph/material_graph.h"

#include "core/de_toml_internal.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Catalog ---- */

enum { T_FLOAT, T_VEC2, T_VEC3, T_VEC4, T_BOOL, T_UV, T_ANY, T_NONE };
static const char *const k_type_name[] = {"float", "vec2", "vec3", "vec4", "bool", "uv", "any"};
static const char *const k_slang_type[] = {"float", "float2", "float3", "float4",
                                           "bool",  "DeMgUv", "float"};
static const u8 k_type_width[] = {1, 2, 3, 4, 1, 0, 0};

/* Implicit conversions on a link: the DIDE GENERIC set plus uv to vec2. */
static const u8 k_conversions[][2] = {{T_FLOAT, T_VEC2}, {T_FLOAT, T_VEC3}, {T_FLOAT, T_VEC4},
                                      {T_VEC3, T_VEC4},  {T_BOOL, T_FLOAT}, {T_UV, T_VEC2}};

enum { V_FLOAT, V_BOOL, V_NAME, V_OP, V_CODE };
static const char *const k_value_kind[] = {"float", "bool", "string", "string", "string"};

enum {
    R_NONE,
    R_PARAM_FLOAT,
    R_PARAM_COLOR,
    R_TEXTURE,
    R_OUTPUT,
    R_CODE,
    R_SUBGRAPH,
    R_SUB_IN,
    R_SUB_OUT,
};

#define MG_PINS 8
#define MG_OUTS 4
#define MG_VALS 6

typedef struct pin_def {
    const char *name;
    u8 type;
    const char *def; /* Slang expression when unconnected and no constant is given */
} pin_def;

typedef struct out_def {
    const char *name;
    u8 type;
    const char *expr; /* template: {pin}, {$value}, {>output}, {@} param, {#} texture slot */
} out_def;

typedef struct val_def {
    const char *name;
    u8 kind;
    float def;
    const char *sdef;
} val_def;

typedef struct node_def {
    const char *id;
    const char *title;
    const char *category;
    u8 role;
    u8 hidden_inputs; /* subgraph.input: its "in" pin is fed from outside, not listed */
    u8 hidden_outputs;
    pin_def in[MG_PINS];
    out_def out[MG_OUTS];
    val_def val[MG_VALS];
    const char *sink; /* output nodes: statements, one per '\n'-separated line */
    u32 features;
} node_def;

#define UV0   "de_mg_uv(v, lod, 0u)"
#define F0    "0.0"
#define F1    "1.0"
#define V3(x) "float3(" x ", " x ", " x ")"
#define TN    "float3(0.0, 0.0, 1.0)"
#define TEX_OUTS(call)                                                                             \
    {{"rgba", T_VEC4, call},                                                                       \
     {"rgb", T_VEC3, "{>rgba}.rgb"},                                                               \
     {"r", T_FLOAT, "{>rgba}.r"},                                                                  \
     {"a", T_FLOAT, "{>rgba}.a"}}
#define BIN_F(id_, title_, op)                                                                     \
    {id_,                                                                                          \
     title_,                                                                                       \
     "Math",                                                                                       \
     R_NONE,                                                                                       \
     0,                                                                                            \
     0,                                                                                            \
     {{"a", T_FLOAT, F0}, {"b", T_FLOAT, F0}},                                                     \
     {{"out", T_FLOAT, "({a} " op " {b})"}},                                                       \
     {{0}},                                                                                        \
     NULL,                                                                                         \
     0}

static const node_def k_nodes[] = {
    /* Values and parameters. */
    {"value.float",
     "Float",
     "Value",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_FLOAT, "{$value}"}},
     {{"value", V_FLOAT, 0.0f, NULL}},
     NULL,
     0},
    {"value.bool",
     "Bool",
     "Value",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_BOOL, "{$value}"}},
     {{"value", V_BOOL, 0.0f, NULL}},
     NULL,
     0},
    {"value.vec3",
     "Vector 3",
     "Value",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_VEC3, "float3({$x}, {$y}, {$z})"}},
     {{"x", V_FLOAT, 0.0f, NULL}, {"y", V_FLOAT, 0.0f, NULL}, {"z", V_FLOAT, 0.0f, NULL}},
     NULL,
     0},
    {"param.float",
     "Float Parameter",
     "Parameter",
     R_PARAM_FLOAT,
     0,
     0,
     {{0}},
     {{"out", T_FLOAT, "{@}"}},
     {{"name", V_NAME, 0.0f, ""},
      {"value", V_FLOAT, 0.0f, NULL},
      {"min", V_FLOAT, 0.0f, NULL},
      {"max", V_FLOAT, 1.0f, NULL}},
     NULL,
     0},
    {"param.color",
     "Colour Parameter",
     "Parameter",
     R_PARAM_COLOR,
     0,
     0,
     {{0}},
     {{"out", T_VEC3, "{@}"}},
     {{"name", V_NAME, 0.0f, ""},
      {"r", V_FLOAT, 1.0f, NULL},
      {"g", V_FLOAT, 1.0f, NULL},
      {"b", V_FLOAT, 1.0f, NULL}},
     NULL,
     0},
    /* Inputs. */
    {"input.uv",
     "UV",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_UV, "de_mg_uv(v, lod, {$set} > 0.5 ? 1u : 0u)"}},
     {{"set", V_FLOAT, 0.0f, NULL}},
     NULL,
     0},
    {"input.vertex_color",
     "Vertex Colour",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"rgb", T_VEC3, "v.color.rgb"}, {"a", T_FLOAT, "v.color.a"}},
     {{0}},
     NULL,
     0},
    {"input.position",
     "World Position",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_VEC3, "v.position"}},
     {{0}},
     NULL,
     0},
    {"input.normal",
     "World Normal",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_VEC3, "v.normal"}},
     {{0}},
     NULL,
     0},
    {"input.view",
     "View Direction",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_VEC3, "v.view"}},
     {{0}},
     NULL,
     0},
    {"input.time",
     "Time",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_FLOAT, "v.time"}},
     {{0}},
     NULL,
     0},
    {"input.object_position",
     "Object Position",
     "Input",
     R_NONE,
     0,
     0,
     {{0}},
     {{"out", T_VEC3, "v.object_position"}},
     {{0}},
     NULL,
     0},
    /* Textures and UVs. */
    {"texture.sample",
     "Sample Texture",
     "Texture",
     R_TEXTURE,
     0,
     0,
     {{"uv", T_UV, UV0}},
     TEX_OUTS("de_mg_sample(m, {#}u, {uv}, lod)"),
     {{"texture", V_NAME, 0.0f, ""}},
     NULL,
     0},
    {"texture.triplanar",
     "Triplanar Texture",
     "Texture",
     R_TEXTURE,
     0,
     0,
     {{"position", T_VEC3, "v.position"},
      {"normal", T_VEC3, "v.normal"},
      {"scale", T_FLOAT, F1},
      {"sharpness", T_FLOAT, "4.0"}},
     TEX_OUTS("de_mg_triplanar(m, {#}u, {position}, {normal}, {scale}, {sharpness}, v.position_dx, v.position_dy, lod)"),
     {{"texture", V_NAME, 0.0f, ""}},
     NULL,
     0},
    {"uv.transform",
     "Transform UV",
     "Texture",
     R_NONE,
     0,
     0,
     {{"uv", T_UV, UV0},
      {"scale", T_VEC2, "float2(1.0, 1.0)"},
      {"offset", T_VEC2, "float2(0.0, 0.0)"},
      {"rotation", T_FLOAT, F0}},
     {{"out", T_UV, "de_mg_uv_transform({uv}, {scale}, {offset}, {rotation})"}},
     {{0}},
     NULL,
     0},
    /* Math (the GENERIC float set first). */
    BIN_F("math.add", "Add", "+"),
    BIN_F("math.subtract", "Subtract", "-"),
    BIN_F("math.multiply", "Multiply", "*"),
    {"math.divide",
     "Divide",
     "Math",
     R_NONE,
     0,
     0,
     {{"a", T_FLOAT, F0}, {"b", T_FLOAT, F1}},
     {{"out", T_FLOAT, "de_mg_divide({a}, {b})"}},
     {{0}},
     NULL,
     0},
    {"math.lerp",
     "Lerp",
     "Math",
     R_NONE,
     0,
     0,
     {{"a", T_FLOAT, F0}, {"b", T_FLOAT, F1}, {"t", T_FLOAT, "0.5"}},
     {{"out", T_FLOAT, "lerp({a}, {b}, {t})"}},
     {{0}},
     NULL,
     0},
    {"math.remap",
     "Remap",
     "Math",
     R_NONE,
     0,
     0,
     {{"x", T_FLOAT, F0}},
     {{"out", T_FLOAT, "de_mg_remap({x}, {$inMin}, {$inMax}, {$outMin}, {$outMax})"}},
     {{"inMin", V_FLOAT, 0.0f, NULL},
      {"inMax", V_FLOAT, 1.0f, NULL},
      {"outMin", V_FLOAT, 0.0f, NULL},
      {"outMax", V_FLOAT, 1.0f, NULL}},
     NULL,
     0},
    {"math.clamp",
     "Clamp",
     "Math",
     R_NONE,
     0,
     0,
     {{"x", T_FLOAT, F0}, {"min", T_FLOAT, F0}, {"max", T_FLOAT, F1}},
     {{"out", T_FLOAT, "clamp({x}, {min}, {max})"}},
     {{0}},
     NULL,
     0},
    {"math.one_minus",
     "One Minus",
     "Math",
     R_NONE,
     0,
     0,
     {{"x", T_FLOAT, F0}},
     {{"out", T_FLOAT, "(1.0 - {x})"}},
     {{0}},
     NULL,
     0},
    {"math.power",
     "Power",
     "Math",
     R_NONE,
     0,
     0,
     {{"x", T_FLOAT, F0}, {"e", T_FLOAT, "2.0"}},
     {{"out", T_FLOAT, "pow(max({x}, 0.0), {e})"}},
     {{0}},
     NULL,
     0},
    /* Vectors. */
    {"vector.compose",
     "Compose",
     "Vector",
     R_NONE,
     0,
     0,
     {{"x", T_FLOAT, F0}, {"y", T_FLOAT, F0}, {"z", T_FLOAT, F0}},
     {{"out", T_VEC3, "float3({x}, {y}, {z})"}},
     {{0}},
     NULL,
     0},
    {"vector.split",
     "Split",
     "Vector",
     R_NONE,
     0,
     0,
     {{"v", T_VEC3, V3(F0)}},
     {{"x", T_FLOAT, "{v}.x"}, {"y", T_FLOAT, "{v}.y"}, {"z", T_FLOAT, "{v}.z"}},
     {{0}},
     NULL,
     0},
    {"vector.dot",
     "Dot",
     "Vector",
     R_NONE,
     0,
     0,
     {{"a", T_VEC3, V3(F0)}, {"b", T_VEC3, V3(F0)}},
     {{"out", T_FLOAT, "dot({a}, {b})"}},
     {{0}},
     NULL,
     0},
    {"vec3.add",
     "Add (vec3)",
     "Vector",
     R_NONE,
     0,
     0,
     {{"a", T_VEC3, V3(F0)}, {"b", T_VEC3, V3(F0)}},
     {{"out", T_VEC3, "({a} + {b})"}},
     {{0}},
     NULL,
     0},
    {"vec3.multiply",
     "Multiply (vec3)",
     "Vector",
     R_NONE,
     0,
     0,
     {{"a", T_VEC3, V3(F1)}, {"b", T_VEC3, V3(F1)}},
     {{"out", T_VEC3, "({a} * {b})"}},
     {{0}},
     NULL,
     0},
    {"vec3.scale",
     "Scale (vec3)",
     "Vector",
     R_NONE,
     0,
     0,
     {{"v", T_VEC3, V3(F1)}, {"s", T_FLOAT, F1}},
     {{"out", T_VEC3, "({v} * {s})"}},
     {{0}},
     NULL,
     0},
    {"vec3.lerp",
     "Lerp (vec3)",
     "Vector",
     R_NONE,
     0,
     0,
     {{"a", T_VEC3, V3(F0)}, {"b", T_VEC3, V3(F1)}, {"t", T_FLOAT, "0.5"}},
     {{"out", T_VEC3, "lerp({a}, {b}, {t})"}},
     {{0}},
     NULL,
     0},
    {"vec3.normalize",
     "Normalize",
     "Vector",
     R_NONE,
     0,
     0,
     {{"v", T_VEC3, TN}},
     {{"out", T_VEC3, "de_mg_normalize({v})"}},
     {{0}},
     NULL,
     0},
    /* Logic. */
    {"logic.compare",
     "Compare",
     "Logic",
     R_NONE,
     0,
     0,
     {{"a", T_FLOAT, F0}, {"b", T_FLOAT, F0}},
     {{"out", T_BOOL, "({a} {$op} {b})"}},
     {{"op", V_OP, 0.0f, "<"}},
     NULL,
     0},
    {"logic.select",
     "Select",
     "Logic",
     R_NONE,
     0,
     0,
     {{"if", T_BOOL, "false"}, {"then", T_FLOAT, F1}, {"else", T_FLOAT, F0}},
     {{"out", T_FLOAT, "({if} ? {then} : {else})"}},
     {{0}},
     NULL,
     0},
    /* Procedural. */
    {"noise.value",
     "Value Noise",
     "Noise",
     R_NONE,
     0,
     0,
     {{"position", T_VEC3, "v.position"}, {"scale", T_FLOAT, F1}},
     {{"out", T_FLOAT, "de_mg_value_noise({position} * {scale})"}},
     {{0}},
     NULL,
     0},
    {"noise.gradient",
     "Gradient Noise",
     "Noise",
     R_NONE,
     0,
     0,
     {{"position", T_VEC3, "v.position"}, {"scale", T_FLOAT, F1}},
     {{"out", T_FLOAT, "de_mg_gradient_noise({position} * {scale})"}},
     {{0}},
     NULL,
     0},
    {"noise.worley",
     "Worley Noise",
     "Noise",
     R_NONE,
     0,
     0,
     {{"position", T_VEC3, "v.position"}, {"scale", T_FLOAT, F1}},
     {{"out", T_FLOAT, "de_mg_worley({position} * {scale})"}},
     {{0}},
     NULL,
     0},
    /* Shading helpers. */
    {"shading.fresnel",
     "Fresnel",
     "Shading",
     R_NONE,
     0,
     0,
     {{"normal", T_VEC3, "v.normal"}, {"power", T_FLOAT, "5.0"}},
     {{"out", T_FLOAT, "de_mg_fresnel({normal}, v.view, {power})"}},
     {{0}},
     NULL,
     0},
    {"normal.unpack",
     "Unpack Normal",
     "Shading",
     R_NONE,
     0,
     0,
     {{"rgb", T_VEC3, "float3(0.5, 0.5, 1.0)"}, {"strength", T_FLOAT, F1}},
     {{"out", T_VEC3, "de_mg_normal_unpack({rgb}, {strength})"}},
     {{0}},
     NULL,
     0},
    {"normal.blend",
     "Blend Normals",
     "Shading",
     R_NONE,
     0,
     0,
     {{"a", T_VEC3, TN}, {"b", T_VEC3, TN}},
     {{"out", T_VEC3, "de_mg_normal_blend({a}, {b})"}},
     {{0}},
     NULL,
     0},
    {"blend.height",
     "Height Blend",
     "Shading",
     R_NONE,
     0,
     0,
     {{"a", T_VEC3, V3(F0)},
      {"b", T_VEC3, V3(F1)},
      {"height_a", T_FLOAT, "0.5"},
      {"height_b", T_FLOAT, "0.5"},
      {"t", T_FLOAT, "0.5"},
      {"contrast", T_FLOAT, "0.1"}},
     {{"mask", T_FLOAT, "de_mg_height_mask({height_a}, {height_b}, {t}, {contrast})"},
      {"out", T_VEC3, "lerp({a}, {b}, {>mask})"}},
     {{0}},
     NULL,
     0},
    {"code.float",
     "Code (float)",
     "Code",
     R_CODE,
     0,
     0,
     {{"a", T_FLOAT, F0}, {"b", T_FLOAT, F0}, {"c", T_FLOAT, F0}},
     {{"out", T_FLOAT, NULL}},
     {{"code", V_CODE, 0.0f, "a"}},
     NULL,
     0},
    /* Structure. */
    {"reroute",
     "Reroute",
     "Structure",
     R_NONE,
     0,
     0,
     {{"in", T_ANY, F0}},
     {{"out", T_ANY, "{in}"}},
     {{0}},
     NULL,
     0},
    {"subgraph", "Subgraph", "Structure", R_SUBGRAPH, 0, 0, {{0}}, {{0}}, {{0}}, NULL, 0},
    {"subgraph.input",
     "Subgraph Input",
     "Structure",
     R_SUB_IN,
     1,
     0,
     {{"in", T_ANY, F0}},
     {{"out", T_ANY, "{in}"}},
     {{0}},
     NULL,
     0},
    {"subgraph.output",
     "Subgraph Output",
     "Structure",
     R_SUB_OUT,
     0,
     1,
     {{"in", T_ANY, F0}},
     {{"out", T_ANY, "{in}"}},
     {{0}},
     NULL,
     0},
    /* Outputs. */
    {"output.base",
     "Surface",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"base_color", T_VEC3, V3("0.8")},
      {"roughness", T_FLOAT, "0.5"},
      {"metallic", T_FLOAT, F0},
      {"specular", T_FLOAT, "0.5"},
      {"normal", T_VEC3, TN},
      {"ao", T_FLOAT, F1},
      {"emissive", T_VEC3, V3(F0)}},
     {{0}},
     {{0}},
     "s.base_color = saturate({base_color});\n"
     "s.roughness = saturate({roughness});\n"
     "s.metallic = saturate({metallic});\n"
     "s.specular = saturate({specular});\n"
     "s.ao = saturate({ao});\n"
     "s.emissive = max({emissive}, float3(0.0, 0.0, 0.0));\n"
     "de_mg_apply_normal(s, m, v, {normal});",
     0},
    {"output.clearcoat",
     "Clearcoat",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"weight", T_FLOAT, F1}, {"roughness", T_FLOAT, "0.1"}, {"normal", T_VEC3, TN}},
     {{0}},
     {{0}},
     "s.clearcoat = saturate({weight});\n"
     "s.clearcoat_roughness = saturate({roughness});\n"
     "s.clearcoat_normal = de_mg_world_normal(m, v, {normal});",
     0x1u},
    {"output.sheen",
     "Sheen",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"weight", T_FLOAT, F1}, {"color", T_VEC3, V3(F1)}, {"roughness", T_FLOAT, "0.5"}},
     {{0}},
     {{0}},
     "s.sheen_color = saturate({color}) * saturate({weight});\n"
     "s.sheen_roughness = saturate({roughness});",
     0x2u},
    {"output.anisotropy",
     "Anisotropy",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"strength", T_FLOAT, "0.5"}, {"direction", T_VEC2, "float2(1.0, 0.0)"}},
     {{0}},
     {{0}},
     "s.anisotropy = clamp({strength}, -1.0, 1.0);\n"
     "s.anisotropy_direction = de_mg_direction({direction});",
     0x4u},
    {"output.iridescence",
     "Iridescence",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"weight", T_FLOAT, F1}, {"ior", T_FLOAT, "1.3"}, {"thickness", T_FLOAT, "400.0"}},
     {{0}},
     {{0}},
     "s.iridescence = saturate({weight});\n"
     "s.iridescence_ior = max({ior}, 1.0);\n"
     "s.iridescence_thickness = max({thickness}, 0.0);",
     0x8u},
    {"output.diffuse_transmission",
     "Diffuse Transmission",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"weight", T_FLOAT, "0.5"}, {"color", T_VEC3, V3(F1)}},
     {{0}},
     {{0}},
     "s.diffuse_transmission = saturate({weight});\n"
     "s.diffuse_transmission_color = saturate({color});",
     0x40u},
    /* DENG-772. Specular transmission: the forward pass draws it (the material pack adds FORWARD
     * for a graph model with TRANSMISSION). */
    {"output.transmission",
     "Transmission",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"weight", T_FLOAT, F1}},
     {{0}},
     {{0}},
     "s.transmission = saturate({weight});",
     0x10u},
    /* The volume under transmission or diffuse transmission. Thickness is in object units, scaled
     * to world units by the instance's uniform-scale estimate, as the Standard model does. */
    {"output.volume",
     "Volume",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{"thickness", T_FLOAT, "0.1"},
      {"attenuation_color", T_VEC3, V3(F1)},
      {"attenuation_distance", T_FLOAT, "1.0e30"}},
     {{0}},
     {{0}},
     "s.thickness = max({thickness}, 0.0) * v.object_scale;\n"
     "s.attenuation_color = saturate({attenuation_color});\n"
     "s.attenuation_distance = max({attenuation_distance}, 1.0e-4);",
     0x20u},
    /* Subsurface scattering. SurfaceData has no subsurface fields: the screen-space SSS pass reads
     * the amount (extra.z) and mean free path (subsurface.xyz, metres) from the material, so the
     * node's values become the parameters subsurface.weight and subsurface.radius in exactly those
     * cells (see layout). The generated resolve only sets the feature bit. */
    {"output.subsurface",
     "Subsurface",
     "Output",
     R_OUTPUT,
     0,
     0,
     {{0}},
     {{0}},
     {{"weight", V_FLOAT, 1.0f, NULL},
      {"radius_r", V_FLOAT, 1.0f, NULL},
      {"radius_g", V_FLOAT, 1.0f, NULL},
      {"radius_b", V_FLOAT, 1.0f, NULL}},
     "// subsurface: the material parameters subsurface.weight and subsurface.radius",
     0x80u},
};
#define MG_NODE_DEFS (sizeof k_nodes / sizeof k_nodes[0])

/* DENG-772: the material cells the SSS pass reads (DeGpuMaterial extra.z, subsurface.xyz). */
#define MG_ROW_EXTRA                    7u
#define MG_ROW_SUBSURFACE               9u
#define MG_SSS_WEIGHT                   "subsurface.weight"
#define MG_SSS_RADIUS                   "subsurface.radius"
#define MG_FEATURE_TRANSMISSION         0x10u
#define MG_FEATURE_DIFFUSE_TRANSMISSION 0x40u

static const char *const k_rows[DE__MG_PARAM_ROWS] = {
    "base_color", "emissive", "pbr",   "specular",    "clearcoat",
    "sheen",      "volume",   "extra", "iridescence", "subsurface"};

/* Slang helpers every generated module carries (names de_mg_*). */
static const char *const k_prelude[] = {
    "struct DeMgUv\n"
    "{\n"
    "    float2 uv;\n"
    "    float2 dx;\n"
    "    float2 dy;\n"
    "};\n"
    "\n",
    "DeMgUv de_mg_uv(VertexAttribs v, TextureLodInfo lod, uint set)\n"
    "{\n"
    "    DeMgUv r;\n"
    "    r.uv = v.uv[set];\n"
    "    r.dx = lod.dx[set];\n"
    "    r.dy = lod.dy[set];\n"
    "    return r;\n"
    "}\n"
    "\n",
    "DeMgUv de_mg_uv_transform(DeMgUv u, float2 scale, float2 offset, float rotation)\n"
    "{\n"
    "    float c = cos(rotation);\n"
    "    float n = sin(rotation);\n"
    "    float2x2 r = float2x2(c, -n, n, c);\n"
    "    DeMgUv o;\n"
    "    o.uv = mul(r, u.uv * scale) + offset;\n"
    "    o.dx = mul(r, u.dx * scale);\n"
    "    o.dy = mul(r, u.dy * scale);\n"
    "    return o;\n"
    "}\n"
    "\n",
    "float4 de_mg_sample(DeGpuMaterial m, uint slot, DeMgUv u, TextureLodInfo lod)\n"
    "{\n"
    "    uint t = de_material_texture(m, slot);\n"
    "    if (t == DE_BINDLESS_INVALID)\n"
    "        return float4(1.0, 1.0, 1.0, 1.0);\n"
    "    if (lod.record)\n"
    "        de_feedback_record(lod.feedback, lod.feedback_capacity, t, u.dx, u.dy);\n"
    "    return de_tex2d(t).SampleGrad(de_sampler(lod.sampler), u.uv, u.dx, u.dy);\n"
    "}\n"
    "\n",
    "// World-space projections. The footprint is the world-position difference of one pixel\n"
    "// (dp_dx, dp_dy), projected onto each plane like the position, so it needs no UVs.\n"
    "float4 de_mg_triplanar(DeGpuMaterial m, uint slot, float3 p, float3 n, float scale,\n"
    "                       float sharpness, float3 dp_dx, float3 dp_dy, TextureLodInfo lod)\n"
    "{\n"
    "    float3 w = pow(abs(n), max(sharpness, 1.0));\n"
    "    w /= max(w.x + w.y + w.z, 1e-5);\n"
    "    DeMgUv u;\n"
    "    u.dx = dp_dx.zy * scale;\n"
    "    u.dy = dp_dy.zy * scale;\n"
    "    u.uv = p.zy * scale;\n"
    "    float4 r = w.x * de_mg_sample(m, slot, u, lod);\n"
    "    u.dx = dp_dx.xz * scale;\n"
    "    u.dy = dp_dy.xz * scale;\n"
    "    u.uv = p.xz * scale;\n"
    "    r += w.y * de_mg_sample(m, slot, u, lod);\n"
    "    u.dx = dp_dx.xy * scale;\n"
    "    u.dy = dp_dy.xy * scale;\n"
    "    u.uv = p.xy * scale;\n"
    "    r += w.z * de_mg_sample(m, slot, u, lod);\n"
    "    return r;\n"
    "}\n"
    "\n",
    "float de_mg_divide(float a, float b)\n"
    "{\n"
    "    return abs(b) > 1e-20 ? a / b : 0.0;\n"
    "}\n"
    "\n",
    "float de_mg_remap(float x, float in_min, float in_max, float out_min, float out_max)\n"
    "{\n"
    "    float d = in_max - in_min;\n"
    "    return lerp(out_min, out_max, (x - in_min) / (abs(d) > 1e-8 ? d : 1e-8));\n"
    "}\n"
    "\n",
    "float3 de_mg_normalize(float3 v)\n"
    "{\n"
    "    return dot(v, v) > 1e-20 ? normalize(v) : float3(0.0, 0.0, 1.0);\n"
    "}\n"
    "\n",
    "float2 de_mg_direction(float2 v)\n"
    "{\n"
    "    return dot(v, v) > 1e-20 ? normalize(v) : float2(1.0, 0.0);\n"
    "}\n"
    "\n",
    "float de_mg_hash(float3 p)\n"
    "{\n"
    "    p = frac(p * 0.3183099 + 0.1) * 17.0;\n"
    "    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));\n"
    "}\n"
    "\n",
    "float3 de_mg_hash3(float3 p)\n"
    "{\n"
    "    p = float3(dot(p, float3(127.1, 311.7, 74.7)), dot(p, float3(269.5, 183.3, 246.1)),\n"
    "               dot(p, float3(113.5, 271.9, 124.6)));\n"
    "    return frac(sin(p) * 43758.5453);\n"
    "}\n"
    "\n",
    "float de_mg_value_noise(float3 x)\n"
    "{\n"
    "    float3 i = floor(x);\n"
    "    float3 f = frac(x);\n"
    "    float3 u = f * f * (3.0 - 2.0 * f);\n"
    "    float a = lerp(de_mg_hash(i), de_mg_hash(i + float3(1.0, 0.0, 0.0)), u.x);\n"
    "    float b = lerp(de_mg_hash(i + float3(0.0, 1.0, 0.0)), de_mg_hash(i + float3(1.0, 1.0, "
    "0.0)), u.x);\n"
    "    float c = lerp(de_mg_hash(i + float3(0.0, 0.0, 1.0)), de_mg_hash(i + float3(1.0, 0.0, "
    "1.0)), u.x);\n"
    "    float d = lerp(de_mg_hash(i + float3(0.0, 1.0, 1.0)), de_mg_hash(i + float3(1.0, 1.0, "
    "1.0)), u.x);\n"
    "    return lerp(lerp(a, b, u.y), lerp(c, d, u.y), u.z);\n"
    "}\n"
    "\n",
    "float de_mg_corner(float3 i, float3 f, float3 c)\n"
    "{\n"
    "    return dot(de_mg_hash3(i + c) * 2.0 - 1.0, f - c);\n"
    "}\n"
    "\n",
    "// Gradient (Perlin-style) noise, mapped to 0..1.\n"
    "float de_mg_gradient_noise(float3 x)\n"
    "{\n"
    "    float3 i = floor(x);\n"
    "    float3 f = frac(x);\n"
    "    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);\n"
    "    float a = lerp(de_mg_corner(i, f, float3(0.0, 0.0, 0.0)),\n"
    "                   de_mg_corner(i, f, float3(1.0, 0.0, 0.0)), u.x);\n"
    "    float b = lerp(de_mg_corner(i, f, float3(0.0, 1.0, 0.0)),\n"
    "                   de_mg_corner(i, f, float3(1.0, 1.0, 0.0)), u.x);\n"
    "    float c = lerp(de_mg_corner(i, f, float3(0.0, 0.0, 1.0)),\n"
    "                   de_mg_corner(i, f, float3(1.0, 0.0, 1.0)), u.x);\n"
    "    float d = lerp(de_mg_corner(i, f, float3(0.0, 1.0, 1.0)),\n"
    "                   de_mg_corner(i, f, float3(1.0, 1.0, 1.0)), u.x);\n"
    "    return saturate(0.5 + 0.5 * lerp(lerp(a, b, u.y), lerp(c, d, u.y), u.z));\n"
    "}\n"
    "\n",
    "// Distance to the nearest feature point (F1).\n"
    "float de_mg_worley(float3 x)\n"
    "{\n"
    "    float3 i = floor(x);\n"
    "    float3 f = frac(x);\n"
    "    float d = 8.0;\n"
    "    for (int z = -1; z <= 1; ++z)\n"
    "        for (int y = -1; y <= 1; ++y)\n"
    "            for (int k = -1; k <= 1; ++k)\n"
    "            {\n"
    "                float3 c = float3(float(k), float(y), float(z));\n"
    "                float3 r = c + de_mg_hash3(i + c) - f;\n"
    "                d = min(d, dot(r, r));\n"
    "            }\n"
    "    return sqrt(d);\n"
    "}\n"
    "\n",
    "float de_mg_fresnel(float3 n, float3 view, float power)\n"
    "{\n"
    "    return pow(1.0 - saturate(dot(de_mg_normalize(n), view)), max(power, 0.0));\n"
    "}\n"
    "\n",
    "float3 de_mg_normal_unpack(float3 rgb, float strength)\n"
    "{\n"
    "    float3 n = rgb * 2.0 - 1.0;\n"
    "    n.xy *= strength;\n"
    "    return normalize(float3(n.xy, max(n.z, 1e-4)));\n"
    "}\n"
    "\n",
    "float3 de_mg_normal_blend(float3 a, float3 b)\n"
    "{\n"
    "    return normalize(float3(a.xy + b.xy, a.z * b.z));\n"
    "}\n"
    "\n",
    "// 0 where layer a wins, 1 where layer b wins; `t` biases towards b, `contrast` sets the "
    "edge.\n"
    "float de_mg_height_mask(float height_a, float height_b, float t, float contrast)\n"
    "{\n"
    "    float a = height_a + (1.0 - t);\n"
    "    float b = height_b + t;\n"
    "    float top = max(a, b) - max(contrast, 1e-4);\n"
    "    float wa = max(a - top, 0.0);\n"
    "    float wb = max(b - top, 0.0);\n"
    "    return wb / max(wa + wb, 1e-5);\n"
    "}\n"
    "\n",
    "// Tangent-space normal to world space, with the double-sided flip of the standard model.\n"
    "float3 de_mg_world_normal(DeGpuMaterial m, VertexAttribs v, float3 tn)\n"
    "{\n"
    "    float3 n = v.normal;\n"
    "    float4 t = v.tangent;\n"
    "    if ((m.flags & DE_GPU_MATERIAL_DOUBLE_SIDED) != 0u && dot(v.geometric_normal, v.view) "
    "< 0.0)\n"
    "    {\n"
    "        n = -n;\n"
    "        t.w = -t.w;\n"
    "    }\n"
    "    if (!v.has_tangent)\n"
    "        return n;\n"
    "    float3 tt = normalize(t.xyz - n * dot(n, t.xyz));\n"
    "    float3 b = cross(n, tt) * t.w;\n"
    "    return normalize(tt * tn.x + b * tn.y + n * tn.z);\n"
    "}\n"
    "\n",
    "void de_mg_apply_normal(inout SurfaceData s, DeGpuMaterial m, VertexAttribs v, float3 tn)\n"
    "{\n"
    "    s.normal = de_mg_world_normal(m, v, tn);\n"
    "    s.tangent = v.has_tangent ? v.tangent : float4(0.0, 0.0, 0.0, 1.0);\n"
    "}\n"
    "\n"};

/* ---- State ---- */

#define MG_NONE 0xFFFFFFFFu

typedef struct mg_node {
    char path[64];
    char var[64];
    i32 def; /* index into k_nodes, -1 unknown */
    toml_datum_t values;
    u32 line;
    u32 in_src[MG_PINS]; /* source node, MG_NONE unconnected */
    u8 in_src_pin[MG_PINS];
    u8 in_type[MG_PINS];
    u8 out_type[MG_OUTS];
    u32 param; /* index + 1 into params */
    u32 indeg;
    bool bad;   /* its types are unknown: checks downstream are skipped */
    bool done;  /* ordered */
    bool reach; /* feeds an output */
} mg_node;

typedef struct mg {
    const de_allocator *a;
    de__mg_result *r;
    mg_node *nodes;
    u32 count;
    u32 links;
    u32 *order; /* topological order */
    u32 order_count;
    u32 *sorted; /* by id */
    de_strbuf src;
    u32 *lines;
    u32 line_cap;
    bool oom;
} mg;

static void *mg_alloc(const de_allocator *a, size_t size) {
    void *p = a->alloc(a->user, size, 16, DE_MEM_ASSET);
    if (p)
        memset(p, 0, size);
    return p;
}

static void mg_free(const de_allocator *a, void *p, size_t size) {
    if (p)
        a->free(a->user, p, size);
}

static void copy_str(char *dst, size_t cap, const char *src) {
    size_t n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = 0;
}

static void diag(mg *g, de__mg_severity sev, const char *node, const char *pin, u32 line,
                 const char *fmt, ...) DE_PRINTF_FMT(6, 7);

static void diag(mg *g, de__mg_severity sev, const char *node, const char *pin, u32 line,
                 const char *fmt, ...) {
    de__mg_result *r = g->r;
    if (sev == DE__MG_ERROR)
        r->error_count++;
    if (r->diag_count == r->diag_cap_) {
        u32 cap = r->diag_cap_ ? r->diag_cap_ * 2 : 8;
        if (cap > 1024u) { /* enough to act on; the count stays right */
            return;
        }
        de__mg_diag *d = mg_alloc(g->a, cap * sizeof *d);
        if (!d) {
            g->oom = true;
            return;
        }
        if (r->diag_count)
            memcpy(d, r->diags, r->diag_count * sizeof *d);
        mg_free(g->a, r->diags, r->diag_cap_ * sizeof *d);
        r->diags = d;
        r->diag_cap_ = cap;
    }
    de__mg_diag *d = &r->diags[r->diag_count++];
    d->severity = sev;
    copy_str(d->node, sizeof d->node, node);
    copy_str(d->pin, sizeof d->pin, pin);
    d->graph_line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof d->message, fmt, ap);
    va_end(ap);
}

#define ERR(g, node, pin, line, ...)  diag((g), DE__MG_ERROR, (node), (pin), (line), __VA_ARGS__)
#define WARN(g, node, pin, line, ...) diag((g), DE__MG_WARNING, (node), (pin), (line), __VA_ARGS__)

static i32 find_def(const char *id) {
    for (u32 i = 0; i < MG_NODE_DEFS; ++i)
        if (strcmp(k_nodes[i].id, id) == 0)
            return (i32)i;
    return -1;
}

static int pin_index(const pin_def *p, const char *name) {
    for (int i = 0; i < MG_PINS && p[i].name; ++i)
        if (strcmp(p[i].name, name) == 0)
            return i;
    return -1;
}

static int out_index(const out_def *o, const char *name) {
    for (int i = 0; i < MG_OUTS && o[i].name; ++i)
        if (strcmp(o[i].name, name) == 0)
            return i;
    return -1;
}

static int pin_count(const node_def *d) {
    int n = 0;
    while (n < MG_PINS && d->in[n].name)
        ++n;
    return n;
}

static int out_count(const node_def *d) {
    int n = 0;
    while (n < MG_OUTS && d->out[n].name)
        ++n;
    return n;
}

static const val_def *find_val(const node_def *d, const char *name) {
    for (int i = 0; i < MG_VALS && d->val[i].name; ++i)
        if (strcmp(d->val[i].name, name) == 0)
            return &d->val[i];
    return NULL;
}

/* Compares two node paths segment by segment; "n<digits>" segments compare by number. */
static int seg_cmp(const char *a, size_t al, const char *b, size_t bl) {
    bool an = al > 1 && a[0] == 'n', bn = bl > 1 && b[0] == 'n';
    for (size_t i = 1; an && i < al; ++i)
        an = a[i] >= '0' && a[i] <= '9';
    for (size_t i = 1; bn && i < bl; ++i)
        bn = b[i] >= '0' && b[i] <= '9';
    if (an && bn) {
        while (al > 2 && a[1] == '0')
            ++a, --al;
        while (bl > 2 && b[1] == '0')
            ++b, --bl;
        if (al != bl)
            return al < bl ? -1 : 1;
    } else if (an != bn) {
        return an ? -1 : 1;
    }
    size_t n = al < bl ? al : bl;
    int c = memcmp(a, b, n);
    return c ? c : (al < bl ? -1 : al > bl ? 1 : 0);
}

static int path_cmp(const char *a, const char *b) {
    for (;;) {
        size_t al = strcspn(a, "/"), bl = strcspn(b, "/");
        int c = seg_cmp(a, al, b, bl);
        if (c)
            return c;
        a += al;
        b += bl;
        if (!*a || !*b)
            return *a ? 1 : *b ? -1 : 0;
        ++a;
        ++b;
    }
}

static int sorted_cmp(const void *x, const void *y, void *ctx) {
    const mg_node *n = ctx;
    return path_cmp(n[*(const u32 *)x].path, n[*(const u32 *)y].path);
}

/* Insertion sort with a context: small inputs, and qsort has no portable context form. */
static void sort_ids(u32 *ids, u32 count, const mg_node *nodes) {
    for (u32 i = 1; i < count; ++i) {
        u32 v = ids[i], j = i;
        while (j > 0 && sorted_cmp(&v, &ids[j - 1], (void *)nodes) < 0) {
            ids[j] = ids[j - 1];
            --j;
        }
        ids[j] = v;
    }
}

static u32 find_node(const mg *g, const char *path) {
    for (u32 i = 0; i < g->count; ++i)
        if (strcmp(g->nodes[i].path, path) == 0)
            return i;
    return MG_NONE;
}

static bool valid_id(const char *s, int len) {
    if (len < 1 || len > 15)
        return false;
    for (int i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

static u32 datum_line(toml_datum_t d) {
    return d.lineno > 0 ? (u32)d.lineno : 0;
}

/* ---- Parse and flatten ---- */

static void collect(mg *g, toml_datum_t level, const char *prefix, u32 depth) {
    toml_datum_t list = toml_get(level, "node");
    if (list.type == 0)
        return;
    if (list.type != TOML_ARRAY) {
        ERR(g, prefix, "", datum_line(list), "`node` must be an array of tables");
        return;
    }
    for (i32 i = 0; i < list.u.arr.size; ++i) {
        toml_datum_t t = list.u.arr.elem[i];
        if (t.type != TOML_TABLE) {
            ERR(g, prefix, "", datum_line(t), "a node entry is not a table");
            continue;
        }
        toml_datum_t id = toml_get(t, "id"), type = toml_get(t, "type");
        u32 line = datum_line(id) ? datum_line(id) : datum_line(t);
        if (id.type != TOML_STRING || !valid_id(id.u.str.ptr, id.u.str.len)) {
            ERR(g, prefix, "", line, "a node needs an `id` of 1 to 15 letters and digits");
            continue;
        }
        char path[64];
        int n = snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", id.u.s);
        if (n < 0 || (size_t)n >= sizeof path) {
            ERR(g, prefix, "", line, "node path is too long (subgraphs nest too deep)");
            continue;
        }
        if (find_node(g, path) != MG_NONE) {
            ERR(g, path, "", line, "duplicate node id");
            continue;
        }
        if (g->count == DE__MG_MAX_NODES) {
            ERR(g, path, "", line, "the graph has more than %u nodes", DE__MG_MAX_NODES);
            return;
        }
        mg_node *nd = &g->nodes[g->count++];
        copy_str(nd->path, sizeof nd->path, path);
        snprintf(nd->var, sizeof nd->var, "_%s", path);
        for (char *c = nd->var; *c; ++c)
            if (*c == '/')
                *c = '_';
        nd->line = line;
        nd->values = toml_get(t, "values");
        for (int p = 0; p < MG_PINS; ++p)
            nd->in_src[p] = MG_NONE;
        if (nd->values.type != 0 && nd->values.type != TOML_TABLE) {
            ERR(g, path, "", line, "`values` must be a table");
            nd->values.type = 0;
        }
        if (type.type != TOML_STRING) {
            ERR(g, path, "", line, "node has no `type`");
            nd->def = -1;
            nd->bad = true;
            continue;
        }
        nd->def = find_def(type.u.s);
        if (nd->def < 0) {
            ERR(g, path, "", line, "unknown node type '%.64s'", type.u.s);
            nd->bad = true;
            continue;
        }
        if (k_nodes[nd->def].role == R_SUBGRAPH) {
            toml_datum_t inner = toml_get(t, "graph");
            if (inner.type != TOML_TABLE) {
                ERR(g, path, "", line, "subgraph node has no `graph` table");
                nd->bad = true;
            } else if (depth + 1 >= DE__MG_MAX_DEPTH) {
                ERR(g, path, "", line, "subgraphs nest deeper than %u", DE__MG_MAX_DEPTH);
                nd->bad = true;
            } else {
                char sub[64];
                copy_str(sub, sizeof sub, path);
                collect(g, inner, sub, depth + 1);
            }
        }
    }
}

typedef struct endpoint {
    u32 node;
    int pin; /* -1: not resolved */
} endpoint;

/* Resolves "n4.pin" at `prefix`. Reports problems; node MG_NONE when it cannot be used. */
static endpoint resolve(mg *g, const char *prefix, const char *text, bool source, u32 line) {
    endpoint e = {MG_NONE, -1};
    const char *dot = strchr(text, '.');
    if (!dot || dot == text || !dot[1] || !valid_id(text, (int)(dot - text))) {
        ERR(g, prefix, "", line, "link endpoint '%.64s' is not \"<node>.<pin>\"", text);
        return e;
    }
    char path[64], pin[32];
    int n = snprintf(path, sizeof path, "%s%s%.*s", prefix, *prefix ? "/" : "", (int)(dot - text),
                     text);
    if (n < 0 || (size_t)n >= sizeof path || strlen(dot + 1) >= sizeof pin) {
        ERR(g, prefix, "", line, "link endpoint '%.64s' is too long", text);
        return e;
    }
    copy_str(pin, sizeof pin, dot + 1);
    u32 idx = find_node(g, path);
    if (idx == MG_NONE) {
        ERR(g, path, pin, line, "a link names %s, but there is no node %s", path, path);
        return e;
    }
    mg_node *nd = &g->nodes[idx];
    if (nd->def < 0)
        return e; /* already reported */
    const node_def *d = &k_nodes[nd->def];
    if (d->role == R_SUBGRAPH) {
        char inner[64];
        u32 in = MG_NONE;
        if (valid_id(pin, (int)strlen(pin)) &&
            snprintf(inner, sizeof inner, "%s/%s", path, pin) < (int)sizeof inner)
            in = find_node(g, inner);
        u8 want = source ? R_SUB_OUT : R_SUB_IN;
        if (in == MG_NONE || g->nodes[in].def < 0 || k_nodes[g->nodes[in].def].role != want) {
            ERR(g, path, pin, line, "'%s' is not a %s of this subgraph", pin,
                source ? "subgraph.output" : "subgraph.input");
            return e;
        }
        e.node = in;
        e.pin = 0; /* "out" of a subgraph.output, "in" of a subgraph.input */
        return e;
    }
    /* DENG-773: DIDE names both subgraph boundary pins "value". */
    if (strcmp(pin, "value") == 0) {
        if (source && d->role == R_SUB_IN)
            copy_str(pin, sizeof pin, "out");
        else if (!source && d->role == R_SUB_OUT)
            copy_str(pin, sizeof pin, "in");
    }
    if (source) {
        int p = d->hidden_outputs ? -1 : out_index(d->out, pin);
        if (p < 0) {
            ERR(g, path, pin, line, "%s has no output pin '%s'", d->id, pin);
            return e;
        }
        e.pin = p;
    } else {
        int p = d->hidden_inputs ? -1 : pin_index(d->in, pin);
        if (p < 0) {
            ERR(g, path, pin, line, "%s has no input pin '%s'", d->id, pin);
            return e;
        }
        e.pin = p;
    }
    e.node = idx;
    return e;
}

static void connect(mg *g, toml_datum_t level, const char *prefix) {
    toml_datum_t list = toml_get(level, "link");
    if (list.type != 0 && list.type != TOML_ARRAY)
        ERR(g, prefix, "", datum_line(list), "`link` must be an array of tables");
    for (i32 i = 0; list.type == TOML_ARRAY && i < list.u.arr.size; ++i) {
        toml_datum_t t = list.u.arr.elem[i];
        toml_datum_t from = toml_get(t, "from"), to = toml_get(t, "to");
        u32 line = datum_line(from) ? datum_line(from) : datum_line(t);
        if (t.type != TOML_TABLE || from.type != TOML_STRING || to.type != TOML_STRING) {
            ERR(g, prefix, "", line, "a link needs `from` and `to` strings");
            continue;
        }
        if (++g->links > DE__MG_MAX_LINKS) {
            ERR(g, prefix, "", line, "the graph has more than %u links", DE__MG_MAX_LINKS);
            return;
        }
        endpoint s = resolve(g, prefix, from.u.s, true, line);
        endpoint d = resolve(g, prefix, to.u.s, false, line);
        if (s.node == MG_NONE || d.node == MG_NONE)
            continue;
        mg_node *dn = &g->nodes[d.node];
        if (dn->in_src[d.pin] != MG_NONE) {
            ERR(g, dn->path, k_nodes[dn->def].in[d.pin].name, line,
                "more than one link into this pin");
            continue;
        }
        dn->in_src[d.pin] = s.node;
        dn->in_src_pin[d.pin] = (u8)s.pin;
    }
    /* Then the subgraphs of this level. */
    toml_datum_t nodes = toml_get(level, "node");
    for (i32 i = 0; nodes.type == TOML_ARRAY && i < nodes.u.arr.size; ++i) {
        toml_datum_t t = nodes.u.arr.elem[i];
        toml_datum_t id = toml_get(t, "id"), inner = toml_get(t, "graph");
        if (t.type != TOML_TABLE || id.type != TOML_STRING || inner.type != TOML_TABLE)
            continue;
        char path[64];
        int n = snprintf(path, sizeof path, "%s%s%s", prefix, *prefix ? "/" : "", id.u.s);
        if (n < 0 || (size_t)n >= sizeof path)
            continue;
        u32 idx = find_node(g, path);
        if (idx == MG_NONE || g->nodes[idx].def < 0 ||
            k_nodes[g->nodes[idx].def].role != R_SUBGRAPH || g->nodes[idx].bad)
            continue;
        connect(g, inner, path);
    }
}

/* ---- Order, cycles, types ---- */

static void order_nodes(mg *g) {
    for (u32 i = 0; i < g->count; ++i) {
        mg_node *n = &g->nodes[i];
        for (int p = 0; p < MG_PINS; ++p)
            n->indeg += n->in_src[p] != MG_NONE;
    }
    for (;;) {
        u32 pick = MG_NONE;
        for (u32 k = 0; k < g->count; ++k) { /* sorted: the first ready one has the least id */
            u32 i = g->sorted[k];
            if (!g->nodes[i].done && g->nodes[i].indeg == 0) {
                pick = i;
                break;
            }
        }
        if (pick == MG_NONE)
            break;
        g->nodes[pick].done = true;
        g->order[g->order_count++] = pick;
        for (u32 i = 0; i < g->count; ++i)
            for (int p = 0; p < MG_PINS; ++p)
                if (g->nodes[i].in_src[p] == pick)
                    g->nodes[i].indeg--;
    }
    if (g->order_count == g->count)
        return;
    /* Walk back from the least remaining node until a node repeats: that is on a cycle. */
    u32 at = MG_NONE;
    for (u32 k = 0; k < g->count && at == MG_NONE; ++k)
        if (!g->nodes[g->sorted[k]].done)
            at = g->sorted[k];
    for (u32 steps = 0; steps <= g->count; ++steps) {
        mg_node *n = &g->nodes[at];
        u32 next = MG_NONE;
        for (int p = 0; p < MG_PINS && next == MG_NONE; ++p)
            if (n->in_src[p] != MG_NONE && !g->nodes[n->in_src[p]].done)
                next = n->in_src[p];
        at = next;
    }
    u32 start = at;
    do {
        mg_node *n = &g->nodes[at];
        ERR(g, n->path, "", n->line, "node is part of a cycle");
        u32 next = MG_NONE;
        for (int p = 0; p < MG_PINS && next == MG_NONE; ++p)
            if (n->in_src[p] != MG_NONE && !g->nodes[n->in_src[p]].done)
                next = n->in_src[p];
        at = next;
    } while (at != start && at != MG_NONE);
}

static bool converts(u8 from, u8 to) {
    if (from == to)
        return true;
    for (u32 i = 0; i < sizeof k_conversions / sizeof k_conversions[0]; ++i)
        if (k_conversions[i][0] == from && k_conversions[i][1] == to)
            return true;
    return false;
}

static bool finite_num(toml_datum_t d, double *out) {
    if (d.type == TOML_FP64)
        *out = d.u.fp64;
    else if (d.type == TOML_INT64)
        *out = (double)d.u.int64;
    else
        return false;
    return isfinite(*out) && fabs(*out) < 1e30;
}

/* Checks a constant for unconnected pin `p` of `n` (if `values` has one). */
static void check_constant(mg *g, const mg_node *n, const pin_def *p) {
    toml_datum_t c =
        n->values.type == TOML_TABLE ? toml_get(n->values, p->name) : (toml_datum_t){0};
    if (c.type == 0)
        return;
    double x;
    if (p->type == T_BOOL) {
        if (c.type != TOML_BOOLEAN && !finite_num(c, &x))
            ERR(g, n->path, p->name, n->line, "the value for bool pin '%s' is not a bool", p->name);
        return;
    }
    if (p->type == T_UV) {
        ERR(g, n->path, p->name, n->line, "uv pin '%s' takes a link, not a value", p->name);
        return;
    }
    if (finite_num(c, &x))
        return;
    u8 w = p->type == T_ANY ? 1 : k_type_width[p->type];
    if (c.type == TOML_ARRAY && c.u.arr.size == (i32)w) {
        bool ok = true;
        for (u32 i = 0; i < w; ++i)
            ok = ok && finite_num(c.u.arr.elem[i], &x);
        if (ok)
            return;
    }
    ERR(g, n->path, p->name, n->line, "the value for %s pin '%s' must be a number%s",
        k_type_name[p->type == T_ANY ? T_FLOAT : p->type], p->name,
        w > 1 ? " or an array of that many numbers" : "");
}

static bool printable_name(const char *s, int len) {
    if (len < 1 || len > 63)
        return false;
    for (int i = 0; i < len; ++i)
        if ((unsigned char)s[i] < 0x20 || s[i] == '"' || s[i] == '\\' || s[i] == 0x7f)
            return false;
    return true;
}

static void check_values(mg *g, const mg_node *n, const node_def *d) {
    for (int i = 0; i < MG_VALS && d->val[i].name; ++i) {
        const val_def *v = &d->val[i];
        toml_datum_t x =
            n->values.type == TOML_TABLE ? toml_get(n->values, v->name) : (toml_datum_t){0};
        double num;
        switch (v->kind) {
        case V_FLOAT:
            if (x.type != 0 && !finite_num(x, &num))
                ERR(g, n->path, v->name, n->line, "value '%s' must be a finite number", v->name);
            break;
        case V_BOOL:
            if (x.type != 0 && x.type != TOML_BOOLEAN && !finite_num(x, &num))
                ERR(g, n->path, v->name, n->line, "value '%s' must be a bool", v->name);
            break;
        case V_NAME:
            if (x.type != TOML_STRING || !printable_name(x.u.str.ptr, x.u.str.len))
                ERR(g, n->path, v->name, n->line,
                    "value '%s' must be a name of 1 to 63 printable characters", v->name);
            break;
        case V_OP: {
            static const char *const ops[] = {"<", "<=", ">", ">=", "==", "!="};
            bool ok = x.type == 0;
            for (u32 k = 0; !ok && x.type == TOML_STRING && k < 6; ++k)
                ok = strcmp(x.u.s, ops[k]) == 0;
            if (!ok)
                ERR(g, n->path, v->name, n->line,
                    "value '%s' must be one of < <= > >= == !=", v->name);
            break;
        }
        case V_CODE: {
            const char *why = NULL;
            if (x.type != 0 && x.type != TOML_STRING)
                why = "must be a string";
            else if (x.type == TOML_STRING) {
                if (x.u.str.len < 1 || x.u.str.len > 512)
                    why = "must be 1 to 512 characters";
                else if (strpbrk(x.u.s, ";{}#\"'\\`\n\r") || strstr(x.u.s, "//") ||
                         strstr(x.u.s, "/*"))
                    why = "must be one expression (no ; { } # quotes, comments or newlines)";
                for (int k = 0; !why && k < x.u.str.len; ++k)
                    if ((unsigned char)x.u.s[k] < 0x20 || (unsigned char)x.u.s[k] >= 0x7f)
                        why = "must be printable ASCII";
            }
            if (why)
                ERR(g, n->path, v->name, n->line, "code %s", why);
            break;
        }
        }
    }
}

static void check_types(mg *g) {
    for (u32 k = 0; k < g->order_count; ++k) {
        mg_node *n = &g->nodes[g->order[k]];
        if (n->def < 0)
            continue;
        const node_def *d = &k_nodes[n->def];
        bool upstream_bad = false;
        for (int p = 0; p < pin_count(d); ++p) {
            const pin_def *pd = &d->in[p];
            n->in_type[p] = pd->type == T_ANY ? T_FLOAT : pd->type;
            if (n->in_src[p] == MG_NONE) {
                check_constant(g, n, pd);
                continue;
            }
            const mg_node *s = &g->nodes[n->in_src[p]];
            if (s->bad) {
                upstream_bad = true;
                continue;
            }
            u8 st = s->out_type[n->in_src_pin[p]];
            if (pd->type == T_ANY) {
                n->in_type[p] = st;
            } else if (!converts(st, pd->type)) {
                const node_def *sd = &k_nodes[s->def];
                const char *sname = sd->role == R_SUB_OUT ? "out" : sd->out[n->in_src_pin[p]].name;
                ERR(g, n->path, pd->name, n->line,
                    "type mismatch: %s output %s.%s cannot connect to %s input '%s'",
                    k_type_name[st], s->path, sname, k_type_name[pd->type], pd->name);
                n->bad = true;
            }
        }
        check_values(g, n, d);
        if (upstream_bad)
            n->bad = true;
        for (int o = 0; o < out_count(d); ++o)
            n->out_type[o] = d->out[o].type == T_ANY ? n->in_type[0] : d->out[o].type;
    }
}

static void check_outputs(mg *g) {
    bool base = false;
    const mg_node *volume = NULL;
    for (u32 k = 0; k < g->count; ++k) {
        mg_node *n = &g->nodes[g->sorted[k]];
        if (n->def < 0 || k_nodes[n->def].role != R_OUTPUT)
            continue;
        const node_def *d = &k_nodes[n->def];
        for (u32 j = 0; j < k; ++j) {
            const mg_node *o = &g->nodes[g->sorted[j]];
            if (o->def == n->def) {
                ERR(g, n->path, "", n->line, "more than one %s node (the first is %s)", d->id,
                    o->path);
                break;
            }
        }
        base = base || strcmp(d->id, "output.base") == 0;
        if (strcmp(d->id, "output.volume") == 0 && !volume)
            volume = n;
        g->r->features |= d->features;
        n->reach = true;
    }
    if (!base)
        ERR(g, "", "", 0, "the graph has no output.base node");
    /* DENG-772: a volume only matters under transmission or diffuse transmission. */
    const u32 under = MG_FEATURE_TRANSMISSION | MG_FEATURE_DIFFUSE_TRANSMISSION;
    if (volume && (g->r->features & under) == 0u)
        WARN(g, volume->path, "", volume->line,
             "output.volume has no effect without output.transmission or "
             "output.diffuse_transmission");
    /* Reachability: back from the outputs, in reverse order. */
    for (u32 k = g->order_count; k-- > 0;) {
        const mg_node *n = &g->nodes[g->order[k]];
        if (!n->reach)
            continue;
        for (int p = 0; p < MG_PINS; ++p)
            if (n->in_src[p] != MG_NONE)
                g->nodes[n->in_src[p]].reach = true;
    }
    for (u32 k = 0; k < g->count; ++k) {
        const mg_node *n = &g->nodes[g->sorted[k]];
        if (!n->reach && n->def >= 0 && k_nodes[n->def].role != R_SUBGRAPH)
            WARN(g, n->path, "", n->line, "node does not reach an output");
    }
}

/* ---- Parameter layout ---- */

static double num_value(const mg_node *n, const char *name, double def) {
    double x;
    toml_datum_t d = n->values.type == TOML_TABLE ? toml_get(n->values, name) : (toml_datum_t){0};
    return finite_num(d, &x) ? x : def;
}

static const char *str_value(const mg_node *n, const char *name) {
    toml_datum_t d = n->values.type == TOML_TABLE ? toml_get(n->values, name) : (toml_datum_t){0};
    return d.type == TOML_STRING ? d.u.s : NULL;
}

/* The first `need` adjacent free components of a row (a bit per component); 4 if none. */
static u32 fit(u32 used, u32 need) {
    const u32 bits = (1u << need) - 1u;
    for (u32 c = 0; c + need <= 4; ++c)
        if ((used & (bits << c)) == 0u)
            return c;
    return 4;
}

/* Whether a parameter name is taken; reports the clash on `n` when it is. */
static bool name_taken(mg *g, const mg_node *n, const char *pin, const char *name) {
    for (u32 i = 0; i < g->r->param_count; ++i)
        if (strcmp(g->r->params[i].name, name) == 0) {
            ERR(g, n->path, pin, n->line, "the name '%s' is already used by %s", name,
                g->r->params[i].node);
            return true;
        }
    return false;
}

/* DENG-772: output.subsurface's values as the parameters in the material's subsurface cells
 * (reserved before packing, so other parameters fit around them). */
static void subsurface_params(mg *g, const mg_node *n) {
    de__mg_result *r = g->r;
    static const char *const radius[3] = {"radius_r", "radius_g", "radius_b"};
    const double weight = num_value(n, "weight", 1.0);
    bool ok = true;
    if (weight < 0.0 || weight > 1.0) {
        ERR(g, n->path, "weight", n->line, "value 'weight' must be 0 to 1");
        ok = false;
    }
    float rgb[3], top = 1.0f;
    for (int c = 0; c < 3; ++c) {
        const double x = num_value(n, radius[c], 1.0);
        if (x < 0.0) {
            ERR(g, n->path, radius[c], n->line, "value '%s' (metres) must not be negative",
                radius[c]);
            ok = false;
        }
        rgb[c] = (float)x;
        top = rgb[c] > top ? rgb[c] : top;
    }
    if (!ok || name_taken(g, n, "", MG_SSS_WEIGHT) || name_taken(g, n, "", MG_SSS_RADIUS))
        return;
    de__mg_param w = {0}, m = {0};
    copy_str(w.name, sizeof w.name, MG_SSS_WEIGHT);
    copy_str(w.node, sizeof w.node, n->path);
    w.kind = DE__MG_PARAM_FLOAT;
    w.row = MG_ROW_EXTRA;
    w.component = 2;
    w.value[0] = (float)weight;
    w.max = 1.0f;
    copy_str(m.name, sizeof m.name, MG_SSS_RADIUS);
    copy_str(m.node, sizeof m.node, n->path);
    m.kind = DE__MG_PARAM_COLOR;
    m.row = MG_ROW_SUBSURFACE;
    memcpy(m.value, rgb, sizeof rgb);
    m.max = top;
    r->params[r->param_count++] = w;
    r->params[r->param_count++] = m;
}

static void layout(mg *g) {
    de__mg_result *r = g->r;
    u32 used[DE__MG_PARAM_ROWS] = {0}; /* a bit per component */
    u32 textures = 0;
    for (u32 k = 0; k < g->count; ++k) {
        const mg_node *n = &g->nodes[g->sorted[k]];
        if (n->def >= 0 && strcmp(k_nodes[n->def].id, "output.subsurface") == 0) {
            used[MG_ROW_EXTRA] |= 1u << 2;
            used[MG_ROW_SUBSURFACE] |= 0x7u;
            break;
        }
    }
    bool sss = false;
    for (u32 k = 0; k < g->count; ++k) {
        mg_node *n = &g->nodes[g->sorted[k]];
        if (n->def < 0)
            continue;
        u8 role = k_nodes[n->def].role;
        if (role == R_OUTPUT && !sss && strcmp(k_nodes[n->def].id, "output.subsurface") == 0) {
            sss = true;
            subsurface_params(g, n);
            continue;
        }
        if (role != R_PARAM_FLOAT && role != R_PARAM_COLOR && role != R_TEXTURE)
            continue;
        const char *name = str_value(n, role == R_TEXTURE ? "texture" : "name");
        if (!name || !printable_name(name, (int)strlen(name)))
            continue; /* reported by check_values */
        u32 found = MG_NONE;
        for (u32 i = 0; i < r->param_count; ++i)
            if (strcmp(r->params[i].name, name) == 0)
                found = i;
        if (found != MG_NONE) {
            if (role == R_TEXTURE && r->params[found].kind == DE__MG_PARAM_TEXTURE) {
                n->param = found + 1;
                continue;
            }
            ERR(g, n->path, role == R_TEXTURE ? "texture" : "name", n->line,
                "the name '%s' is already used by %s", name, r->params[found].node);
            n->bad = true;
            continue;
        }
        de__mg_param p = {0};
        copy_str(p.name, sizeof p.name, name);
        copy_str(p.node, sizeof p.node, n->path);
        if (role == R_TEXTURE) {
            if (textures == DE__MG_TEXTURE_SLOTS) {
                ERR(g, n->path, "texture", n->line, "the graph uses more than %u texture slots",
                    DE__MG_TEXTURE_SLOTS);
                n->bad = true;
                continue;
            }
            p.kind = DE__MG_PARAM_TEXTURE;
            p.slot = textures++;
        } else {
            u32 need = role == R_PARAM_COLOR ? 3 : 1, row = 0;
            while (row < DE__MG_PARAM_ROWS && fit(used[row], need) == 4)
                ++row;
            if (row == DE__MG_PARAM_ROWS) {
                ERR(g, n->path, "name", n->line,
                    "no room for this parameter: the %u material rows are full", DE__MG_PARAM_ROWS);
                n->bad = true;
                continue;
            }
            p.kind = role == R_PARAM_COLOR ? DE__MG_PARAM_COLOR : DE__MG_PARAM_FLOAT;
            p.row = row;
            p.component = fit(used[row], need);
            used[row] |= ((1u << need) - 1u) << p.component;
            if (role == R_PARAM_COLOR) {
                p.value[0] = (float)num_value(n, "r", 1.0);
                p.value[1] = (float)num_value(n, "g", 1.0);
                p.value[2] = (float)num_value(n, "b", 1.0);
                p.max = 1.0f;
            } else {
                p.value[0] = (float)num_value(n, "value", 0.0);
                p.min = (float)num_value(n, "min", 0.0);
                p.max = (float)num_value(n, "max", 1.0);
            }
        }
        r->params[r->param_count++] = p;
        n->param = r->param_count;
    }
}

/* ---- Emission ---- */

static void put_line_node(mg *g, u32 node_plus1) {
    if (g->oom)
        return;
    de__mg_result *r = g->r;
    if (r->line_count_ == g->line_cap) {
        u32 cap = g->line_cap ? g->line_cap * 2 : 256;
        u32 *l = mg_alloc(g->a, cap * sizeof *l);
        if (!l) {
            g->oom = true;
            return;
        }
        if (r->line_count_)
            memcpy(l, g->lines, r->line_count_ * sizeof *l);
        mg_free(g->a, g->lines, g->line_cap * sizeof *l);
        g->lines = l;
        g->line_cap = cap;
    }
    g->lines[r->line_count_++] = node_plus1;
}

/* Appends `text` (whole lines) attributed to `node_plus1`. */
static void emit(mg *g, u32 node_plus1, const char *text) {
    if (g->oom)
        return;
    if (de_strbuf_append_cstr(&g->src, text) != DE_OK) {
        g->oom = true;
        return;
    }
    for (const char *c = text; *c; ++c)
        if (*c == '\n')
            put_line_node(g, node_plus1);
}

static void emitf(mg *g, u32 node_plus1, const char *fmt, ...) DE_PRINTF_FMT(3, 4);

static void emitf(mg *g, u32 node_plus1, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof buf) {
        g->oom = true; /* lines are bounded; a cut line would be wrong code */
        return;
    }
    emit(g, node_plus1, buf);
}

static void put_float(de_strbuf *b, double x, bool *oom) {
    char t[48];
    snprintf(t, sizeof t, "%.9g", x);
    bool frac = strpbrk(t, ".eE") != NULL;
    if (de_strbuf_appendf(b, x < 0 ? "(%s%s)" : "%s%s", t, frac ? "" : ".0") != DE_OK)
        *oom = true;
}

static void put(de_strbuf *b, const char *s, bool *oom) {
    if (de_strbuf_append_cstr(b, s) != DE_OK)
        *oom = true;
}

/* The expression for input `p` of `n`, converted to the pin's type. */
static void pin_expr(mg *g, const mg_node *n, int p, de_strbuf *b) {
    const pin_def *pd = &k_nodes[n->def].in[p];
    u8 want = n->in_type[p];
    if (n->in_src[p] != MG_NONE) {
        const mg_node *s = &g->nodes[n->in_src[p]];
        const node_def *sd = &k_nodes[s->def];
        u8 st = s->out_type[n->in_src_pin[p]];
        char v[96];
        snprintf(v, sizeof v, "%s_%s", s->var, sd->out[n->in_src_pin[p]].name);
        if (st == want)
            put(b, v, &g->oom);
        else if (st == T_UV)
            de_strbuf_appendf(b, "%s.uv", v);
        else if (st == T_BOOL)
            de_strbuf_appendf(b, "(%s ? 1.0 : 0.0)", v);
        else if (st == T_VEC3 && want == T_VEC4)
            de_strbuf_appendf(b, "float4(%s, 1.0)", v);
        else
            de_strbuf_appendf(b, "((%s)(%s))", k_slang_type[want], v);
        return;
    }
    toml_datum_t c =
        n->values.type == TOML_TABLE ? toml_get(n->values, pd->name) : (toml_datum_t){0};
    double x;
    if (c.type != 0 && pd->type != T_UV) {
        if (want == T_BOOL) {
            put(b,
                (c.type == TOML_BOOLEAN ? c.u.boolean : finite_num(c, &x) && x != 0.0) ? "true"
                                                                                       : "false",
                &g->oom);
            return;
        }
        u8 w = k_type_width[want];
        if (finite_num(c, &x)) {
            if (w > 1)
                de_strbuf_appendf(b, "((%s)", k_slang_type[want]);
            put_float(b, x, &g->oom);
            if (w > 1)
                put(b, ")", &g->oom);
            return;
        }
        if (c.type == TOML_ARRAY) {
            de_strbuf_appendf(b, "%s(", k_slang_type[want]);
            for (u8 i = 0; i < w; ++i) {
                if (i)
                    put(b, ", ", &g->oom);
                (void)finite_num(c.u.arr.elem[i], &x);
                put_float(b, x, &g->oom);
            }
            put(b, ")", &g->oom);
            return;
        }
    }
    put(b, pd->def, &g->oom);
}

static void value_literal(mg *g, const mg_node *n, const val_def *v, de_strbuf *b) {
    toml_datum_t x =
        n->values.type == TOML_TABLE ? toml_get(n->values, v->name) : (toml_datum_t){0};
    double num;
    switch (v->kind) {
    case V_FLOAT:
        put_float(b, finite_num(x, &num) ? num : v->def, &g->oom);
        break;
    case V_BOOL:
        put(b,
            (x.type == TOML_BOOLEAN ? x.u.boolean
             : finite_num(x, &num)  ? num != 0.0
                                    : v->def != 0.0f)
                ? "true"
                : "false",
            &g->oom);
        break;
    default:
        put(b, x.type == TOML_STRING ? x.u.s : v->sdef, &g->oom);
        break;
    }
}

/* Expands template `t` for node `n` into `b`. */
static void expand(mg *g, const mg_node *n, const char *t, de_strbuf *b) {
    const node_def *d = &k_nodes[n->def];
    const de__mg_result *r = g->r;
    while (*t && !g->oom) {
        if (*t != '{') {
            size_t run = strcspn(t, "{");
            if (de_strbuf_append(b, (de_str){t, run}) != DE_OK)
                g->oom = true;
            t += run;
            continue;
        }
        /* A placeholder is "{" key "}" with key [$>@#]?[A-Za-z0-9_]*; any other '{' is text. */
        size_t len = 1 + (t[1] == '$' || t[1] == '>' || t[1] == '@' || t[1] == '#');
        while ((t[len] >= 'a' && t[len] <= 'z') || (t[len] >= 'A' && t[len] <= 'Z') ||
               (t[len] >= '0' && t[len] <= '9') || t[len] == '_')
            ++len;
        if (t[len] != '}' || len == 1 || len > 31) {
            if (de_strbuf_append_char(b, '{') != DE_OK)
                g->oom = true;
            ++t;
            continue;
        }
        char key[32];
        memcpy(key, t + 1, len - 1);
        key[len - 1] = 0;
        t += len + 1;
        if (key[0] == '$') {
            value_literal(g, n, find_val(d, key + 1), b);
        } else if (key[0] == '>') {
            de_strbuf_appendf(b, "%s_%s", n->var, key + 1);
        } else if (key[0] == '@') {
            const de__mg_param *p = &r->params[n->param - 1];
            static const char *const sw[] = {"x", "y", "z", "w"};
            if (p->kind == DE__MG_PARAM_COLOR)
                de_strbuf_appendf(b, "m.%s.%s", k_rows[p->row], p->component ? "yzw" : "xyz");
            else
                de_strbuf_appendf(b, "m.%s.%s", k_rows[p->row], sw[p->component]);
        } else if (key[0] == '#') {
            de_strbuf_appendf(b, "%u", r->params[n->param - 1].slot);
        } else {
            int p = pin_index(d->in, key);
            if (p >= 0)
                pin_expr(g, n, p, b);
        }
    }
}

static void emit_node(mg *g, u32 idx, de_strbuf *tmp) {
    const mg_node *n = &g->nodes[idx];
    const node_def *d = &k_nodes[n->def];
    if (d->role == R_SUBGRAPH)
        return;
    if (d->role == R_OUTPUT) {
        for (const char *line = d->sink; *line && !g->oom;) {
            size_t len = strcspn(line, "\n");
            char one[256];
            memcpy(one, line, len);
            one[len] = 0;
            de_strbuf_clear(tmp);
            expand(g, n, one, tmp);
            if (!g->oom)
                emitf(g, idx + 1, "        %s\n", de_strbuf_cstr(tmp));
            line += len + (line[len] == '\n');
        }
        return;
    }
    if (d->role == R_CODE) {
        de_strbuf_clear(tmp);
        expand(g, n,
               "float {>out}; { float a = {a}; float b = {b}; float c = {c}; "
               "{>out} = ({$code}); }",
               tmp);
        if (!g->oom)
            emitf(g, idx + 1, "        %s\n", de_strbuf_cstr(tmp));
        return;
    }
    for (int o = 0; o < out_count(d) && !g->oom; ++o) {
        de_strbuf_clear(tmp);
        expand(g, n, d->out[o].expr, tmp);
        if (!g->oom)
            emitf(g, idx + 1, "        %s %s_%s = %s;\n", k_slang_type[n->out_type[o]], n->var,
                  d->out[o].name, de_strbuf_cstr(tmp));
    }
}

static void generate_source(mg *g, const char *name) {
    char clean[65];
    size_t k = 0;
    for (const char *c = name ? name : "material graph"; *c && k < 64; ++c)
        clean[k++] = ((unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7f) ? *c : '?';
    clean[k] = 0;
    emit(g, 0,
         "// Generated from a material graph by the DEngine material graph compiler (DENG-579).\n");
    emitf(g, 0, "// Graph: %s. Do not edit: regenerate it from the graph.\n", clean);
    emit(g, 0,
         "import \"visbuffer/resolve_model\";\n"
         "import \"materials/standard/standard\";\n"
         "import \"scene/scene_types\";\n"
         "import \"visbuffer/feedback\";\n"
         "import bindless;\n"
         "\n");
    for (u32 i = 0; i < sizeof k_prelude / sizeof k_prelude[0]; ++i)
        emit(g, 0, k_prelude[i]);
    emit(g, 0,
         "struct MaterialGraphModel : IMaterialModel\n"
         "{\n"
         "    typealias Params = DeGpuMaterial;\n"
         "\n"
         "    static Params load_params(DeGpuMaterial material)\n"
         "    {\n"
         "        return material;\n"
         "    }\n"
         "\n"
         "    static SurfaceData resolve_gbuffer(Params m, VertexAttribs v, TextureLodInfo lod)\n"
         "    {\n"
         "        SurfaceData s = de_surface_default();\n");
    de_strbuf tmp;
    de_strbuf_init(&tmp, g->a);
    for (u32 k = 0; k < g->order_count && !g->oom; ++k)
        if (g->nodes[g->order[k]].reach)
            emit_node(g, g->order[k], &tmp);
    de_strbuf_free(&tmp);
    emit(g, 0, "        s.model = m.model & (DE_GPU_MATERIAL_MODELS - 1u);\n");
    emitf(g, 0, "        s.features = 0x%xu;\n", g->r->features);
    emit(g, 0,
         "        return s;\n"
         "    }\n"
         "\n"
         "    static float3 evaluate(SurfaceData s, float3 wo, float3 wi)\n"
         "    {\n"
         "        return StandardModel::evaluate(s, wo, wi);\n"
         "    }\n"
         "\n"
         "    static BsdfSample sample(SurfaceData s, float3 wo, float3 u)\n"
         "    {\n"
         "        return StandardModel::sample(s, wo, u);\n"
         "    }\n"
         "\n"
         "    static float pdf(SurfaceData s, float3 wo, float3 wi)\n"
         "    {\n"
         "        return StandardModel::pdf(s, wo, wi);\n"
         "    }\n"
         "\n"
         "    static uint lobes(SurfaceData s)\n"
         "    {\n"
         "        return StandardModel::lobes(s);\n"
         "    }\n"
         "};\n"
         "\n"
         "[shader(\"compute\")]\n"
         "[numthreads(64, 1, 1)]\n"
         "void cs_resolve_model(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,\n"
         "                      uniform ResolveParams pc)\n"
         "{\n"
         "    de_resolve_tiles<MaterialGraphModel>(group, thread.x, pc);\n"
         "}\n");
}

/* ---- Entry points ---- */

static void release(mg *g) {
    mg_free(g->a, g->nodes, DE__MG_MAX_NODES * sizeof *g->nodes);
    mg_free(g->a, g->order, DE__MG_MAX_NODES * sizeof *g->order);
    mg_free(g->a, g->sorted, DE__MG_MAX_NODES * sizeof *g->sorted);
    mg_free(g->a, g->lines, g->line_cap * sizeof *g->lines);
    de_strbuf_free(&g->src);
}

/* Moves the line map, node paths and source into the result. */
static void finish(mg *g) {
    de__mg_result *r = g->r;
    size_t paths = g->count * (sizeof(char *) + 64);
    char **p = g->count ? mg_alloc(g->a, paths) : NULL;
    u32 *lines = r->line_count_ ? mg_alloc(g->a, r->line_count_ * sizeof *lines) : NULL;
    char *src = mg_alloc(g->a, g->src.len + 1);
    if ((g->count && !p) || (r->line_count_ && !lines) || !src) {
        mg_free(g->a, p, paths);
        mg_free(g->a, lines, r->line_count_ * sizeof *lines);
        mg_free(g->a, src, g->src.len + 1);
        g->oom = true;
        return;
    }
    char *text = (char *)(p + g->count);
    for (u32 i = 0; i < g->count; ++i) {
        p[i] = text + i * 64;
        memcpy(p[i], g->nodes[i].path, 64);
    }
    if (r->line_count_)
        memcpy(lines, g->lines, r->line_count_ * sizeof *lines);
    memcpy(src, g->src.ptr, g->src.len);
    src[g->src.len] = 0;
    r->node_path_ = p;
    r->node_count_ = g->count;
    r->line_node_ = lines;
    r->source = src;
    r->source_size = g->src.len;
}

de_result de__mg_generate(const de__mg_desc *d, de__mg_result *out) {
    if (!out)
        return DE_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!d || !d->graph || !d->allocator || d->graph_size > 0x7fffffffu)
        return DE_ERR_INVALID_ARGUMENT;
    mg g = {.a = d->allocator, .r = out};
    out->alloc_ = d->allocator;
    de_strbuf_init(&g.src, g.a);
    g.nodes = mg_alloc(g.a, DE__MG_MAX_NODES * sizeof *g.nodes);
    g.order = mg_alloc(g.a, DE__MG_MAX_NODES * sizeof *g.order);
    g.sorted = mg_alloc(g.a, DE__MG_MAX_NODES * sizeof *g.sorted);
    out->params = mg_alloc(g.a, DE__MG_MAX_NODES * sizeof *out->params);
    if (!g.nodes || !g.order || !g.sorted || !out->params) {
        release(&g);
        de__mg_result_free(out);
        return DE_ERR_OUT_OF_MEMORY;
    }
    toml_result_t t = de__toml_parse(g.a, d->graph, (int)d->graph_size);
    if (!t.ok) {
        if (strstr(t.errmsg, "out of memory") || strstr(t.errmsg, "ENOMEM")) {
            g.oom = true;
        } else {
            u32 line = 0;
            const char *at = strstr(t.errmsg, "line ");
            if (at)
                line = (u32)strtoul(at + 5, NULL, 10);
            ERR(&g, "", "", line, "the graph is not valid TOML: %s", t.errmsg);
        }
    } else {
        toml_datum_t kind = toml_get(t.toptab, "kind"), version = toml_get(t.toptab, "version");
        double v;
        if (kind.type != TOML_STRING || strcmp(kind.u.s, "material") != 0)
            ERR(&g, "", "", datum_line(kind), "the graph's kind must be \"material\"");
        else if (!finite_num(version, &v) || v != 1.0)
            ERR(&g, "", "", datum_line(version), "unsupported graph version (expected 1)");
        else {
            collect(&g, t.toptab, "", 0);
            for (u32 i = 0; i < g.count; ++i)
                g.sorted[i] = i;
            sort_ids(g.sorted, g.count, g.nodes);
            connect(&g, t.toptab, "");
            order_nodes(&g);
            check_types(&g);
            check_outputs(&g);
            layout(&g);
            if (!out->error_count && !g.oom)
                generate_source(&g, d->name);
            if (!out->error_count && !g.oom)
                finish(&g);
        }
    }
    de__toml_free(t);
    release(&g);
    if (g.oom) {
        de__mg_result_free(out);
        return DE_ERR_OUT_OF_MEMORY;
    }
    return out->error_count ? DE_ERR_PARSE : DE_OK;
}

const char *de__mg_node_at_line(const de__mg_result *g, u32 line) {
    if (!g || !g->line_node_ || line == 0 || line > g->line_count_)
        return NULL;
    u32 n = g->line_node_[line - 1];
    return n && n <= g->node_count_ ? g->node_path_[n - 1] : NULL;
}

/* The 1-based line after "<path>:" or "<path>(" in `s`, 0 when `s` does not name `path`. */
static u32 slang_line(const char *s, const char *path, size_t plen) {
    const char *at = strstr(s, path);
    if (!at || (at[plen] != ':' && at[plen] != '('))
        return 0;
    return (u32)strtoul(at + plen + 1, NULL, 10);
}

/*
 * Maps Slang's diagnostics to node diagnostics. Slang writes "error[E20001]: <message>" and then
 * "   --> <path>:<line>:<col>" (older builds: "<path>(<line>): error <n>: <message>"). A message
 * whose location is not in the generated module, or names a boilerplate line, gets node "".
 * The summary lines that end a failed compile ("compilation ceased", ...) are skipped.
 */
static void map_slang(de__mg_result *g, const char *path, const char *text) {
    mg m = {.a = g->alloc_, .r = g};
    size_t plen = strlen(path);
    char msg[256] = {0};
    de__mg_severity sev = DE__MG_ERROR;
    bool pending = false;
    u32 located = 0; /* diags index + 1 of the last diagnostic placed by a "-->" line */
    for (;;) {
        size_t len = strcspn(text, "\n");
        char line[512];
        size_t n = len < sizeof line - 1 ? len : sizeof line - 1;
        memcpy(line, text, n);
        line[n] = 0;
        const char *e = strstr(line, "error");
        const char *w = strstr(line, "warning");
        const char *hit = e && (!w || e < w) ? e : w;
        bool header = hit && (hit[e == hit ? 5 : 7] == '[' || hit[e == hit ? 5 : 7] == ':' ||
                              hit[e == hit ? 5 : 7] == ' ');
        if (header && (strstr(line, "compilation ceased") || strstr(line, "import failed")))
            header = false, hit = NULL;
        u32 at = slang_line(line, path, plen);
        bool arrow = strstr(line, "-->") != NULL;
        if ((header || !*text) && pending) { /* no location line followed */
            diag(&m, sev, "", "", 0, "%s", msg);
            pending = false;
        }
        const char *bar = strchr(line, '|');
        const char *caret = bar ? bar + 1 + strspn(bar + 1, " ") : NULL;
        if (arrow && pending) {
            const char *node = de__mg_node_at_line(g, at);
            u32 before = g->diag_count;
            diag(&m, sev, node ? node : "", "", 0, "%s", msg);
            pending = false;
            located = g->diag_count > before ? g->diag_count : 0;
        } else if (located && caret && *caret == '^' && !header) {
            /* "   |   ^^^^ undefined identifier 'x'." names the culprit: it replaces the header */
            caret += strspn(caret, "^~-");
            caret += strspn(caret, " ");
            if (*caret && located <= g->diag_count)
                snprintf(g->diags[located - 1].message, sizeof g->diags[0].message, "%s", caret);
            located = 0;
        } else if (header) {
            located = 0;
            sev = hit == e ? DE__MG_ERROR : DE__MG_WARNING;
            const char *body = strstr(hit, ": ");
            body = body ? body + 2 : hit;
            snprintf(msg, sizeof msg, "%s", body);
            if (at) { /* the location is on the header line itself */
                const char *node = de__mg_node_at_line(g, at);
                diag(&m, sev, node ? node : "", "", 0, "%s", msg);
            } else {
                pending = true;
            }
        }
        if (!*text)
            break;
        text += len + (text[len] == '\n');
    }
}

de_result de__mg_compile(de__mg_result *g, const de__mg_compile_desc *d,
                         de__slang_compile_result *out) {
    if (!g || !d || !out || !g->source)
        return DE_ERR_INVALID_ARGUMENT;
    const char *path = d->path ? d->path : "material_graph.slang";
    const de__slang_entry entry = {"cs_resolve_model", 0};
    de_result r = de__slang_compile(&(de__slang_compile_desc){.target = d->target,
                                                              .path = path,
                                                              .source = g->source,
                                                              .source_size = g->source_size,
                                                              .modules = d->modules,
                                                              .module_count = d->module_count,
                                                              .preload_count = d->preload_count,
                                                              .read = d->read,
                                                              .read_user = d->read_user,
                                                              .entries = &entry,
                                                              .entry_count = 1,
                                                              .allocator = g->alloc_},
                                    out);
    map_slang(g, path, out->diagnostics);
    return r;
}

static de_result json_pins(de_strbuf *b, const char *key, const pin_def *in, const out_def *outs,
                           int count) {
    DE_TRY(de_strbuf_appendf(b, ",\"%s\":[", key));
    for (int i = 0; i < count; ++i) {
        const char *name = in ? in[i].name : outs[i].name;
        u8 type = in ? in[i].type : outs[i].type;
        DE_TRY(de_strbuf_appendf(b, "%s{\"name\":\"%s\",\"type\":\"%s\"}", i ? "," : "", name,
                                 k_type_name[type]));
    }
    return de_strbuf_append_char(b, ']');
}

de_result de__mg_catalog_json(de_strbuf *b) {
    if (!b)
        return DE_ERR_INVALID_ARGUMENT;
    DE_TRY(de_strbuf_append_cstr(b, "{\"kind\":\"material\",\"conversions\":["));
    for (u32 i = 0; i < sizeof k_conversions / sizeof k_conversions[0]; ++i)
        DE_TRY(de_strbuf_appendf(b, "%s[\"%s\",\"%s\"]", i ? "," : "",
                                 k_type_name[k_conversions[i][0]],
                                 k_type_name[k_conversions[i][1]]));
    DE_TRY(de_strbuf_append_cstr(b, "],\"types\":["));
    for (u32 i = 0; i < MG_NODE_DEFS; ++i) {
        const node_def *d = &k_nodes[i];
        DE_TRY(de_strbuf_appendf(b, "%s{\"id\":\"%s\",\"title\":\"%s\",\"category\":\"%s\"",
                                 i ? "," : "", d->id, d->title, d->category));
        DE_TRY(json_pins(b, "inputs", d->in, NULL, d->hidden_inputs ? 0 : pin_count(d)));
        DE_TRY(json_pins(b, "outputs", NULL, d->out, d->hidden_outputs ? 0 : out_count(d)));
        DE_TRY(de_strbuf_append_cstr(b, ",\"values\":["));
        for (int v = 0; v < MG_VALS && d->val[v].name; ++v) {
            const val_def *x = &d->val[v];
            DE_TRY(de_strbuf_appendf(b, "%s{\"name\":\"%s\",\"type\":\"%s\",\"default\":",
                                     v ? "," : "", x->name, k_value_kind[x->kind]));
            if (x->kind == V_FLOAT)
                DE_TRY(de_strbuf_appendf(b, "%.9g}", (double)x->def));
            else if (x->kind == V_BOOL)
                DE_TRY(de_strbuf_append_cstr(b, x->def != 0.0f ? "true}" : "false}"));
            else
                DE_TRY(de_strbuf_appendf(b, "\"%s\"}", x->sdef));
        }
        DE_TRY(de_strbuf_append_cstr(b, "]}"));
    }
    return de_strbuf_append_cstr(b, "]}");
}

void de__mg_result_free(de__mg_result *r) {
    if (!r || !r->alloc_) {
        if (r)
            memset(r, 0, sizeof *r);
        return;
    }
    const de_allocator *a = r->alloc_;
    mg_free(a, r->source, r->source ? r->source_size + 1 : 0);
    mg_free(a, r->params, DE__MG_MAX_NODES * sizeof *r->params);
    mg_free(a, r->diags, r->diag_cap_ * sizeof *r->diags);
    mg_free(a, r->line_node_, r->line_count_ * sizeof *r->line_node_);
    mg_free(a, r->node_path_, r->node_count_ * (sizeof(char *) + 64));
    memset(r, 0, sizeof *r);
}
