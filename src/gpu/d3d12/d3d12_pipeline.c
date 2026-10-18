/* d3d12_pipeline.c — D3D12 pipelines: the DXIL reader, PSOs from pipeline state streams, and the
 * device-level pipeline setup (DENG-324; SPEC §12.1, §12.2, §3.3).
 *
 * Layout: every PSO uses the one global root signature (d3d12_root_signature.c): the bindless heaps
 * are reached through ResourceDescriptorHeap / SamplerDescriptorHeap (SM 6.6 direct indexing) and
 * the push constants are 32 root constants at b0 space0. No input layout: shaders pull vertices
 * from bindless buffers.
 *
 * Fixed state (de_gpu.h, Decision: Pipelines share fixed state on every backend): triangle lists;
 * FrontCounterClockwise TRUE (counter-clockwise in clip space, y up, which D3D12's viewport keeps);
 * reverse-Z GREATER_EQUAL with depth clipping on (no clamp); depth_write without depth_test is
 * ALWAYS; no stencil; one sample; the four blend modes with op ADD and all channels written.
 *
 * Streams: one C struct per kind (graphics VS/PS, mesh AS/MS/PS, compute CS) whose members are
 * pipeline-state subobjects, each a {type, value} pair aligned to a pointer like d3dx12's
 * CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT. ID3D12Device2::CreatePipelineState builds every kind,
 * and the pipeline library (d3d12_pipeline_library.c) loads and stores them under the desc's key.
 *
 * Checks before any D3D12 call (every build; creation is not a per-frame path), the same as the
 * Vulkan backend's: each blob is a DXIL container of the stage it is given for, the shaders' root
 * constants (the cbuffer at b0 space0) fit desc.push_constant_bytes, a given threadgroup size
 * matches the shader's, and every pixel-shader output has a color format. Then the device: a mesh
 * pipeline needs caps.mesh_shaders, and every pipeline needs the root signature (SM 6.6, binding
 * tier 3) and a shader model at least the blob's; otherwise DE_ERR_UNSUPPORTED. The blob checks
 * come first, so they run on any device (CI's WARP has shader model 6.2).
 *
 * The DXIL reader (portable): the DXBC container's parts; DXIL (program header: kind and shader
 * model, then the LLVM 3.7 bitcode), PSV0 (runtime info: numthreads), OSG1/OSGN (output signature:
 * SV_Target). From the bitcode it reads only the module-level metadata named `dx.resources`, whose
 * constant-buffer entries hold register space, lower bound and size in bytes. The bitstream reader
 * knows abbreviations and the BLOCKINFO block; it enters the module, its constants and its metadata
 * blocks and skips the rest by their length words.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---------- Little-endian reads ---------- */

static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

#define FOURCC(a, b, c, d) ((u32)(a) | ((u32)(b) << 8) | ((u32)(c) << 16) | ((u32)(d) << 24))
#define FOURCC_DXBC        FOURCC('D', 'X', 'B', 'C')
#define FOURCC_DXIL        FOURCC('D', 'X', 'I', 'L')
#define FOURCC_STAT        FOURCC('S', 'T', 'A', 'T')
#define FOURCC_PSV0        FOURCC('P', 'S', 'V', '0')
#define FOURCC_OSG1        FOURCC('O', 'S', 'G', '1')
#define FOURCC_OSGN        FOURCC('O', 'S', 'G', 'N')

#define D3D_NAME_TARGET 64u /* D3D_NAME_TARGET: the system value of SV_Target */

static de_result scan_fail(char *why, size_t cap, const char *msg) {
    if (why && cap)
        snprintf(why, cap, "%s", msg);
    return DE_ERR_PARSE;
}

const char *de__d3d12_dxil_kind_name(u32 kind) {
    switch (kind) {
    case DE__DXIL_KIND_PIXEL:
        return "pixel";
    case DE__DXIL_KIND_VERTEX:
        return "vertex";
    case DE__DXIL_KIND_COMPUTE:
        return "compute";
    case DE__DXIL_KIND_MESH:
        return "mesh";
    case DE__DXIL_KIND_AMPLIFICATION:
        return "amplification";
    case 2:
        return "geometry";
    case 3:
        return "hull";
    case 4:
        return "domain";
    case 6:
        return "library";
    default:
        return "unknown kind";
    }
}

/* ---------- DXBC container ---------- */

typedef struct dx_part {
    const u8 *data;
    u32 size;
} dx_part;

/* The container's parts. Header: "DXBC", 16-byte digest, u16 major, u16 minor, u32 total size,
 * u32 part count, then that many u32 part offsets; each part is {fourcc, size, bytes}. */
static de_result find_parts(const u8 *c, u64 size, const u32 *want, dx_part *out, u32 count,
                            char *why, size_t cap) {
    for (u32 i = 0; i < count; ++i)
        out[i] = (dx_part){NULL, 0};
    if (!c || size < 32)
        return scan_fail(why, cap, "not DXIL: shorter than a DXBC container header");
    if (rd32(c) != FOURCC_DXBC)
        return scan_fail(why, cap, "not DXIL: no DXBC container magic");
    const u32 total = rd32(c + 24), parts = rd32(c + 28);
    if (total > size || total < 32)
        return scan_fail(why, cap, "not DXIL: the container size is outside the blob");
    if (parts > 64 || 32u + (u64)parts * 4u > total)
        return scan_fail(why, cap, "not DXIL: the part table is outside the container");
    for (u32 p = 0; p < parts; ++p) {
        const u32 off = rd32(c + 32 + p * 4u);
        if ((u64)off + 8u > total)
            return scan_fail(why, cap, "not DXIL: a part header is outside the container");
        const u32 fourcc = rd32(c + off), psize = rd32(c + off + 4);
        if ((u64)off + 8u + psize > total)
            return scan_fail(why, cap, "not DXIL: a part is outside the container");
        for (u32 i = 0; i < count; ++i)
            if (want[i] == fourcc && !out[i].data)
                out[i] = (dx_part){c + off + 8, psize};
    }
    return DE_OK;
}

/* DxilProgramHeader: u32 ProgramVersion (kind << 16 | major << 4 | minor), u32 SizeInUint32,
 * then DxilBitcodeHeader {u32 'DXIL', u32 DxilVersion, u32 BitcodeOffset (from the start of this
 * bitcode header), u32 BitcodeSize}. */
static de_result program_header(const dx_part *p, u32 *kind, u32 *sm, const u8 **bc, u32 *bc_size,
                                char *why, size_t cap) {
    if (p->size < 24 || rd32(p->data + 8) != FOURCC_DXIL)
        return scan_fail(why, cap, "the DXIL part has no program header");
    const u32 version = rd32(p->data);
    *kind = version >> 16;
    *sm = ((version >> 4) & 0xFu) << 4 | (version & 0xFu);
    const u32 off = rd32(p->data + 16), n = rd32(p->data + 20);
    if ((u64)8u + off + n > p->size)
        return scan_fail(why, cap, "the DXIL bitcode is outside its part");
    *bc = p->data + 8 + off;
    *bc_size = n;
    return DE_OK;
}

/* PSV0: u32 size of the runtime info, then PSVRuntimeInfo0..3. Info2 (48 bytes and up) ends with
 * NumThreadsX/Y/Z at offset 36. */
static void read_psv(const dx_part *p, de__d3d12_dxil_info *out) {
    if (!p->data || p->size < 4)
        return;
    const u32 info = rd32(p->data);
    if (info < 48 || (u64)4u + info > p->size)
        return;
    for (u32 i = 0; i < 3; ++i)
        out->threadgroup[i] = rd32(p->data + 4 + 36 + i * 4u);
    out->has_psv = true;
}

static bool name_is_sv_target(const dx_part *p, u32 name_off) {
    static const char k[] = "sv_target";
    if (name_off >= p->size)
        return false;
    for (u32 i = 0; i < sizeof k; ++i) {
        if (name_off + i >= p->size)
            return false;
        char ch = (char)p->data[name_off + i];
        if (ch >= 'A' && ch <= 'Z')
            ch = (char)(ch - 'A' + 'a');
        if (ch != k[i])
            return false;
    }
    return true;
}

/* OSG1 elements are 32 bytes: stream, name, index, system value, component type, register, mask,
 * rw mask, pad, min precision. OSGN elements are 24: name, index, system value, component type,
 * register, mask, rw mask, pad. Header: u32 count, u32 offset of the first element. */
static void read_outputs(const dx_part *p, bool v1, de__d3d12_dxil_info *out) {
    if (!p->data || p->size < 8)
        return;
    const u32 count = rd32(p->data), first = rd32(p->data + 4);
    const u32 stride = v1 ? 32u : 24u;
    if (count > 64 || (u64)first + (u64)count * stride > p->size)
        return;
    for (u32 i = 0; i < count; ++i) {
        const u8 *e = p->data + first + i * stride;
        const u8 *f = v1 ? e + 4 : e; /* name, index, system value */
        const u32 name = rd32(f), index = rd32(f + 4), sv = rd32(f + 8);
        if ((sv == D3D_NAME_TARGET || name_is_sv_target(p, name)) && index < 32)
            out->target_mask |= 1u << index;
    }
    out->has_outputs = true;
}

/* ---------- LLVM bitstream ---------- */

enum { BC_END_BLOCK = 0, BC_ENTER_SUBBLOCK = 1, BC_DEFINE_ABBREV = 2, BC_UNABBREV_RECORD = 3 };
#define BLK_BLOCKINFO 0u
#define BLK_MODULE    8u
#define BLK_CONSTANTS 11u
#define BLK_METADATA  15u
#define BLK_TOP       0xFFFFFFFFu /* the parent of a top-level block */
enum { ENC_LITERAL, ENC_FIXED, ENC_VBR, ENC_ARRAY, ENC_CHAR6, ENC_BLOB };
enum { MD_OTHER, MD_VALUE, MD_NODE };
#define NO_ID       0xFFFFFFFFu
#define MAX_DEPTH   8u
#define MAX_RECORDS (1u << 22) /* records per module: a bound on malformed input */

typedef struct bc_op {
    u32 enc;
    u64 value;
} bc_op;

typedef struct bc_abbrev {
    u32 block; /* BLOCKINFO entries: the block they apply to */
    u32 first, count;
} bc_abbrev;

typedef struct bc_md {
    u32 kind;
    u32 a, b; /* VALUE: a = value id. NODE: operands md_ops[a .. a + b) */
} bc_md;

typedef struct bc_const {
    u32 id;
    i64 value;
} bc_const;

typedef struct bc {
    const u8 *p;
    u64 bits, pos;
    bool bad, oom;
    const char *err;
    const de_allocator *a;
    u32 records;
    /* Abbreviation operands, shared by every definition. */
    bc_op *ops;
    u32 op_count, op_cap;
    /* BLOCKINFO abbreviations. */
    bc_abbrev *info;
    u32 info_count, info_cap;
    /* The current record. */
    u64 *rec;
    u32 rec_count, rec_cap;
    /* Module values: globals counted, module-level integer constants. */
    u32 globals, next_value;
    bc_const *consts;
    u32 const_count, const_cap;
    /* Module-level metadata. */
    bc_md *md;
    u32 md_count, md_cap;
    u32 *md_ops;
    u32 md_op_count, md_op_cap;
    char pending_name[32];
    bool pending_resources;
    u64 md_codes;   /* diagnostics: metadata record codes seen */
    char names[96]; /* diagnostics: named metadata */
    u32 resources;  /* md id of the first operand of !dx.resources, or NO_ID */
} bc;

static void bc_bad(bc *s, const char *err) {
    if (!s->bad)
        s->err = err;
    s->bad = true;
    s->pos = s->bits;
}

static bool grow(bc *s, void **p, u32 *cap, u32 need, size_t elem) {
    if (need <= *cap)
        return true;
    u32 n = *cap ? *cap * 2u : 64u;
    while (n < need)
        n *= 2u;
    void *q = DE_ALLOC(s->a, (size_t)n * elem, 8, DE_MEM_RENDER);
    if (!q) {
        s->oom = true;
        bc_bad(s, "out of memory");
        return false;
    }
    if (*p) {
        memcpy(q, *p, (size_t)*cap * elem);
        DE_FREE(s->a, *p, (size_t)*cap * elem);
    }
    *p = q;
    *cap = n;
    return true;
}

#define PUSH(s, arr, count, cap, val)                                                              \
    (grow((s), (void **)&(s)->arr, &(s)->cap, (s)->count + 1u, sizeof *(s)->arr)                   \
         ? ((s)->arr[(s)->count++] = (val), true)                                                  \
         : false)

static u64 bc_read(bc *s, u32 n) {
    if (n == 0)
        return 0;
    if (n > 64 || s->pos + n > s->bits) {
        bc_bad(s, "truncated bitcode");
        return 0;
    }
    u64 v = 0;
    for (u32 i = 0; i < n;) {
        const u64 at = s->pos + i;
        const u32 bit = (u32)(at & 7u);
        const u32 take = 8u - bit < n - i ? 8u - bit : n - i;
        const u64 piece = ((u64)s->p[at >> 3] >> bit) & ((1u << take) - 1u);
        v |= piece << i;
        i += take;
    }
    s->pos += n;
    return v;
}

static u64 bc_vbr(bc *s, u32 n) {
    if (n < 2 || n > 32) {
        bc_bad(s, "bad VBR width");
        return 0;
    }
    const u64 hi = (u64)1 << (n - 1u);
    u64 v = 0;
    for (u32 shift = 0;; shift += n - 1u) {
        const u64 chunk = bc_read(s, n);
        if (s->bad)
            return 0;
        if (shift < 64)
            v |= (chunk & (hi - 1u)) << shift;
        if (!(chunk & hi))
            return v;
        if (shift > 64) {
            bc_bad(s, "VBR value too long");
            return 0;
        }
    }
}

static void bc_align32(bc *s) {
    const u64 to = (s->pos + 31u) & ~(u64)31u;
    if (to > s->bits)
        bc_bad(s, "truncated bitcode");
    else
        s->pos = to;
}

/* DEFINE_ABBREV: [numops vbr5, op...]; an op is a literal [1, value vbr8] or an encoding [0,
 * enc fixed3, (width vbr5 for fixed and vbr)]. An array is the second-last op, followed by its
 * element's op. Fixed(0) and VBR(0) read nothing: literal 0. */
static bool read_abbrev(bc *s, u32 *first, u32 *count) {
    const u32 n = (u32)bc_vbr(s, 5);
    if (s->bad || n == 0 || n > 64) {
        bc_bad(s, "bad abbreviation");
        return false;
    }
    *first = s->op_count;
    *count = n;
    for (u32 i = 0; i < n && !s->bad; ++i) {
        bc_op op = {ENC_LITERAL, 0};
        if (bc_read(s, 1)) {
            op.value = bc_vbr(s, 8);
        } else {
            const u32 e = (u32)bc_read(s, 3);
            if (e == 1 || e == 2) {
                op.value = bc_vbr(s, 5);
                op.enc = op.value == 0 ? ENC_LITERAL : e == 1 ? ENC_FIXED : ENC_VBR;
                if (op.value > 64 || (e == 2 && op.value == 1) || (e == 2 && op.value > 32))
                    bc_bad(s, "bad abbreviation operand width");
            } else if (e == 3) {
                op.enc = ENC_ARRAY;
                if (i + 2u != n)
                    bc_bad(s, "an array that is not the second-last abbreviation operand");
            } else if (e == 4) {
                op.enc = ENC_CHAR6;
            } else if (e == 5) {
                op.enc = ENC_BLOB;
                if (i + 1u != n)
                    bc_bad(s, "a blob that is not the last abbreviation operand");
            } else {
                bc_bad(s, "unknown abbreviation encoding");
            }
        }
        if (!PUSH(s, ops, op_count, op_cap, op))
            return false;
    }
    if (!s->bad && n >= 2 && s->ops[*first + n - 2u].enc == ENC_ARRAY) {
        const u32 el = s->ops[*first + n - 1u].enc;
        if (el == ENC_ARRAY || el == ENC_BLOB)
            bc_bad(s, "bad array element encoding");
    }
    return !s->bad;
}

static u64 read_scalar(bc *s, const bc_op *op) {
    switch (op->enc) {
    case ENC_LITERAL:
        return op->value;
    case ENC_FIXED:
        return bc_read(s, (u32)op->value);
    case ENC_VBR:
        return bc_vbr(s, (u32)op->value);
    case ENC_CHAR6: {
        static const char k[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";
        return (u64)(u8)k[bc_read(s, 6) & 63u];
    }
    default:
        bc_bad(s, "bad scalar encoding");
        return 0;
    }
}

/* Reads one record (abbreviated with `ops`, or unabbreviated when `ops` is NULL) into s->rec:
 * rec[0] is the code. */
static bool read_record(bc *s, const bc_op *ops, u32 n) {
    s->rec_count = 0;
    if (++s->records > MAX_RECORDS) {
        bc_bad(s, "too many records");
        return false;
    }
    if (!ops) {
        const u64 code = bc_vbr(s, 6), count = bc_vbr(s, 6);
        if (s->bad || count > (s->bits - s->pos) / 6u + 1u) {
            bc_bad(s, "truncated record");
            return false;
        }
        if (!PUSH(s, rec, rec_count, rec_cap, code))
            return false;
        for (u64 i = 0; i < count && !s->bad; ++i) {
            const u64 v = bc_vbr(s, 6);
            if (!PUSH(s, rec, rec_count, rec_cap, v))
                return false;
        }
        return !s->bad;
    }
    for (u32 i = 0; i < n && !s->bad; ++i) {
        if (ops[i].enc == ENC_ARRAY) {
            const u64 len = bc_vbr(s, 6);
            if (s->bad || len > s->bits - s->pos) {
                bc_bad(s, "truncated array");
                return false;
            }
            for (u64 k = 0; k < len && !s->bad; ++k) {
                const u64 v = read_scalar(s, &ops[i + 1u]);
                if (!PUSH(s, rec, rec_count, rec_cap, v))
                    return false;
            }
            break;
        }
        if (ops[i].enc == ENC_BLOB) {
            const u64 len = bc_vbr(s, 6);
            bc_align32(s);
            if (s->bad || len > (s->bits - s->pos) / 8u) {
                bc_bad(s, "truncated blob");
                return false;
            }
            s->pos += len * 8u;
            bc_align32(s);
            break;
        }
        const u64 v = read_scalar(s, &ops[i]);
        if (!PUSH(s, rec, rec_count, rec_cap, v))
            return false;
    }
    if (!s->bad && s->rec_count == 0)
        bc_bad(s, "a record without a code");
    return !s->bad;
}

static void define_md(bc *s, u32 kind, u32 a, u32 b) {
    const bc_md m = {kind, a, b};
    (void)PUSH(s, md, md_count, md_cap, m);
}

/* Module, constants and metadata records (LLVM 3.7 codes, which DXIL uses). */
static void on_record(bc *s, u32 block, u32 parent) {
    const u64 code = s->rec[0];
    const u64 *op = s->rec + 1;
    const u32 n = s->rec_count - 1u;
    if (block == BLK_MODULE) {
        /* GLOBALVAR, FUNCTION, ALIAS_OLD, ALIAS, IFUNC: one module value each. */
        if (code == 7 || code == 8 || code == 9 || code == 14 || code == 15)
            ++s->globals;
        return;
    }
    if (block == BLK_CONSTANTS && parent == BLK_MODULE) {
        if (code == 1) /* SETTYPE */
            return;
        const u32 id = s->next_value++;
        if (code == 2) { /* NULL: LLVM writes every zero this way, i32 0 included */
            const bc_const c = {id, 0};
            (void)PUSH(s, consts, const_count, const_cap, c);
        } else if (code == 4 && n >= 1) { /* INTEGER: sign-rotated VBR */
            const u64 v = op[0];
            const i64 value = (v & 1u) ? (v == 1u ? INT64_MIN : -(i64)(v >> 1)) : (i64)(v >> 1);
            const bc_const c = {id, value};
            (void)PUSH(s, consts, const_count, const_cap, c);
        }
        return;
    }
    if (block != BLK_METADATA || parent != BLK_MODULE)
        return;
    if (code < 64)
        s->md_codes |= (u64)1 << code;
    switch (code) {
    case 2: /* VALUE: [type, value id] */
        define_md(s, MD_VALUE, n >= 2 ? (u32)op[1] : NO_ID, 0);
        break;
    case 3: /* NODE: [n x (md id + 1)] */
    case 5: /* DISTINCT_NODE */
    {
        const u32 first = s->md_op_count;
        for (u32 i = 0; i < n; ++i) {
            const u32 id = op[i] ? (u32)(op[i] - 1u) : NO_ID;
            if (!PUSH(s, md_ops, md_op_count, md_op_cap, id))
                return;
        }
        define_md(s, MD_NODE, first, n);
        break;
    }
    case 4: { /* NAME: the characters of the next NAMED_NODE's name */
        u32 i = 0;
        for (; i < n && i + 1u < sizeof s->pending_name; ++i)
            s->pending_name[i] = (char)op[i];
        s->pending_name[i] = '\0';
        {
            const size_t used = strlen(s->names);
            snprintf(s->names + used, sizeof s->names - used, "%s%s", used ? ";" : "",
                     s->pending_name);
        }
        s->pending_resources = n == 12 && strcmp(s->pending_name, "dx.resources") == 0;
        break;
    }
    case 10: /* NAMED_NODE: [n x md id] */
        if (s->pending_resources && n >= 1)
            s->resources = (u32)op[0];
        s->pending_resources = false;
        break;
    case 6:  /* KIND */
    case 11: /* ATTACHMENT */
        break;
    default: /* STRING and every other kind defines one metadata id */
        define_md(s, MD_OTHER, 0, 0);
        break;
    }
}

static bool wanted(u32 block, u32 parent) {
    if (block == BLK_BLOCKINFO)
        return true;
    if (block == BLK_MODULE)
        return parent == BLK_TOP;
    return (block == BLK_CONSTANTS || block == BLK_METADATA) && parent == BLK_MODULE;
}

/* One block, from after its ENTER_SUBBLOCK header to its END_BLOCK (or `end`). */
static void parse_block(bc *s, u32 block, u32 width, u64 end, u32 depth, u32 parent) {
    /* This block's abbreviations: BLOCKINFO's for its id, then its own (indices into s->info
     * for the first, into a local list for the second). */
    u32 *local = NULL, local_count = 0, local_cap = 0;
    u32 setbid = NO_ID;
    /* Module-level constants are numbered after the global values (LLVM's value list). */
    if (block == BLK_CONSTANTS && parent == BLK_MODULE)
        s->next_value = s->globals;
    for (u32 i = 0; i < s->info_count; ++i)
        if (s->info[i].block == block) {
            if (!grow(s, (void **)&local, &local_cap, local_count + 1u, sizeof *local))
                goto done;
            local[local_count++] = i; /* info index; own definitions are stored as info too */
        }
    while (!s->bad && s->pos < end) {
        const u32 id = (u32)bc_read(s, width);
        if (s->bad)
            break;
        if (id == BC_END_BLOCK) {
            bc_align32(s);
            break;
        }
        if (id == BC_ENTER_SUBBLOCK) {
            const u32 sub = (u32)bc_vbr(s, 8), w = (u32)bc_vbr(s, 4);
            bc_align32(s);
            const u64 words = bc_read(s, 32);
            if (s->bad)
                break;
            const u64 sub_end = s->pos + words * 32u;
            if (sub_end > end || w < 2 || w > 32) {
                bc_bad(s, "a block is outside its parent");
                break;
            }
            if (wanted(sub, block) && depth < MAX_DEPTH)
                parse_block(s, sub, w, sub_end, depth + 1u, block);
            if (!s->bad)
                s->pos = sub_end;
            continue;
        }
        if (id == BC_DEFINE_ABBREV) {
            u32 first = 0, count = 0;
            if (!read_abbrev(s, &first, &count))
                break;
            const bc_abbrev ab = {block == BLK_BLOCKINFO ? setbid : block, first, count};
            if (block == BLK_BLOCKINFO && setbid == NO_ID) {
                bc_bad(s, "BLOCKINFO abbreviation before SETBID");
                break;
            }
            if (!PUSH(s, info, info_count, info_cap, ab))
                break;
            if (block != BLK_BLOCKINFO) {
                /* Own definition: mark it so later blocks of this id do not inherit it. */
                s->info[s->info_count - 1u].block = NO_ID - 1u;
                if (!grow(s, (void **)&local, &local_cap, local_count + 1u, sizeof *local))
                    break;
                local[local_count++] = s->info_count - 1u;
            }
            continue;
        }
        const bc_op *ops = NULL;
        u32 nops = 0;
        if (id != BC_UNABBREV_RECORD) {
            const u32 k = id - 4u;
            if (k >= local_count) {
                bc_bad(s, "an undefined abbreviation");
                break;
            }
            const bc_abbrev *ab = &s->info[local[k]];
            ops = &s->ops[ab->first];
            nops = ab->count;
        }
        if (!read_record(s, ops, nops))
            break;
        if (block == BLK_BLOCKINFO) {
            if (s->rec[0] == 1 && s->rec_count >= 2) /* SETBID */
                setbid = (u32)s->rec[1];
            continue;
        }
        on_record(s, block, parent);
    }
done:
    if (local)
        DE_FREE(s->a, local, (size_t)local_cap * sizeof *local);
}

static void bc_free(bc *s) {
#define FREE_ARR(arr, cap)                                                                         \
    if (s->arr)                                                                                    \
    DE_FREE(s->a, s->arr, (size_t)s->cap * sizeof *s->arr)
    FREE_ARR(ops, op_cap);
    FREE_ARR(info, info_cap);
    FREE_ARR(rec, rec_cap);
    FREE_ARR(consts, const_cap);
    FREE_ARR(md, md_cap);
    FREE_ARR(md_ops, md_op_cap);
#undef FREE_ARR
}

/* The integer constant behind metadata id `id` (a VALUE of a module-level integer constant). */
static bool md_int(const bc *s, u32 id, i64 *out) {
    if (id >= s->md_count || s->md[id].kind != MD_VALUE)
        return false;
    const u32 value = s->md[id].a;
    for (u32 i = 0; i < s->const_count; ++i)
        if (s->consts[i].id == value) {
            *out = s->consts[i].value;
            return true;
        }
    return false;
}

static const bc_md *md_node(const bc *s, u32 id) {
    return id < s->md_count && s->md[id].kind == MD_NODE ? &s->md[id] : NULL;
}

/* !dx.resources = !{!R}; R = !{SRVs, UAVs, CBuffers, Samplers}; a CBuffer is
 * !{id, global, name, space, lower bound, range size, size in bytes, tags}. */
/* Appends to the diagnostics trace. */
static void trace(de__d3d12_dxil_info *out, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void trace(de__d3d12_dxil_info *out, const char *fmt, ...) {
    const size_t used = strlen(out->bc.trace);
    if (used + 1u >= sizeof out->bc.trace)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out->bc.trace + used, sizeof out->bc.trace - used, fmt, ap);
    va_end(ap);
}

static char md_kind_char(const bc *s, u32 id) {
    if (id == NO_ID)
        return '-';
    if (id >= s->md_count)
        return '?';
    return s->md[id].kind == MD_NODE ? 'N' : s->md[id].kind == MD_VALUE ? 'V' : 'o';
}

static void collect_cbuffers(const bc *s, de__d3d12_dxil_info *out) {
    const bc_md *r = md_node(s, s->resources);
    trace(out, "R%c", md_kind_char(s, s->resources));
    if (r)
        for (u32 i = 0; i < r->b && i < 6; ++i)
            trace(out, " %d%c", (int)s->md_ops[r->a + i], md_kind_char(s, s->md_ops[r->a + i]));
    if (!r || r->b < 3)
        return;
    const bc_md *list = md_node(s, s->md_ops[r->a + 2u]);
    if (!list)
        return;
    trace(out, "; list %u:", list->b);
    for (u32 i = 0; i < list->b; ++i) {
        const bc_md *cb = md_node(s, s->md_ops[list->a + i]);
        if (cb) {
            trace(out, " cb(%u)", cb->b);
            for (u32 k = 0; k < cb->b && k < 8; ++k) {
                const u32 id = s->md_ops[cb->a + k];
                trace(out, " %c", md_kind_char(s, id));
                if (id < s->md_count && s->md[id].kind == MD_VALUE)
                    trace(out, "v%u", s->md[id].a);
            }
        }
        i64 space = 0, lower = 0, size = 0;
        if (!cb || cb->b < 7 || !md_int(s, s->md_ops[cb->a + 3u], &space) ||
            !md_int(s, s->md_ops[cb->a + 4u], &lower) || !md_int(s, s->md_ops[cb->a + 6u], &size))
            continue;
        if (out->cbuffer_count < DE__DXIL_MAX_CBUFFERS)
            out->cbuffers[out->cbuffer_count] =
                (de__d3d12_dxil_cbuffer){(u32)space, (u32)lower, (u32)size};
        ++out->cbuffer_count;
        if (space == 0 && lower == 0)
            out->push_constant_bytes = (u32)size;
    }
}

/* Reads the module's dx.resources constant buffers into `out`. */
static de_result read_bitcode(const u8 *p, u32 size, const de_allocator *a,
                              de__d3d12_dxil_info *out, char *why, size_t cap) {
    if (size < 8 || size % 4u != 0 || p[0] != 'B' || p[1] != 'C' || p[2] != 0xC0 || p[3] != 0xDE)
        return scan_fail(why, cap, "the DXIL part holds no LLVM bitcode");
    bc s;
    memset(&s, 0, sizeof s);
    s.p = p;
    s.bits = (u64)size * 8u;
    s.pos = 32;
    s.a = a;
    s.resources = NO_ID;
    while (!s.bad && s.pos + 32u <= s.bits) {
        const u32 id = (u32)bc_read(&s, 2);
        if (id != BC_ENTER_SUBBLOCK)
            break; /* the end, or padding */
        const u32 block = (u32)bc_vbr(&s, 8), w = (u32)bc_vbr(&s, 4);
        bc_align32(&s);
        const u64 words = bc_read(&s, 32);
        if (s.bad)
            break;
        const u64 end = s.pos + words * 32u;
        if (end > s.bits || w < 2 || w > 32) {
            bc_bad(&s, "a top-level block is outside the bitcode");
            break;
        }
        if (wanted(block, BLK_TOP))
            parse_block(&s, block, w, end, 1, BLK_TOP);
        if (!s.bad)
            s.pos = end;
    }
    de_result r = DE_OK;
    if (s.oom) {
        r = DE_FAIL(DE_ERR_OUT_OF_MEMORY, "d3d12: DXIL bitcode reader");
    } else if (s.bad) {
        char msg[128];
        snprintf(msg, sizeof msg, "bad DXIL bitcode: %s", s.err ? s.err : "?");
        r = scan_fail(why, cap, msg);
    } else {
        collect_cbuffers(&s, out);
        out->has_bitcode = true;
    }
    out->bc.globals = s.globals;
    out->bc.constants = s.const_count;
    out->bc.metadata = s.md_count;
    out->bc.md_codes = s.md_codes;
    snprintf(out->bc.names, sizeof out->bc.names, "%s", s.names);
    out->bc.resources = s.resources;
    if (s.const_count)
        trace(out, "; const ids %u..%u", s.consts[0].id, s.consts[s.const_count - 1u].id);
    bc_free(&s);
    return r;
}

de_result de__d3d12_dxil_scan(const void *code, u64 size, const de_allocator *a,
                              de__d3d12_dxil_info *out, char *why, size_t why_cap) {
    memset(out, 0, sizeof *out);
    if (why && why_cap)
        why[0] = '\0';
    static const u32 want[5] = {FOURCC_DXIL, FOURCC_STAT, FOURCC_PSV0, FOURCC_OSG1, FOURCC_OSGN};
    dx_part parts[5];
    de_result r = find_parts((const u8 *)code, size, want, parts, 5, why, why_cap);
    if (r != DE_OK)
        return r;
    if (!parts[0].data)
        return scan_fail(why, why_cap, "not DXIL: the container has no DXIL part");
    const u8 *bitcode = NULL;
    u32 bitcode_size = 0;
    r = program_header(&parts[0], &out->kind, &out->shader_model, &bitcode, &bitcode_size, why,
                       why_cap);
    if (r != DE_OK)
        return r;
    read_psv(&parts[2], out);
    if (parts[3].data)
        read_outputs(&parts[3], true, out);
    else if (parts[4].data)
        read_outputs(&parts[4], false, out);
    r = read_bitcode(bitcode, bitcode_size, a, out, why, why_cap);
    if (r == DE_ERR_PARSE && parts[1].data) {
        /* The reflection part holds the same module with its metadata: try it. */
        u32 k = 0, sm = 0;
        if (program_header(&parts[1], &k, &sm, &bitcode, &bitcode_size, NULL, 0) == DE_OK &&
            read_bitcode(bitcode, bitcode_size, a, out, NULL, 0) == DE_OK)
            r = DE_OK;
    }
    return r;
}

/* ---------- Pipeline keys, scissor (portable) ---------- */

/* Bump when the stream layout or the fixed state changes: older library entries then miss. */
#define DE__D3D12_PIPELINE_STREAM_VERSION 1u

static u64 blob_hash(const de_gpu_shader_blob *b, u64 seed) {
    return b->code && b->size ? de_hash64(b->code, (size_t)b->size, seed) ^ b->size : 0u;
}

u64 de__d3d12_pipeline_key(const de_gpu_pipeline_desc *desc) {
    struct {
        u32 version, kind, color_count, depth_format, cull, depth_test, depth_write, pad;
        u32 colors[DE_GPU_MAX_COLOR_ATTACHMENTS], blend[DE_GPU_MAX_COLOR_ATTACHMENTS];
        u64 blobs[5];
    } k;
    memset(&k, 0, sizeof k);
    k.version = DE__D3D12_PIPELINE_STREAM_VERSION;
    k.kind = (u32)desc->kind;
    const u32 nc = desc->color_count < DE_GPU_MAX_COLOR_ATTACHMENTS ? desc->color_count
                                                                    : DE_GPU_MAX_COLOR_ATTACHMENTS;
    if (desc->kind != DE_PIPE_COMPUTE) {
        k.color_count = nc;
        for (u32 i = 0; i < nc; ++i) {
            k.colors[i] = (u32)desc->color_formats[i];
            k.blend[i] = (u32)desc->blend[i];
        }
        k.depth_format = (u32)desc->depth_format;
        k.cull = (u32)desc->cull;
        k.depth_test = desc->depth_test;
        k.depth_write = desc->depth_write;
    }
    const de_gpu_shader_blob *b[5] = {&desc->vs, &desc->ps, &desc->as, &desc->ms, &desc->cs};
    for (u32 i = 0; i < 5; ++i)
        k.blobs[i] = blob_hash(b[i], i + 1u);
    return de_hash64(&k, sizeof k, DE__D3D12_PIPELINE_LIBRARY_MAGIC);
}

de__d3d12_rect de__d3d12_scissor_rect(i32 x, i32 y, u32 w, u32 h) {
    i64 x0 = x, y0 = y, x1 = (i64)x + w, y1 = (i64)y + h;
    const i64 lo = 0, hi = DE__D3D12_SCISSOR_MAX;
    x0 = x0 < lo ? lo : x0 > hi ? hi : x0;
    y0 = y0 < lo ? lo : y0 > hi ? hi : y0;
    x1 = x1 < x0 ? x0 : x1 > hi ? hi : x1;
    y1 = y1 < y0 ? y0 : y1 > hi ? hi : y1;
    de__d3d12_rect r = {(i32)x0, (i32)y0, (i32)x1, (i32)y1};
    return r;
}

/* ---------- The backend (Windows) ---------- */
#if defined(_WIN32)

/* A pipeline-state subobject: {type, value}, aligned to a pointer (d3dx12's
 * CD3DX12_PIPELINE_STATE_STREAM_SUBOBJECT). */
#define SUBOBJ(T)                                                                                  \
    struct {                                                                                       \
        _Alignas(sizeof(void *)) D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type;                         \
        T v;                                                                                       \
    }

typedef struct gfx_stream {
    SUBOBJ(ID3D12RootSignature *) root;
    SUBOBJ(D3D12_SHADER_BYTECODE) vs;
    SUBOBJ(D3D12_SHADER_BYTECODE) ps;
    SUBOBJ(D3D12_BLEND_DESC) blend;
    SUBOBJ(UINT) sample_mask;
    SUBOBJ(D3D12_RASTERIZER_DESC) raster;
    SUBOBJ(D3D12_DEPTH_STENCIL_DESC) depth;
    SUBOBJ(D3D12_PRIMITIVE_TOPOLOGY_TYPE) topology;
    SUBOBJ(struct D3D12_RT_FORMAT_ARRAY) rtv;
    SUBOBJ(DXGI_FORMAT) dsv;
    SUBOBJ(DXGI_SAMPLE_DESC) samples;
} gfx_stream;

typedef struct mesh_stream {
    SUBOBJ(ID3D12RootSignature *) root;
    SUBOBJ(D3D12_SHADER_BYTECODE) as;
    SUBOBJ(D3D12_SHADER_BYTECODE) ms;
    SUBOBJ(D3D12_SHADER_BYTECODE) ps;
    SUBOBJ(D3D12_BLEND_DESC) blend;
    SUBOBJ(UINT) sample_mask;
    SUBOBJ(D3D12_RASTERIZER_DESC) raster;
    SUBOBJ(D3D12_DEPTH_STENCIL_DESC) depth;
    SUBOBJ(D3D12_PRIMITIVE_TOPOLOGY_TYPE) topology;
    SUBOBJ(struct D3D12_RT_FORMAT_ARRAY) rtv;
    SUBOBJ(DXGI_FORMAT) dsv;
    SUBOBJ(DXGI_SAMPLE_DESC) samples;
} mesh_stream;

typedef struct compute_stream {
    SUBOBJ(ID3D12RootSignature *) root;
    SUBOBJ(D3D12_SHADER_BYTECODE) cs;
} compute_stream;

/* ---------- Device-level setup ---------- */

de_result de__d3d12_pipelines_init(de_gpu_device *d, const char *dir) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_pipe_state *ps = (d3d12_pipe_state *)DE_ALLOC(
        d->alloc, sizeof *ps, DE_ALIGNOF(d3d12_pipe_state), DE_MEM_RENDER);
    if (!ps)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "d3d12: pipeline state");
    memset(ps, 0, sizeof *ps);
    de_mutex_init(&ps->lock);
    de_atomic_init_u32(&ps->hits, 0);
    de_atomic_init_u32(&ps->misses, 0);
    de_atomic_init_u32(&ps->stored, 0);
    dd->pipe = ps;
    if (FAILED(ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12Device1, (void **)&ps->dev1)))
        ps->dev1 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12Device2, (void **)&ps->dev2)))
        ps->dev2 = NULL;
    de__d3d12_root_signature_init(d);
    de__d3d12_command_signatures_init(d);
    de__d3d12_library_init(d, dir);
    return DE_OK;
}

void de__d3d12_pipelines_shutdown(de_gpu_device *d) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_pipe_state *ps = dd->pipe;
    if (!ps)
        return;
    de__d3d12_library_shutdown(d);
    ID3D12CommandSignature *sigs[3] = {ps->sig_draw_indexed, ps->sig_dispatch,
                                       ps->sig_dispatch_mesh};
    for (u32 i = 0; i < 3; ++i)
        if (sigs[i])
            ID3D12CommandSignature_Release(sigs[i]);
    if (ps->root)
        ID3D12RootSignature_Release(ps->root);
    if (ps->dev2)
        ID3D12Device2_Release(ps->dev2);
    if (ps->dev1)
        ID3D12Device1_Release(ps->dev1);
    de_mutex_destroy(&ps->lock);
    DE_FREE(d->alloc, ps, sizeof *ps);
    dd->pipe = NULL;
}

/* ---------- Checks ---------- */

typedef struct stage_in {
    const de_gpu_shader_blob *blob;
    const char *name; /* "vs", "ps", ... */
    u32 kind;         /* expected DE__DXIL_KIND_* */
    const u32 *threadgroup;
} stage_in;

static de_result check_stage(de_gpu_device *d, const de_gpu_pipeline_desc *desc, const stage_in *st,
                             de__d3d12_dxil_info *info) {
    static const char F[] = "de_gpu_pipeline_create";
    const char *n = desc->name ? desc->name : "";
    char why[160];
    de_result r =
        de__d3d12_dxil_scan(st->blob->code, st->blob->size, d->alloc, info, why, sizeof why);
    if (r == DE_ERR_OUT_OF_MEMORY)
        return r;
    if (r != DE_OK)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, "%s(\"%s\"): the %s blob: %s", F, n,
                            st->name, why);
    if (info->kind != st->kind)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): the %s blob is a %s shader (DXIL shader kind %u), not a "
                            "%s shader",
                            F, n, st->name, de__d3d12_dxil_kind_name(info->kind), info->kind,
                            de__d3d12_dxil_kind_name(st->kind));
    if (info->push_constant_bytes > desc->push_constant_bytes)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): the %s shader uses %u bytes of push constants, but "
                            "push_constant_bytes is %u (use the shader reflection's size)",
                            F, n, st->name, info->push_constant_bytes, desc->push_constant_bytes);
    if (info->push_constant_bytes > DE_GPU_MAX_PUSH_CONSTANT_BYTES)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): the %s shader's constant buffer at b0 space0 is %u bytes, "
                            "more than the %u bytes of root constants",
                            F, n, st->name, info->push_constant_bytes,
                            DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    const u32 *tg = st->threadgroup;
    if (tg && tg[0] && info->has_psv && info->threadgroup[0] &&
        (tg[0] != info->threadgroup[0] || tg[1] != info->threadgroup[1] ||
         tg[2] != info->threadgroup[2]))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            "%s(\"%s\"): threadgroup size (%u, %u, %u) given for %s, but the "
                            "shader's is (%u, %u, %u)",
                            F, n, tg[0], tg[1], tg[2], st->name, info->threadgroup[0],
                            info->threadgroup[1], info->threadgroup[2]);
    return DE_OK;
}

static void blend_rt(de_gpu_blend b, D3D12_RENDER_TARGET_BLEND_DESC *o) {
    memset(o, 0, sizeof *o);
    o->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    o->LogicOp = D3D12_LOGIC_OP_NOOP;
    o->BlendOp = D3D12_BLEND_OP_ADD;
    o->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    switch (b) {
    case DE_BLEND_ALPHA:
        o->BlendEnable = TRUE;
        o->SrcBlend = D3D12_BLEND_SRC_ALPHA;
        o->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        o->SrcBlendAlpha = D3D12_BLEND_ONE;
        o->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case DE_BLEND_PREMUL:
        o->BlendEnable = TRUE;
        o->SrcBlend = D3D12_BLEND_ONE;
        o->DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        o->SrcBlendAlpha = D3D12_BLEND_ONE;
        o->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case DE_BLEND_ADD:
        o->BlendEnable = TRUE;
        o->SrcBlend = D3D12_BLEND_ONE;
        o->DestBlend = D3D12_BLEND_ONE;
        o->SrcBlendAlpha = D3D12_BLEND_ONE;
        o->DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case DE_BLEND_OPAQUE:
    default:
        o->SrcBlend = D3D12_BLEND_ONE;
        o->DestBlend = D3D12_BLEND_ZERO;
        o->SrcBlendAlpha = D3D12_BLEND_ONE;
        o->DestBlendAlpha = D3D12_BLEND_ZERO;
        break;
    }
}

/* The raster state every graphics and mesh pipeline shares, from the desc. */
typedef struct raster_state {
    D3D12_BLEND_DESC blend;
    D3D12_RASTERIZER_DESC raster;
    D3D12_DEPTH_STENCIL_DESC depth;
    struct D3D12_RT_FORMAT_ARRAY rtv;
    DXGI_FORMAT dsv;
} raster_state;

static void raster_from(const de_gpu_pipeline_desc *desc, raster_state *o) {
    memset(o, 0, sizeof *o);
    o->blend.AlphaToCoverageEnable = FALSE;
    o->blend.IndependentBlendEnable = TRUE;
    const u32 nc = desc->color_count;
    for (u32 i = 0; i < DE_GPU_MAX_COLOR_ATTACHMENTS; ++i)
        blend_rt(i < nc ? desc->blend[i] : DE_BLEND_OPAQUE, &o->blend.RenderTarget[i]);

    o->raster.FillMode = D3D12_FILL_MODE_SOLID;
    o->raster.CullMode = desc->cull == DE_CULL_BACK    ? D3D12_CULL_MODE_BACK
                         : desc->cull == DE_CULL_FRONT ? D3D12_CULL_MODE_FRONT
                                                       : D3D12_CULL_MODE_NONE;
    /* Counter-clockwise in clip space (y up). The viewport maps clip-space up to the top of the
     * target without mirroring the image, so the winding D3D12 sees is the same. */
    o->raster.FrontCounterClockwise = TRUE;
    o->raster.DepthClipEnable = TRUE; /* no depth clamp */
    o->raster.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    /* Reverse-Z (SPEC §5.4): near is 1, far is 0. depth_write without depth_test writes every
     * fragment (ALWAYS). */
    o->depth.DepthEnable = desc->depth_test || desc->depth_write;
    o->depth.DepthWriteMask =
        desc->depth_write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    o->depth.DepthFunc =
        desc->depth_test ? D3D12_COMPARISON_FUNC_GREATER_EQUAL : D3D12_COMPARISON_FUNC_ALWAYS;
    o->depth.StencilEnable = FALSE;
    o->depth.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    o->depth.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC keep = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
                                             D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS};
    o->depth.FrontFace = keep;
    o->depth.BackFace = keep;

    o->rtv.NumRenderTargets = nc;
    for (u32 i = 0; i < nc; ++i)
        o->rtv.RTFormats[i] = de__d3d12_format_resource(desc->color_formats[i], 0);
    o->dsv = desc->depth_format == DE_FMT_UNKNOWN
                 ? DXGI_FORMAT_UNKNOWN
                 : de__d3d12_format_resource(desc->depth_format, 0);
}

static D3D12_SHADER_BYTECODE bytecode(const de_gpu_shader_blob *b) {
    D3D12_SHADER_BYTECODE c;
    c.pShaderBytecode = b->code && b->size ? b->code : NULL;
    c.BytecodeLength = b->code ? (SIZE_T)b->size : 0;
    return c;
}

enum { MAX_STAGES = 3 };

de_result de__d3d12_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                    const de_gpu_pipeline_desc *desc) {
    if (desc->kind == DE_PIPE_RAYTRACING)
        return de__d3d12_rt_pipeline_create(d, p, desc);
    static const char F[] = "de_gpu_pipeline_create";
    d3d12_device *dd = de__d3d12(d);
    d3d12_pipe_state *ps = dd->pipe;
    d3d12_pipeline *dp = DE__GPU_PAYLOAD(d3d12_pipeline, p);
    const char *n = desc->name ? desc->name : "";

    stage_in st[MAX_STAGES];
    u32 count = 0;
    switch (desc->kind) {
    case DE_PIPE_COMPUTE:
        st[count++] = (stage_in){&desc->cs, "cs", DE__DXIL_KIND_COMPUTE, desc->threadgroup_size};
        break;
    case DE_PIPE_GRAPHICS:
        st[count++] = (stage_in){&desc->vs, "vs", DE__DXIL_KIND_VERTEX, NULL};
        break;
    case DE_PIPE_MESH:
        if (!d->caps.mesh_shaders)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "%s(\"%s\"): this D3D12 device has no mesh shaders (mesh shader "
                                "tier %u, shader model %u.%u)",
                                F, n, dd->features.mesh_shader_tier,
                                (dd->features.shader_model >> 4) & 0xFu,
                                dd->features.shader_model & 0xFu);
        if (desc->as.code)
            st[count++] = (stage_in){&desc->as, "as", DE__DXIL_KIND_AMPLIFICATION,
                                     desc->object_threadgroup_size};
        st[count++] = (stage_in){&desc->ms, "ms", DE__DXIL_KIND_MESH, desc->threadgroup_size};
        break;
    default:
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "%s(\"%s\"): kind %d is not supported", F, n,
                            (int)desc->kind);
    }
    if (desc->kind != DE_PIPE_COMPUTE && desc->ps.code)
        st[count++] = (stage_in){&desc->ps, "ps", DE__DXIL_KIND_PIXEL, NULL};

    de__d3d12_dxil_info info[MAX_STAGES];
    u32 highest_sm = 0;
    for (u32 i = 0; i < count; ++i) {
        de_result r = check_stage(d, desc, &st[i], &info[i]);
        if (r != DE_OK)
            return r;
        if (info[i].shader_model > highest_sm)
            highest_sm = info[i].shader_model;
    }
    /* Every pixel-shader output needs a color format at its index. */
    if (count && st[count - 1].kind == DE__DXIL_KIND_PIXEL) {
        const u32 outputs = info[count - 1].target_mask;
        for (u32 t = 0; t < 32; ++t)
            if ((outputs & (1u << t)) && t >= desc->color_count)
                return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                    "%s(\"%s\"): the pixel shader writes SV_Target%u, but "
                                    "color_count is %u: give a color format for every output",
                                    F, n, t, desc->color_count);
    }

    /* The device. */
    if (!ps || !ps->root || !ps->dev2)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED, "%s(\"%s\"): %s", F, n,
                            ps && ps->root_why[0] ? ps->root_why
                                                  : "d3d12: no ID3D12Device2 (runtime too old)");
    if (highest_sm > dd->features.shader_model)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "%s(\"%s\"): the shaders need shader model %u.%u; the device has %u.%u",
                            F, n, highest_sm >> 4, highest_sm & 0xFu,
                            (dd->features.shader_model >> 4) & 0xFu,
                            dd->features.shader_model & 0xFu);

    D3D12_PIPELINE_STATE_STREAM_DESC sd;
    gfx_stream gs;
    mesh_stream ms;
    compute_stream cs;
    raster_state rs;
    const DXGI_SAMPLE_DESC one = {1, 0};
    if (desc->kind == DE_PIPE_COMPUTE) {
        memset(&cs, 0, sizeof cs);
        cs.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        cs.root.v = ps->root;
        cs.cs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS;
        cs.cs.v = bytecode(&desc->cs);
        sd.SizeInBytes = sizeof cs;
        sd.pPipelineStateSubobjectStream = &cs;
    } else if (desc->kind == DE_PIPE_GRAPHICS) {
        raster_from(desc, &rs);
        memset(&gs, 0, sizeof gs);
        gs.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        gs.root.v = ps->root;
        gs.vs.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS;
        gs.vs.v = bytecode(&desc->vs);
        gs.ps.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
        gs.ps.v = bytecode(&desc->ps);
        gs.blend.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
        gs.blend.v = rs.blend;
        gs.sample_mask.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
        gs.sample_mask.v = UINT_MAX;
        gs.raster.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        gs.raster.v = rs.raster;
        gs.depth.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
        gs.depth.v = rs.depth;
        gs.topology.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
        gs.topology.v = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        gs.rtv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
        gs.rtv.v = rs.rtv;
        gs.dsv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
        gs.dsv.v = rs.dsv;
        gs.samples.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
        gs.samples.v = one;
        sd.SizeInBytes = sizeof gs;
        sd.pPipelineStateSubobjectStream = &gs;
    } else {
        raster_from(desc, &rs);
        memset(&ms, 0, sizeof ms);
        ms.root.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
        ms.root.v = ps->root;
        ms.as.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS;
        ms.as.v = bytecode(&desc->as);
        ms.ms.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS;
        ms.ms.v = bytecode(&desc->ms);
        ms.ps.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS;
        ms.ps.v = bytecode(&desc->ps);
        ms.blend.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND;
        ms.blend.v = rs.blend;
        ms.sample_mask.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK;
        ms.sample_mask.v = UINT_MAX;
        ms.raster.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER;
        ms.raster.v = rs.raster;
        ms.depth.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL;
        ms.depth.v = rs.depth;
        ms.topology.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY;
        ms.topology.v = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        ms.rtv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS;
        ms.rtv.v = rs.rtv;
        ms.dsv.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT;
        ms.dsv.v = rs.dsv;
        ms.samples.type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC;
        ms.samples.v = one;
        sd.SizeInBytes = sizeof ms;
        sd.pPipelineStateSubobjectStream = &ms;
    }

    HRESULT hr = de__d3d12_library_pipeline(d, de__d3d12_pipeline_key(desc), &sd, &dp->pso);
    if (FAILED(hr)) {
        dp->pso = NULL;
        de__d3d12_drain_messages(dd);
        return de__d3d12_fail(hr,
                              "%s(\"%s\"): CreatePipelineState (%s pipeline; see the debug "
                              "layer)",
                              F, n,
                              desc->kind == DE_PIPE_COMPUTE ? "compute"
                              : desc->kind == DE_PIPE_MESH  ? "mesh"
                                                            : "graphics");
    }
    dp->kind = desc->kind;
    de__d3d12_set_name((ID3D12Object *)dp->pso, p->name);
    return DE_OK;
}

void de__d3d12_pipeline_destroy(de_gpu_device *d, de__gpu_pipeline *p) {
    (void)d;
    d3d12_pipeline *dp = DE__GPU_PAYLOAD(d3d12_pipeline, p);
    if (dp->rt_state)
        ID3D12StateObject_Release(dp->rt_state);
    dp->rt_state = NULL;
    if (dp->pso)
        ID3D12PipelineState_Release(dp->pso);
    dp->pso = NULL;
}

/* ---------- Test hooks ---------- */

bool de__d3d12_pipelines_supported(de_gpu_device *d, const char **why) {
    const bool d3d12 = d && d->vt == &de__gpu_d3d12_vtable;
    const d3d12_pipe_state *ps = d3d12 ? de__d3d12(d)->pipe : NULL;
    const bool ok = ps && ps->root && ps->dev2;
    if (why)
        *why = ok ? "" : !d3d12 ? "not a D3D12 device" : ps ? ps->root_why : "no pipeline state";
    return ok;
}

#endif /* _WIN32 */
