/* vk_pipeline.c — Vulkan graphics, mesh and compute pipelines (DENG-323; SPEC §12.1, §5.4).
 *
 * Layout: every pipeline uses de__vk_device.pipeline_layout (vk_bindless.c): the bindless set 0
 * and one push-constant range of 128 bytes at offset 0 for all stages. There is no second
 * descriptor layout and no vertex input: shaders pull vertices from bindless buffers.
 *
 * Fixed state (the same on every backend, de_gpu.h and Decision: Pipelines share fixed state on
 * every backend): triangle lists; front faces counter-clockwise in clip space (y up), which with
 * the negative-height viewport of vk_cmd.c is VK_FRONT_FACE_COUNTER_CLOCKWISE; reverse-Z depth
 * test GREATER_OR_EQUAL without clamping; no stencil; one sample; the four blend modes; dynamic
 * viewport and scissor; dynamic rendering with the desc's color and depth formats.
 *
 * Checks before any Vulkan call (every build: pipeline creation is not a per-frame path): each
 * blob is SPIR-V with the named entry point of the stage it is given for (blob/kind), the shaders'
 * push-constant blocks fit desc.push_constant_bytes, a given threadgroup size matches the
 * module's local size, and every pixel-shader output has a color format. de__vk_spirv_scan reads
 * those facts from the module (no SPIRV-Reflect).
 *
 * Pipelines are created through the device's VkPipelineCache (vk_pipeline_cache.c).
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

/* ---------- SPIR-V scan ---------- */

/* SPIR-V opcodes, storage classes and decorations used below (spirv.h). */
enum {
    OP_ENTRY_POINT = 15,
    OP_EXECUTION_MODE = 16,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_MATRIX = 24,
    OP_TYPE_ARRAY = 28,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_CONSTANT = 43,
    OP_VARIABLE = 59,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    OP_EXECUTION_MODE_ID = 331,
    SC_OUTPUT = 3,
    SC_PUSH_CONSTANT = 9,
    DEC_ARRAY_STRIDE = 6,
    DEC_MATRIX_STRIDE = 7,
    DEC_BUILTIN = 11,
    DEC_LOCATION = 30,
    DEC_OFFSET = 35,
    EM_LOCAL_SIZE = 17,
    EM_LOCAL_SIZE_ID = 38
};

#define SPIRV_MAGIC 0x07230203u
#define NO_WORD     0xFFFFFFFFu

typedef struct spv {
    const u8 *code;
    u32 words;
    u32 bound;
    u32 *def; /* word index of the instruction that defines each id, or NO_WORD */
} spv;

static u32 word(const spv *s, u32 i) {
    u32 w;
    memcpy(&w, s->code + (size_t)i * 4u, 4);
    return w;
}

static u32 op_of(const spv *s, u32 at) {
    return word(s, at) & 0xFFFFu;
}
static u32 len_of(const spv *s, u32 at) {
    return word(s, at) >> 16;
}

/* Instruction operand `k` (0 = the first word after the opcode), or 0 when out of range. */
static u32 arg(const spv *s, u32 at, u32 k) {
    return k + 1u < len_of(s, at) ? word(s, at + 1u + k) : 0u;
}

/* The value of a 32-bit OpConstant, or 0. */
static u32 constant_value(const spv *s, u32 id) {
    if (id >= s->bound || s->def[id] == NO_WORD || op_of(s, s->def[id]) != OP_CONSTANT)
        return 0;
    return arg(s, s->def[id], 2);
}

/* Decoration `dec` (one literal) of `id` (member `member`, or ~0u for the id itself). */
static bool decoration(const spv *s, u32 id, u32 member, u32 dec, u32 *value) {
    for (u32 at = 5; at < s->words;) {
        const u32 op = op_of(s, at), n = len_of(s, at);
        if (op == OP_DECORATE && member == ~0u && arg(s, at, 0) == id && arg(s, at, 1) == dec) {
            *value = arg(s, at, 2);
            return true;
        }
        if (op == OP_MEMBER_DECORATE && member != ~0u && arg(s, at, 0) == id &&
            arg(s, at, 1) == member && arg(s, at, 2) == dec) {
            *value = arg(s, at, 3);
            return true;
        }
        at += n;
    }
    return false;
}

/* Bytes a value of type `id` occupies in an explicitly laid-out block (offsets and strides from
 * the decorations); 0 for types it cannot size. `depth` bounds the recursion. */
static u32 type_size(const spv *s, u32 id, u32 depth) {
    if (id >= s->bound || s->def[id] == NO_WORD || depth > 16)
        return 0;
    const u32 at = s->def[id];
    switch (op_of(s, at)) {
    case OP_TYPE_BOOL:
        return 4;
    case OP_TYPE_INT:
    case OP_TYPE_FLOAT:
        return arg(s, at, 1) / 8u;
    case OP_TYPE_VECTOR:
        return type_size(s, arg(s, at, 1), depth + 1) * arg(s, at, 2);
    case OP_TYPE_MATRIX:
        return type_size(s, arg(s, at, 1), depth + 1) * arg(s, at, 2);
    case OP_TYPE_POINTER:
        return 8; /* physical storage buffer address */
    case OP_TYPE_ARRAY: {
        const u32 count = constant_value(s, arg(s, at, 2));
        u32 stride = 0;
        if (!decoration(s, id, ~0u, DEC_ARRAY_STRIDE, &stride))
            stride = type_size(s, arg(s, at, 1), depth + 1);
        return count ? stride * (count - 1u) + type_size(s, arg(s, at, 1), depth + 1) : 0u;
    }
    case OP_TYPE_STRUCT: {
        u32 end = 0;
        const u32 members = len_of(s, at) - 2u;
        for (u32 m = 0; m < members; ++m) {
            u32 offset = 0;
            (void)decoration(s, id, m, DEC_OFFSET, &offset);
            u32 size = type_size(s, arg(s, at, 1u + m), depth + 1);
            u32 stride = 0;
            const u32 mt = arg(s, at, 1u + m);
            if (mt < s->bound && s->def[mt] != NO_WORD && op_of(s, s->def[mt]) == OP_TYPE_MATRIX &&
                decoration(s, id, m, DEC_MATRIX_STRIDE, &stride))
                size = stride * arg(s, s->def[mt], 2);
            if (offset + size > end)
                end = offset + size;
        }
        return end;
    }
    default:
        return 0;
    }
}

static de_result scan_fail(char *why, size_t cap, const char *msg) {
    if (why && cap)
        snprintf(why, cap, "%s", msg);
    return DE_ERR_PARSE;
}

de_result de__vk_spirv_scan(const void *code, u64 size, const char *entry, const de_allocator *a,
                            de__vk_spirv_info *out, char *why, size_t why_cap) {
    memset(out, 0, sizeof *out);
    if (!code || size < 20 || size % 4u != 0 || size / 4u > 0x3FFFFFFFu)
        return scan_fail(why, why_cap, "not SPIR-V: the size is not a multiple of 4 words");
    spv s;
    s.code = (const u8 *)code;
    s.words = (u32)(size / 4u);
    if (word(&s, 0) != SPIRV_MAGIC)
        return scan_fail(why, why_cap, "not SPIR-V: bad magic number");
    s.bound = word(&s, 3);
    if (s.bound == 0 || s.bound > (1u << 22))
        return scan_fail(why, why_cap, "SPIR-V id bound is out of range");
    /* Every instruction stays inside the module. */
    for (u32 at = 5; at < s.words;) {
        const u32 n = len_of(&s, at);
        if (n == 0 || n > s.words - at)
            return scan_fail(why, why_cap, "truncated SPIR-V instruction");
        at += n;
    }
    s.def = (u32 *)DE_ALLOC(a, (size_t)s.bound * sizeof(u32), DE_ALIGNOF(u32), DE_MEM_RENDER);
    if (!s.def)
        return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "SPIR-V scan (%u ids)", s.bound);
    memset(s.def, 0xFF, (size_t)s.bound * sizeof(u32));

    /* Pass 1: definitions (result ids) and the entry point. Types and constants have the result
     * id first; OpVariable and OpConstant have the result type first. */
    u32 entry_at = NO_WORD, entry_id = 0, entries = 0, only_at = NO_WORD;
    char names[96] = ""; /* the entry points seen, for the error message */
    for (u32 at = 5; at < s.words; at += len_of(&s, at)) {
        const u32 op = op_of(&s, at);
        u32 rid = 0;
        if (op >= OP_TYPE_BOOL && op <= OP_TYPE_POINTER)
            rid = arg(&s, at, 0);
        else if (op == OP_CONSTANT || op == OP_VARIABLE)
            rid = arg(&s, at, 1);
        if (rid && rid < s.bound)
            s.def[rid] = at;
        if (op == OP_ENTRY_POINT) {
            const char *name = (const char *)(s.code + (size_t)(at + 3u) * 4u);
            const size_t max = len_of(&s, at) > 3u ? (size_t)(len_of(&s, at) - 3u) * 4u : 0u;
            if (max == 0 || memchr(name, 0, max) == NULL)
                continue;
            ++entries;
            only_at = at;
            if (entry_at != NO_WORD)
                continue;
            const size_t used = strlen(names);
            snprintf(names + used, sizeof names - used, "%s\"%s\"", used ? ", " : "", name);
            if (!entry || !entry[0] || strcmp(name, entry) == 0) {
                entry_at = at;
                entry_id = arg(&s, at, 1);
            }
        }
    }
    /* A module with one entry point is that entry point whatever its name: the cooked format has
     * one SPIR-V module per entry point, and Slang names its entry point "main". */
    if (entry_at == NO_WORD && entries == 1) {
        entry_at = only_at;
        entry_id = arg(&s, only_at, 1);
    }
    de_result r = DE_OK;
    if (entry_at == NO_WORD) {
        if (why && why_cap)
            snprintf(why, why_cap, "the SPIR-V module has no entry point \"%s\" (it has %s)",
                     entry && entry[0] ? entry : "(any)", names[0] ? names : "none");
        r = DE_ERR_PARSE;
        goto done;
    }
    out->model = arg(&s, entry_at, 0);
    snprintf(out->entry, sizeof out->entry, "%s",
             (const char *)(s.code + (size_t)(entry_at + 3u) * 4u));

    /* The entry's interface: the ids after the NUL-terminated name. */
    const u32 name_words =
        (u32)((strlen((const char *)(s.code + (size_t)(entry_at + 3u) * 4u)) + 4u) / 4u);
    const u32 iface_first = entry_at + 3u + name_words, iface_end = entry_at + len_of(&s, entry_at);

    for (u32 at = 5; at < s.words; at += len_of(&s, at)) {
        const u32 op = op_of(&s, at);
        if ((op == OP_EXECUTION_MODE || op == OP_EXECUTION_MODE_ID) && arg(&s, at, 0) == entry_id &&
            (arg(&s, at, 1) == EM_LOCAL_SIZE || arg(&s, at, 1) == EM_LOCAL_SIZE_ID)) {
            const bool ids = arg(&s, at, 1) == EM_LOCAL_SIZE_ID;
            for (u32 k = 0; k < 3; ++k)
                out->local_size[k] =
                    ids ? constant_value(&s, arg(&s, at, 2u + k)) : arg(&s, at, 2u + k);
        }
        if (op != OP_VARIABLE)
            continue;
        const u32 var = arg(&s, at, 1), sc = arg(&s, at, 2);
        if (sc != SC_PUSH_CONSTANT && sc != SC_OUTPUT)
            continue;
        /* Only variables the entry point uses: SPIR-V 1.4+ lists every global it touches. */
        bool used = false;
        for (u32 i = iface_first; i < iface_end && !used; ++i)
            used = word(&s, i) == var;
        if (!used && sc == SC_OUTPUT)
            continue;
        if (sc == SC_PUSH_CONSTANT) {
            if (!used && word(&s, 1) >= 0x00010400u)
                continue;
            const u32 ptr = arg(&s, at, 0);
            if (ptr < s.bound && s.def[ptr] != NO_WORD &&
                op_of(&s, s.def[ptr]) == OP_TYPE_POINTER) {
                const u32 bytes = type_size(&s, arg(&s, s.def[ptr], 2), 0);
                if (bytes > out->push_constant_bytes)
                    out->push_constant_bytes = bytes;
            }
        } else {
            u32 loc = 0, builtin = 0;
            if (!decoration(&s, var, ~0u, DEC_BUILTIN, &builtin) &&
                decoration(&s, var, ~0u, DEC_LOCATION, &loc) && loc < 32)
                out->output_locations |= 1u << loc;
        }
    }
done:
    DE_FREE(a, s.def, (size_t)s.bound * sizeof(u32));
    return r;
}

/* ---------- Pipeline creation ---------- */

typedef struct stage_in {
    const de_gpu_shader_blob *blob;
    const char *name;            /* "vs", "ps", ... */
    u32 model;                   /* expected SPIR-V execution model */
    VkShaderStageFlagBits stage; /* Vulkan stage */
    const u32 *threadgroup;      /* the desc's size to check, or NULL */
} stage_in;

/* Checks one stage's blob against the desc: SPIR-V, entry point, stage, push constants,
 * threadgroup size. Fills `info`. */
static de_result check_stage(de_gpu_device *d, const de_gpu_pipeline_desc *desc, const stage_in *st,
                             de__vk_spirv_info *info) {
    static const char F[] = "de_gpu_pipeline_create";
    const char *n = desc->name ? desc->name : "";
    char why[160];
    de_result r = de__vk_spirv_scan(st->blob->code, st->blob->size, st->blob->entry, d->alloc, info,
                                    why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY)
        return r;
    if (r != DE_OK)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): the %s blob: %s", F, n,
                            st->name, why);
    if (info->model != st->model)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): the %s blob's entry point \"%s\" is a stage of SPIR-V "
                            "execution model %u, not %u (the stage of %s)",
                            F, n, st->name, st->blob->entry ? st->blob->entry : "", info->model,
                            st->model, st->name);
    if (info->push_constant_bytes > desc->push_constant_bytes)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): the %s shader uses %u bytes of push constants, but "
                            "push_constant_bytes is %u (use the shader reflection's size)",
                            F, n, st->name, info->push_constant_bytes, desc->push_constant_bytes);
    const u32 *tg = st->threadgroup;
    if (tg && tg[0] && info->local_size[0] &&
        (tg[0] != info->local_size[0] || tg[1] != info->local_size[1] ||
         tg[2] != info->local_size[2]))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): threadgroup size (%u, %u, %u) given for %s, but the "
                            "shader's is (%u, %u, %u)",
                            F, n, tg[0], tg[1], tg[2], st->name, info->local_size[0],
                            info->local_size[1], info->local_size[2]);
    return DE_OK;
}

/* A shader module from a blob; the code is copied when it is not 4-byte aligned. */
static de_result make_module(de_gpu_device *d, const de_gpu_shader_blob *b, const char *what,
                             VkShaderModule *out) {
    de__vk_device *vk = de__vk(d);
    const void *code = b->code;
    void *copy = NULL;
    if ((uintptr_t)code % 4u != 0) {
        copy = DE_ALLOC(d->alloc, (size_t)b->size, 4, DE_MEM_RENDER);
        if (!copy)
            return DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_pipeline_create: %s code copy", what);
        memcpy(copy, code, (size_t)b->size);
        code = copy;
    }
    VkShaderModuleCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = (size_t)b->size;
    ci.pCode = (const uint32_t *)code;
    VkResult vr = vk->fn.vkCreateShaderModule(vk->device, &ci, NULL, out);
    if (copy)
        DE_FREE(d->alloc, copy, (size_t)b->size);
    if (vr != VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "de_gpu_pipeline_create: vkCreateShaderModule (%s)", what);
    }
    return DE_OK;
}

static void blend_state(de_gpu_blend b, VkPipelineColorBlendAttachmentState *out) {
    memset(out, 0, sizeof *out);
    out->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    out->colorBlendOp = VK_BLEND_OP_ADD;
    out->alphaBlendOp = VK_BLEND_OP_ADD;
    switch (b) {
    case DE_BLEND_ALPHA:
        out->blendEnable = VK_TRUE;
        out->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        out->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        out->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case DE_BLEND_PREMUL:
        out->blendEnable = VK_TRUE;
        out->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        out->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case DE_BLEND_ADD:
        out->blendEnable = VK_TRUE;
        out->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        out->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case DE_BLEND_OPAQUE:
    default:
        out->blendEnable = VK_FALSE;
        out->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        out->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        out->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    }
}

static void set_name(de__vk_device *vk, VkPipeline p, const char *name) {
    if (!name || !name[0] || !vk->on.debug_utils || !vk->fn.vkSetDebugUtilsObjectNameEXT)
        return;
    VkDebugUtilsObjectNameInfoEXT ni;
    memset(&ni, 0, sizeof ni);
    ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ni.objectType = VK_OBJECT_TYPE_PIPELINE;
    ni.objectHandle = (u64)(uintptr_t)p;
    ni.pObjectName = name;
    vk->fn.vkSetDebugUtilsObjectNameEXT(vk->device, &ni);
}

enum { MAX_STAGES = 3 };

de_result de__vk_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                 const de_gpu_pipeline_desc *desc) {
    static const char F[] = "de_gpu_pipeline_create";
    de__vk_device *vk = de__vk(d);
    de__vk_pipeline *vp = DE__GPU_PAYLOAD(de__vk_pipeline, p);
    const char *n = desc->name ? desc->name : "";

    /* The stages of this kind (the front-end checked which blobs are set). */
    stage_in st[MAX_STAGES];
    u32 count = 0;
    switch (desc->kind) {
    case DE_PIPE_COMPUTE:
        st[count++] = (stage_in){&desc->cs, "cs", DE__VK_SPIRV_GLCOMPUTE,
                                 VK_SHADER_STAGE_COMPUTE_BIT, desc->threadgroup_size};
        break;
    case DE_PIPE_GRAPHICS:
        st[count++] =
            (stage_in){&desc->vs, "vs", DE__VK_SPIRV_VERTEX, VK_SHADER_STAGE_VERTEX_BIT, NULL};
        break;
    case DE_PIPE_MESH:
        if (!vk->on.mesh_shader || !vk->fn.vkCmdDrawMeshTasksEXT)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "%s(\"%s\"): VK_EXT_mesh_shader is not enabled on this device", F,
                                n);
        if (desc->as.code)
            st[count++] = (stage_in){&desc->as, "as", DE__VK_SPIRV_TASK_EXT,
                                     VK_SHADER_STAGE_TASK_BIT_EXT, desc->object_threadgroup_size};
        st[count++] = (stage_in){&desc->ms, "ms", DE__VK_SPIRV_MESH_EXT,
                                 VK_SHADER_STAGE_MESH_BIT_EXT, desc->threadgroup_size};
        break;
    default:
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "%s(\"%s\"): kind %d is not supported", F, n,
                            (int)desc->kind);
    }
    if (desc->kind != DE_PIPE_COMPUTE && desc->ps.code)
        st[count++] =
            (stage_in){&desc->ps, "ps", DE__VK_SPIRV_FRAGMENT, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};

    de__vk_spirv_info info[MAX_STAGES];
    for (u32 i = 0; i < count; ++i) {
        de_result r = check_stage(d, desc, &st[i], &info[i]);
        if (r != DE_OK)
            return r;
    }
    /* Every pixel-shader output needs a color format at its location. */
    if (count && st[count - 1].model == DE__VK_SPIRV_FRAGMENT) {
        const u32 outputs = info[count - 1].output_locations;
        for (u32 loc = 0; loc < 32; ++loc)
            if ((outputs & (1u << loc)) && loc >= desc->color_count)
                return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                    "%s(\"%s\"): the pixel shader writes SV_Target%u, but "
                                    "color_count is %u: give a color format for every output",
                                    F, n, loc, desc->color_count);
    }

    VkShaderModule modules[MAX_STAGES] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineShaderStageCreateInfo si[MAX_STAGES];
    de_result r = DE_OK;
    for (u32 i = 0; i < count && r == DE_OK; ++i) {
        r = make_module(d, st[i].blob, st[i].name, &modules[i]);
        memset(&si[i], 0, sizeof si[i]);
        si[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        si[i].stage = st[i].stage;
        si[i].module = modules[i];
        /* The name in the module (de__vk_spirv_scan): blob.entry is the source's name, which a
         * one-entry module may not use ("main"). */
        si[i].pName = info[i].entry;
    }
    VkResult vr = VK_SUCCESS;
    if (r == DE_OK && desc->kind == DE_PIPE_COMPUTE) {
        VkComputePipelineCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage = si[0];
        ci.layout = vk->pipeline_layout;
        ci.basePipelineIndex = -1;
        vr = vk->fn.vkCreateComputePipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL,
                                             &vp->pipeline);
        vp->bind_point = VK_PIPELINE_BIND_POINT_COMPUTE;
    } else if (r == DE_OK) {
        VkFormat colors[DE_GPU_MAX_COLOR_ATTACHMENTS];
        VkPipelineColorBlendAttachmentState blends[DE_GPU_MAX_COLOR_ATTACHMENTS];
        const u32 nc = desc->color_count;
        for (u32 i = 0; i < nc; ++i) {
            colors[i] = de__vk_format(desc->color_formats[i]);
            blend_state(desc->blend[i], &blends[i]);
        }
        VkPipelineRenderingCreateInfo rci;
        memset(&rci, 0, sizeof rci);
        rci.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rci.colorAttachmentCount = nc;
        rci.pColorAttachmentFormats = nc ? colors : NULL;
        rci.depthAttachmentFormat = de__vk_format(desc->depth_format);
        if (desc->depth_format == DE_FMT_D32_FLOAT_S8)
            rci.stencilAttachmentFormat = rci.depthAttachmentFormat;

        VkPipelineInputAssemblyStateCreateInfo ia;
        memset(&ia, 0, sizeof ia);
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineVertexInputStateCreateInfo vi;
        memset(&vi, 0, sizeof vi);
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineViewportStateCreateInfo vps;
        memset(&vps, 0, sizeof vps);
        vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vps.viewportCount = 1;
        vps.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rs;
        memset(&rs, 0, sizeof rs);
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = desc->cull == DE_CULL_BACK    ? VK_CULL_MODE_BACK_BIT
                      : desc->cull == DE_CULL_FRONT ? VK_CULL_MODE_FRONT_BIT
                                                    : VK_CULL_MODE_NONE;
        /* Counter-clockwise in clip space (y up). The negative-height viewport keeps that
         * orientation in Vulkan's framebuffer space, so this is Vulkan's COUNTER_CLOCKWISE. */
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms;
        memset(&ms, 0, sizeof ms);
        ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo ds;
        memset(&ds, 0, sizeof ds);
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = desc->depth_test ? VK_TRUE : VK_FALSE;
        ds.depthWriteEnable = desc->depth_write ? VK_TRUE : VK_FALSE;
        /* Reverse-Z (SPEC §5.4): near is 1, far is 0. depth_write without depth_test writes
         * every fragment (ALWAYS). */
        ds.depthCompareOp =
            desc->depth_test ? VK_COMPARE_OP_GREATER_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

        VkPipelineColorBlendStateCreateInfo cb;
        memset(&cb, 0, sizeof cb);
        cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = nc;
        cb.pAttachments = nc ? blends : NULL;

        static const VkDynamicState k_dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                   VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy;
        memset(&dy, 0, sizeof dy);
        dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dy.dynamicStateCount = 2;
        dy.pDynamicStates = k_dynamic;

        VkGraphicsPipelineCreateInfo ci;
        memset(&ci, 0, sizeof ci);
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext = &rci;
        ci.stageCount = count;
        ci.pStages = si;
        /* Mesh pipelines have no vertex input or input assembly. */
        ci.pVertexInputState = desc->kind == DE_PIPE_GRAPHICS ? &vi : NULL;
        ci.pInputAssemblyState = desc->kind == DE_PIPE_GRAPHICS ? &ia : NULL;
        ci.pViewportState = &vps;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = vk->pipeline_layout;
        ci.basePipelineIndex = -1;
        vr = vk->fn.vkCreateGraphicsPipelines(vk->device, vk->pipeline_cache, 1, &ci, NULL,
                                              &vp->pipeline);
        vp->bind_point = VK_PIPELINE_BIND_POINT_GRAPHICS;
    }
    for (u32 i = 0; i < count; ++i)
        if (modules[i])
            vk->fn.vkDestroyShaderModule(vk->device, modules[i], NULL);
    if (r != DE_OK)
        return r;
    if (vr != VK_SUCCESS) {
        vp->pipeline = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "%s(\"%s\"): vkCreate%sPipelines", F, n,
                           desc->kind == DE_PIPE_COMPUTE ? "Compute" : "Graphics");
    }
    set_name(vk, vp->pipeline, p->name);
    de_atomic_fetch_add_u32(&vk->pipelines_created, 1u, DE_ATOMIC_RELAXED);
    return DE_OK;
}

void de__vk_pipeline_destroy(de_gpu_device *d, de__gpu_pipeline *p) {
    de__vk_device *vk = de__vk(d);
    de__vk_pipeline *vp = DE__GPU_PAYLOAD(de__vk_pipeline, p);
    if (vp->pipeline)
        vk->fn.vkDestroyPipeline(vk->device, vp->pipeline, NULL);
    vp->pipeline = VK_NULL_HANDLE;
}
