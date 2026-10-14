/* d3d12_debug.c — D3D12 debug markers and object names (DENG-326; SPEC §18.3).
 *
 * Markers: ID3D12GraphicsCommandList::BeginEvent / EndEvent in the format PIX and RenderDoc read
 * without WinPixEventRuntime: metadata 0 (WINPIX_EVENT_UNICODE_VERSION) and the name as a
 * NUL-terminated UTF-16 string. The name is converted into a stack buffer (no allocation per
 * marker; names longer than 255 UTF-16 units are cut). That format has no colour, so the marker
 * colour is dropped on D3D12.
 *
 * Names: ID3D12Object::SetName (UTF-8 -> UTF-16, de__d3d12_set_name) on buffers and textures
 * (their resource), pipelines (the PSO), fences and query heaps. Samplers are descriptors, not
 * objects: they have no name. Command lists are named by cmd_begin (d3d12_cmd.c). The debug
 * layer, PIX and DRED quote these names.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

/* WINPIX_EVENT_UNICODE_VERSION (pix3.h): the event data is a UTF-16 string. */
#define DE__PIX_EVENT_UNICODE_VERSION 0u
#define MARKER_CHARS                  256

void de__d3d12_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color) {
    (void)color;
    wchar_t wide[MARKER_CHARS];
    int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, MARKER_CHARS);
    if (n <= 0) {
        /* Too long (or not UTF-8): keep what fits, cut at a code unit. */
        n = MultiByteToWideChar(CP_UTF8, 0, name, (int)strnlen(name, MARKER_CHARS - 1), wide,
                                MARKER_CHARS - 1);
        if (n < 0)
            n = 0;
        wide[n] = L'\0';
        n += 1;
    }
    ID3D12GraphicsCommandList_BeginEvent(de__d3d12_cmd(c)->list, DE__PIX_EVENT_UNICODE_VERSION,
                                         wide, (UINT)((size_t)n * sizeof(wchar_t)));
}

void de__d3d12_cmd_marker_pop(de_gpu_cmd *c) {
    ID3D12GraphicsCommandList_EndEvent(de__d3d12_cmd(c)->list);
}

/* The ID3D12Object behind a record of `kind`, or NULL. */
static ID3D12Object *object_of(de__gpu_kind kind, void *rec) {
    switch (kind) {
    case DE__GPU_BUFFER:
        return (ID3D12Object *)DE__GPU_PAYLOAD(d3d12_buffer, (de__gpu_buffer *)rec)->res;
    case DE__GPU_TEXTURE:
        return (ID3D12Object *)DE__GPU_PAYLOAD(d3d12_texture, (de__gpu_texture *)rec)->res;
    case DE__GPU_PIPELINE:
        return (ID3D12Object *)DE__GPU_PAYLOAD(d3d12_pipeline, (de__gpu_pipeline *)rec)->pso;
    case DE__GPU_FENCE:
        return (ID3D12Object *)DE__GPU_PAYLOAD(d3d12_fence, (de__gpu_fence *)rec)->fence;
    case DE__GPU_QUERY_HEAP:
        return (ID3D12Object *)DE__GPU_PAYLOAD(d3d12_query_heap, (de__gpu_query_heap *)rec)->heap;
    default:
        return NULL;
    }
}

void de__d3d12_object_name(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name) {
    (void)d;
    de__d3d12_set_name(object_of(kind, rec), name);
}

bool de__d3d12_object_debug_name(de_gpu_device *d, de__gpu_kind kind, u32 id, char *out,
                                 size_t cap) {
    if (out && cap)
        out[0] = '\0';
    if (!d || d->vt != &de__gpu_d3d12_vtable || !out || cap == 0)
        return false;
    void *rec = NULL;
    switch (kind) {
    case DE__GPU_BUFFER:
        rec = de__gpu_get_buffer(d, (de_gpu_buffer){id});
        break;
    case DE__GPU_TEXTURE:
        rec = de__gpu_get_texture(d, (de_gpu_texture){id});
        break;
    case DE__GPU_PIPELINE:
        rec = de__gpu_get_pipeline(d, (de_gpu_pipeline){id});
        break;
    case DE__GPU_FENCE:
        rec = de__gpu_get_fence(d, (de_gpu_fence){id});
        break;
    case DE__GPU_QUERY_HEAP:
        rec = de__gpu_get_query_heap(d, (de_gpu_query_heap){id});
        break;
    default:
        break;
    }
    ID3D12Object *obj = rec ? object_of(kind, rec) : NULL;
    if (!obj)
        return false;
    wchar_t wide[128];
    UINT size = (UINT)sizeof wide;
    if (FAILED(ID3D12Object_GetPrivateData(obj, &WKPDID_D3DDebugObjectNameW, &size, wide)))
        return false;
    const int chars = (int)(size / sizeof(wchar_t));
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide, chars, out, (int)cap - 1, NULL, NULL);
    if (n <= 0)
        return false;
    out[n] = '\0';
    /* The stored name may include its terminator. */
    return true;
}
