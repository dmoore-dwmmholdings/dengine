/*
 * model_ir.c: the model IR's memory, warnings and geometry helpers (DENG-101): area-weighted
 * normals, MikkTSpace tangents, bounds; and the byte builder of model_internal.h. See model_ir.h.
 */
#include "model_internal.h"
#include "model_ir.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>

#include <mikktspace.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Memory ---------------------------------------------------------------------------------- */

/* Each allocation has this header in front; the blocks form a singly linked list. */
struct de__model_block {
    de__model_block *next;
    size_t total;
    u8 pad[16 - (sizeof(void *) + sizeof(size_t)) % 16];
};

void de_model_ir_init(de_model_ir *ir, const de_allocator *a, const char *log_name) {
    memset(ir, 0, sizeof *ir);
    ir->alloc = a != NULL ? a : de_heap_allocator();
    ir->log_name = log_name != NULL ? log_name : "";
}

void de_model_ir_free(de_model_ir *ir) {
    const de_allocator *a = ir->alloc;
    de__model_block *b = ir->blocks;
    while (b != NULL) {
        de__model_block *next = b->next;
        DE_FREE(a, b, b->total);
        b = next;
    }
    const char *log_name = ir->log_name;
    memset(ir, 0, sizeof *ir);
    ir->alloc = a;
    ir->log_name = log_name;
}

void *de_model_ir_alloc(de_model_ir *ir, u64 size) {
    size_t head = sizeof(de__model_block);
    if (size > (u64)(SIZE_MAX / 2u) - head) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "model import: %llu bytes do not fit",
                      (unsigned long long)size);
        return NULL;
    }
    size_t total = head + (size_t)size;
    de__model_block *b = (de__model_block *)DE_ALLOC(ir->alloc, total, 16, DE_MEM_ASSET);
    if (b == NULL) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "model import: out of memory (%llu bytes)",
                      (unsigned long long)size);
        return NULL;
    }
    memset(b, 0, total);
    b->total = total;
    b->next = ir->blocks;
    ir->blocks = b;
    return (u8 *)b + head;
}

void *de_model_ir_array(de_model_ir *ir, u64 count, u64 elem) {
    if (elem != 0 && count > (u64)UINT32_MAX * 64u / elem) {
        (void)DE_FAIL(DE_ERR_OUT_OF_MEMORY, "model import: %llu elements do not fit",
                      (unsigned long long)count);
        return NULL;
    }
    return de_model_ir_alloc(ir, count * elem);
}

const char *de_model_ir_strdup(de_model_ir *ir, const char *s) {
    if (s == NULL)
        s = "";
    size_t n = strlen(s) + 1u;
    char *d = (char *)de_model_ir_alloc(ir, n);
    if (d != NULL)
        memcpy(d, s, n);
    return d;
}

static de_log_category model_log(void) {
    return de_log_register_category("cook", DE_LOG_INFO);
}

void de_model_ir_warn(de_model_ir *ir, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    DE_WARN(model_log(), "%s: %s", ir->log_name, msg);
    if (ir->warning_count == ir->warning_cap) {
        u32 ncap = ir->warning_cap != 0 ? ir->warning_cap * 2u : 16u;
        const char **n = (const char **)de_model_ir_array(ir, ncap, sizeof(char *));
        if (n == NULL)
            return; /* the warning is logged; only the record is lost */
        if (ir->warning_count != 0)
            memcpy((void *)n, (const void *)ir->warnings, sizeof(char *) * ir->warning_count);
        ir->warnings = n;
        ir->warning_cap = ncap;
    }
    const char *copy = de_model_ir_strdup(ir, msg);
    if (copy != NULL)
        ir->warnings[ir->warning_count++] = copy;
}

/* ---- Normals --------------------------------------------------------------------------------- */

void de_model_generate_normals(const f32 *position, f32 *normal, u32 vertex_first, u32 vertex_count,
                               const u32 *indices, u32 index_count) {
    /* Accumulated in f64 in triangle order: the same input gives the same bits everywhere. */
    for (u32 i = 0; i < vertex_count; ++i)
        for (u32 k = 0; k < 3; ++k)
            normal[3u * (vertex_first + i) + k] = 0.0f;
    for (u32 t = 0; t + 2u < index_count; t += 3u) {
        u32 a = indices[t], b = indices[t + 1], c = indices[t + 2];
        const f32 *pa = position + 3u * a, *pb = position + 3u * b, *pc = position + 3u * c;
        f64 e1[3], e2[3], n[3];
        for (u32 k = 0; k < 3; ++k) {
            e1[k] = (f64)pb[k] - (f64)pa[k];
            e2[k] = (f64)pc[k] - (f64)pa[k];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        const u32 v[3] = {a, b, c};
        for (u32 j = 0; j < 3; ++j)
            for (u32 k = 0; k < 3; ++k)
                normal[3u * v[j] + k] = (f32)((f64)normal[3u * v[j] + k] + n[k]);
    }
    for (u32 i = 0; i < vertex_count; ++i) {
        f32 *n = normal + 3u * (vertex_first + i);
        f64 len = sqrt((f64)n[0] * n[0] + (f64)n[1] * n[1] + (f64)n[2] * n[2]);
        if (len > 1e-20) {
            for (u32 k = 0; k < 3; ++k)
                n[k] = (f32)((f64)n[k] / len);
        } else {
            n[0] = 0.0f;
            n[1] = 0.0f;
            n[2] = 1.0f;
        }
    }
}

/* ---- Tangents (MikkTSpace) ------------------------------------------------------------------- */

typedef struct mikk_user {
    const f32 *position, *normal, *uv;
    f32 *tangent;
    const u32 *indices;
    u32 face_count;
} mikk_user;

static int mk_faces(const SMikkTSpaceContext *c) {
    return (int)((const mikk_user *)c->m_pUserData)->face_count;
}
static int mk_verts(const SMikkTSpaceContext *c, const int face) {
    (void)c;
    (void)face;
    return 3;
}
static u32 mk_index(const SMikkTSpaceContext *c, int face, int vert) {
    return ((const mikk_user *)c->m_pUserData)->indices[3 * face + vert];
}
static void mk_pos(const SMikkTSpaceContext *c, float out[], const int face, const int vert) {
    const f32 *p = ((const mikk_user *)c->m_pUserData)->position + 3u * mk_index(c, face, vert);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}
static void mk_nrm(const SMikkTSpaceContext *c, float out[], const int face, const int vert) {
    const f32 *p = ((const mikk_user *)c->m_pUserData)->normal + 3u * mk_index(c, face, vert);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
}
static void mk_uv(const SMikkTSpaceContext *c, float out[], const int face, const int vert) {
    const f32 *p = ((const mikk_user *)c->m_pUserData)->uv + 2u * mk_index(c, face, vert);
    out[0] = p[0];
    out[1] = p[1];
}
static void mk_set(const SMikkTSpaceContext *c, const float t[], const float sign, const int face,
                   const int vert) {
    f32 *p = ((mikk_user *)c->m_pUserData)->tangent + 4u * mk_index(c, face, vert);
    p[0] = t[0];
    p[1] = t[1];
    p[2] = t[2];
    p[3] = sign < 0.0f ? -1.0f : 1.0f;
}

bool de_model_generate_tangents(const f32 *position, const f32 *normal, const f32 *uv, f32 *tangent,
                                const u32 *indices, u32 index_count) {
    if (index_count < 3u)
        return true;
    mikk_user u = {position, normal, uv, tangent, indices, index_count / 3u};
    SMikkTSpaceInterface it;
    memset(&it, 0, sizeof it);
    it.m_getNumFaces = mk_faces;
    it.m_getNumVerticesOfFace = mk_verts;
    it.m_getPosition = mk_pos;
    it.m_getNormal = mk_nrm;
    it.m_getTexCoord = mk_uv;
    it.m_setTSpaceBasic = mk_set;
    SMikkTSpaceContext ctx = {&it, &u};
    return genTangSpaceDefault(&ctx) != 0;
}

/* ---- Bounds ---------------------------------------------------------------------------------- */

/* The bounds of the vertices that indices[first .. first + count) name. */
static void bounds_of(const f32 *position, const u32 *indices, u32 first, u32 count, f32 mn[3],
                      f32 mx[3]) {
    for (u32 k = 0; k < 3; ++k) {
        mn[k] = INFINITY;
        mx[k] = -INFINITY;
    }
    for (u32 i = 0; i < count; ++i) {
        const f32 *p = position + 3u * indices[first + i];
        for (u32 k = 0; k < 3; ++k) {
            mn[k] = p[k] < mn[k] ? p[k] : mn[k];
            mx[k] = p[k] > mx[k] ? p[k] : mx[k];
        }
    }
    if (count == 0)
        for (u32 k = 0; k < 3; ++k)
            mn[k] = mx[k] = 0.0f;
}

void de_model_ir_compute_bounds(de_model_ir *ir) {
    for (u32 m = 0; m < ir->mesh_count; ++m) {
        de_model_mesh *mesh = &ir->meshes[m];
        /* The mesh: every vertex (also one no triangle uses). */
        for (u32 k = 0; k < 3; ++k) {
            mesh->bounds_min[k] = mesh->vertex_count != 0 ? INFINITY : 0.0f;
            mesh->bounds_max[k] = mesh->vertex_count != 0 ? -INFINITY : 0.0f;
        }
        for (u32 v = 0; v < mesh->vertex_count; ++v)
            for (u32 k = 0; k < 3; ++k) {
                f32 x = mesh->position[3u * v + k];
                mesh->bounds_min[k] = x < mesh->bounds_min[k] ? x : mesh->bounds_min[k];
                mesh->bounds_max[k] = x > mesh->bounds_max[k] ? x : mesh->bounds_max[k];
            }
        for (u32 s = 0; s < mesh->submesh_count; ++s) {
            de_model_submesh *sm = &mesh->submeshes[s];
            bounds_of(mesh->position, mesh->indices, sm->index_first, sm->index_count,
                      sm->bounds_min, sm->bounds_max);
        }
    }
}

/* ---- Byte builder ---------------------------------------------------------------------------- */

void de_model_buf_init(de_model_buf *b, de_model_ir *ir) {
    memset(b, 0, sizeof *b);
    b->ir = ir;
}

/* Grows to hold `need` bytes. The old block stays with the IR (freed with it). */
static bool buf_reserve(de_model_buf *b, u64 need) {
    if (b->failed)
        return false;
    if (need <= b->cap)
        return true;
    u64 ncap = b->cap != 0 ? b->cap : 256u;
    while (ncap < need) {
        if (ncap > (u64)UINT32_MAX * 4u) {
            b->failed = true;
            return false;
        }
        ncap *= 2u;
    }
    u8 *n = (u8 *)de_model_ir_alloc(b->ir, ncap);
    if (n == NULL) {
        b->failed = true;
        return false;
    }
    if (b->size != 0)
        memcpy(n, b->data, (size_t)b->size);
    b->data = n;
    b->cap = ncap;
    return true;
}

u64 de_model_buf_put(de_model_buf *b, const void *p, u64 n) {
    u64 at = b->size;
    if (!buf_reserve(b, b->size + n))
        return at;
    if (p != NULL)
        memcpy(b->data + b->size, p, (size_t)n);
    else
        memset(b->data + b->size, 0, (size_t)n);
    b->size += n;
    return at;
}

void de_model_buf_u32(de_model_buf *b, u32 v) {
    u8 x[4];
    de_cooked_put_u32(x, v);
    de_model_buf_put(b, x, 4);
}

void de_model_buf_u64(de_model_buf *b, u64 v) {
    u8 x[8];
    de_cooked_put_u64(x, v);
    de_model_buf_put(b, x, 8);
}

void de_model_buf_f32(de_model_buf *b, f32 v) {
    u8 x[4];
    de_cooked_put_f32(x, v);
    de_model_buf_put(b, x, 4);
}

void de_model_buf_align(de_model_buf *b, u64 align) {
    u64 pad = (align - b->size % align) % align;
    if (pad != 0)
        de_model_buf_put(b, NULL, pad);
}

void de_model_buf_printf(de_model_buf *b, const char *fmt, ...) {
    char small[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->failed = true;
        return;
    }
    if ((size_t)n < sizeof small) {
        de_model_buf_put(b, small, (u64)n);
        return;
    }
    if (!buf_reserve(b, b->size + (u64)n + 1u))
        return;
    va_start(ap, fmt);
    vsnprintf((char *)b->data + b->size, (size_t)n + 1u, fmt, ap);
    va_end(ap);
    b->size += (u64)n;
}

void de_model_buf_patch_u32(de_model_buf *b, u64 off, u32 v) {
    if (!b->failed && off + 4u <= b->size)
        de_cooked_put_u32(b->data + off, v);
}
