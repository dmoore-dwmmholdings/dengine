/* editor_views.c: views as shared D3D12 textures, picking and input (DENG-631); the gizmo and
 * the overlays are editor_gizmo.c's (DENG-650).
 *
 * A view renders through the engine's renderer: an editor view is a render-to-texture de_view
 * with an editor camera entity (DeCamera.view) driven by view.setCamera; a Game view reads the
 * main view, so the game's UI is in it. A render hook at DE_HOOK_UI copies the view's colour
 * (RGBA16F, UI included) into a ring of readback buffers on the render thread. Two frames later
 * the frame has completed on the GPU, and the main thread converts it to 8-bit sRGB
 * (`rgba8_srgb`, what the IDE's presenter shows; clamped, no HDR yet) into a ring of 3 textures
 * that a small D3D12 device of the service shares with the IDE by named NT handles, signals a
 * named shared fence and sends view.frame. This CPU bridge works whatever backend renders
 * (D3D12 or Vulkan); a zero-copy path is DENG-556.
 *
 * Windows only: elsewhere views.sharedTexture is not offered and view.create is unsupported. */
#if defined(_WIN32)
#define _CRT_SECURE_NO_WARNINGS /* snprintf */
#endif
#include "host/editor/editor_internal.h"
#include "host/editor/editor_gizmo.h"

#include <dengine/de_debug.h>
#include <dengine/de_gpu.h>
#include <dengine/de_input.h>
#include <dengine/de_platform.h>
#include <dengine/de_render.h>

#include "host/host_internal.h"
#include "input/input_internal.h"
#include "platform/de_atomic.h"
#include "render/loaders/render_assets.h"
#include "render/renderer_internal.h"
#include "render/world/render_world.h"
#include "render/views_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && DE_HAS_RENDER && DE_HAS_GPU_D3D12
#define ED_WIN_VIEWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define COBJMACROS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#undef near /* windef.h: the camera has fields named near and far */
#undef far
#else
#define ED_WIN_VIEWS 0
#endif

#if defined(__APPLE__) && DE_HAS_RENDER && DE_HAS_GPU_METAL
#define ED_MAC_VIEWS 1
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#else
#define ED_MAC_VIEWS 0
#endif
#define ED_VIEWS (ED_WIN_VIEWS || ED_MAC_VIEWS)

#define ED_MAX_VIEWS 8u
#define ED_RB_SLOTS  4u
#define ED_RING      3u
#define ED_MAX_RINGS 4u
#define ED_MAX_PICKS 16u

/* ---------- Shared ring (the bridge's D3D12 device) ---------- */

#if ED_WIN_VIEWS
typedef struct ed_ring {
    u32 generation;
    u32 w, h;
    ID3D12Resource *tex[ED_RING];
    HANDLE handle[ED_RING];
    u64 last_fence;
    bool released;
} ed_ring;

typedef struct ed_bridge {
    ID3D12Device *dev;
    ID3D12CommandQueue *queue;
    ID3D12CommandAllocator *alloc;
    ID3D12GraphicsCommandList *list;
    ID3D12Fence *wait;
    u64 wait_value;
    HANDLE event;
    char adapter[24];
    char prefix[48];
} ed_bridge;
#endif

#if ED_MAC_VIEWS
/* Private, same-user shared frame files. A sequence word protects the pixel copy. */
typedef struct ed_ring {
    u32 generation, w, h;
    char path[ED_RING][128];
    void *map[ED_RING];
    size_t bytes;
    u64 last_fence;
    bool released;
} ed_ring;
typedef struct ed_bridge { char adapter[24]; } ed_bridge;
#endif

#define ED_PICK_SKIP 128u

typedef struct ed_pick {
    u64 req_id, render_id;
    u32 view;
    f32 x, y; /* source pixels */
    u64 skip[ED_PICK_SKIP]; /* see through these (Alt-click cycling, locked entities) */
    u32 skip_count;
    u64 sprite; /* the nearest sprite under the cursor: the answer when the GPU sees nothing */
    bool pending;
    u32 frames; /* frames waited */
} ed_pick;

typedef struct ed_view {
    u32 id; /* 0: free */
    bool game, paused, hidden_mask;
    u32 w, h;   /* the IDE's size */
    u32 sw, sh; /* the source size (what the readback holds) */
    f32 scale;
    u32 max_fps;
    u64 next_readback_ns;  /* render thread; reset while idle on a policy change */
    de_view rv;            /* the renderer view (the main view for a Game view) */
    de_gpu_texture target; /* an editor view's render target */
    de_entity cam;         /* an editor view's camera entity */
    bool cam_dirty;        /* the IDE's camera moved: write it to the entity */
    de_ep_camera camera;   /* the IDE's camera */
    f64 cam_pos[3];
    f32 cam_rot[4];
    char render_mode[32];
    /* Readback ring, written on the render thread. */
    de_gpu_buffer rb[ED_RB_SLOTS];
    const u8 *rb_map[ED_RB_SLOTS];
    u32 rb_pitch, rb_w, rb_h;
    de_atomic_u64 produced; /* readbacks recorded so far */
    u64 consumed;           /* readbacks handed to the IDE */
    u64 frames;             /* view.frame count */
    bool focused, captured;
    u32 show; /* DE__ED_SHOW_* */
#if ED_VIEWS
    ed_ring rings[ED_MAX_RINGS];
    u32 ring_count;
    u32 next_generation;
#if ED_WIN_VIEWS
    ID3D12Fence *fence;
    HANDLE fence_handle;
    char fence_name[96];
    u64 fence_value;
    ID3D12Resource *upload;
    u64 upload_bytes;
    u8 *upload_map;
#else
    u64 fence_value;
#endif
#endif
} ed_view;

typedef struct de__ed_views {
    ed_view views[ED_MAX_VIEWS];
    u32 next_id;
    bool hook;
    ed_pick picks[ED_MAX_PICKS];
    u64 next_pick;
    const de__view_hidden *hidden;
    de_component c_camera;
#if ED_VIEWS
    ed_bridge bridge;
    bool bridge_ok;
#endif
} de__ed_views;

static de__ed_views g_views;
static de__editor *g_ed; /* the render hook's way back */

void de__ed_each_bounds(de__editor *ed,
                        void (*fn)(u64 e, de_dvec3 lo, de_dvec3 hi, const de__ed_mesh_geo *geo, void *user),
                        void *user) {
#if DE_HAS_RENDER
    const de_renderer *r = ed->e->renderer;
    const de__render_world *rw = r != NULL ? r->render_world : NULL;
    if (rw == NULL || !rw->origin_valid)
        return;
    for (u32 s = 0; s < rw->inst.count; ++s) {
        if (rw->inst.entity[s] == 0 || !(rw->inst.flags[s] & DE__RW_INST_HAS_BOUNDS) ||
            (rw->inst.flags[s] & DE__RW_INST_SPRITE))
            continue;
        const de_aabb b = rw->inst.bounds[s];
        if (!(b.max.x >= b.min.x))
            continue;
        const de_render_mesh *m =
            ed->e->assets != NULL ? de_render_mesh_get(ed->e->assets, rw->inst.mesh[s]) : NULL;
        de__ed_mesh_geo geo;
        const bool has_geo = m != NULL && m->cpu_positions != NULL;
        if (has_geo) {
            geo.positions = m->cpu_positions;
            geo.triangles = m->cpu_triangles;
            geo.vertex_count = m->cpu_vertex_count;
            geo.triangle_count = m->cpu_triangle_count;
            memcpy(geo.model, rw->inst.transform[s].m, sizeof geo.model);
            geo.origin = rw->origin;
        }
        fn(rw->inst.entity[s], (de_dvec3){rw->origin.x + b.min.x, rw->origin.y + b.min.y, rw->origin.z + b.min.z},
           (de_dvec3){rw->origin.x + b.max.x, rw->origin.y + b.max.y, rw->origin.z + b.max.z},
           has_geo ? &geo : NULL, user);
    }
#else
    (void)ed;
    (void)fn;
    (void)user;
#endif
}

bool de__ed_entity_bounds(de__editor *ed, u64 e, de_dvec3 *min, de_dvec3 *max) {
#if DE_HAS_RENDER
    const de_renderer *r = ed->e->renderer;
    const de__render_world *rw = r != NULL ? r->render_world : NULL;
    if (rw == NULL || !rw->origin_valid)
        return false;
    const u32 s = de__rw_pool_find(rw, DE__RW_KIND_INSTANCE, (de_entity){e});
    if (s == DE__RW_NONE || s >= rw->inst.count || !(rw->inst.flags[s] & DE__RW_INST_HAS_BOUNDS) ||
        (rw->inst.flags[s] & DE__RW_INST_SPRITE))
        return false;
    const de_aabb b = rw->inst.bounds[s];
    if (!(b.max.x >= b.min.x))
        return false;
    *min = (de_dvec3){rw->origin.x + b.min.x, rw->origin.y + b.min.y, rw->origin.z + b.min.z};
    *max = (de_dvec3){rw->origin.x + b.max.x, rw->origin.y + b.max.y, rw->origin.z + b.max.z};
    return true;
#else
    (void)ed;
    (void)e;
    (void)min;
    (void)max;
    return false;
#endif
}

bool de__ed_views_owns(u64 entity) {
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i)
        if (g_views.views[i].id != 0 && !g_views.views[i].game && g_views.views[i].cam.bits == entity &&
            entity != 0)
            return true;
    return false;
}

/* The editor views' own camera entities (no icon, no frame): their count. */
static u32 editor_cameras(u64 out[ED_MAX_VIEWS]) {
    u32 n = 0;
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i)
        if (g_views.views[i].id != 0 && !g_views.views[i].game)
            out[n++] = g_views.views[i].cam.bits;
    return n;
}

/* The view's camera, for the gizmo and the overlays (source pixels). */
static void cam_of(const ed_view *v, de__ed_cam *c) {
    memcpy(c->pos, v->cam_pos, sizeof c->pos);
    memcpy(c->rot, v->cam_rot, sizeof c->rot);
    const bool orth = v->camera.has_ortho_height || !(v->camera.fov_y > 0);
    c->fov_y = orth ? 0.0 : v->camera.fov_y;
    c->ortho_h = v->camera.has_ortho_height ? v->camera.ortho_height : 10.0;
    c->near = v->camera.near > 0 ? v->camera.near : 0.1;
    c->w = v->sw;
    c->h = v->sh;
}

static ed_view *view_find(u32 id) {
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i)
        if (g_views.views[i].id == id && id != 0)
            return &g_views.views[i];
    return NULL;
}

#if ED_WIN_VIEWS
/* ---------- The bridge device ---------- */

static bool parse_luid(const char *s, LUID *out) {
    char *end = NULL;
    const unsigned long long v = strtoull(s, &end, 16);
    if (end == s)
        return false;
    out->LowPart = (DWORD)(v & 0xFFFFFFFFu);
    out->HighPart = (LONG)(v >> 32);
    return true;
}

static bool bridge_init(const char *adapter) {
    ed_bridge *b = &g_views.bridge;
    if (g_views.bridge_ok)
        return true;
    IDXGIFactory4 *factory = NULL;
    if (FAILED(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&factory)))
        return false;
    IDXGIAdapter1 *a = NULL;
    LUID luid;
    if (adapter != NULL && parse_luid(adapter, &luid)) {
        if (FAILED(IDXGIFactory4_EnumAdapterByLuid(factory, luid, &IID_IDXGIAdapter1, (void **)&a)))
            a = NULL;
    }
    for (UINT i = 0; a == NULL; ++i) {
        IDXGIAdapter1 *c = NULL;
        if (FAILED(IDXGIFactory4_EnumAdapters1(factory, i, &c)))
            break;
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(c, &d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            a = c;
        else
            IDXGIAdapter1_Release(c);
    }
    IDXGIFactory4_Release(factory);
    if (a == NULL)
        return false;
    DXGI_ADAPTER_DESC1 desc;
    IDXGIAdapter1_GetDesc1(a, &desc);
    snprintf(b->adapter, sizeof b->adapter, "%08x%08x", (unsigned)desc.AdapterLuid.HighPart,
             (unsigned)desc.AdapterLuid.LowPart);
    const HRESULT hr = D3D12CreateDevice((IUnknown *)a, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device,
                                         (void **)&b->dev);
    IDXGIAdapter1_Release(a);
    if (FAILED(hr))
        return false;
    D3D12_COMMAND_QUEUE_DESC qd;
    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(b->dev, &qd, &IID_ID3D12CommandQueue, (void **)&b->queue)) ||
        FAILED(ID3D12Device_CreateCommandAllocator(b->dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   &IID_ID3D12CommandAllocator, (void **)&b->alloc)) ||
        FAILED(ID3D12Device_CreateCommandList(b->dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, b->alloc, NULL,
                                              &IID_ID3D12GraphicsCommandList, (void **)&b->list)) ||
        FAILED(ID3D12Device_CreateFence(b->dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&b->wait)))
        return false;
    ID3D12GraphicsCommandList_Close(b->list);
    b->event = CreateEventA(NULL, FALSE, FALSE, NULL);
    snprintf(b->prefix, sizeof b->prefix, "dengine-%lu", (unsigned long)GetCurrentProcessId());
    g_views.bridge_ok = true;
    return true;
}

static void bridge_wait(void) {
    ed_bridge *b = &g_views.bridge;
    ID3D12CommandQueue_Signal(b->queue, b->wait, ++b->wait_value);
    if (ID3D12Fence_GetCompletedValue(b->wait) < b->wait_value) {
        ID3D12Fence_SetEventOnCompletion(b->wait, b->wait_value, b->event);
        WaitForSingleObject(b->event, INFINITE);
    }
}

static void wname(char *name, WCHAR *out, size_t cap) {
    size_t i = 0;
    for (; name[i] && i + 1 < cap; ++i)
        out[i] = (WCHAR)name[i];
    out[i] = 0;
}

static void ring_free(ed_ring *r) {
    for (u32 i = 0; i < ED_RING; ++i) {
        if (r->handle[i] != NULL)
            CloseHandle(r->handle[i]);
        if (r->tex[i] != NULL)
            ID3D12Resource_Release(r->tex[i]);
    }
    memset(r, 0, sizeof *r);
}

static bool ring_create(ed_view *v, u32 w, u32 h) {
    ed_bridge *b = &g_views.bridge;
    if (v->ring_count == ED_MAX_RINGS) {
        /* Drop the oldest released ring (the IDE let go of it). */
        u32 k = 0;
        while (k < v->ring_count - 1 && !v->rings[k].released)
            ++k;
        ring_free(&v->rings[k]);
        memmove(&v->rings[k], &v->rings[k + 1], sizeof(ed_ring) * (v->ring_count - k - 1));
        --v->ring_count;
    }
    ed_ring *r = &v->rings[v->ring_count];
    memset(r, 0, sizeof *r);
    r->generation = v->next_generation++;
    r->w = w ? w : 1;
    r->h = h ? h : 1;
    for (u32 i = 0; i < ED_RING; ++i) {
        D3D12_RESOURCE_DESC d;
        memset(&d, 0, sizeof d);
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = r->w;
        d.Height = r->h;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        D3D12_HEAP_PROPERTIES hp;
        memset(&hp, 0, sizeof hp);
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(ID3D12Device_CreateCommittedResource(b->dev, &hp, D3D12_HEAP_FLAG_SHARED, &d,
                                                        D3D12_RESOURCE_STATE_COMMON, NULL,
                                                        &IID_ID3D12Resource, (void **)&r->tex[i]))) {
            ring_free(r);
            return false;
        }
        char name[128];
        WCHAR wn[128];
        snprintf(name, sizeof name, "%s-v%u-g%u-s%u", b->prefix, v->id, r->generation, i);
        wname(name, wn, 128);
        if (FAILED(ID3D12Device_CreateSharedHandle(b->dev, (ID3D12DeviceChild *)r->tex[i], NULL,
                                                   GENERIC_ALL, wn, &r->handle[i]))) {
            ring_free(r);
            return false;
        }
    }
    ++v->ring_count;
    return true;
}

static bool view_gpu_create(ed_view *v) {
    ed_bridge *b = &g_views.bridge;
    if (FAILED(ID3D12Device_CreateFence(b->dev, 0, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, (void **)&v->fence)))
        return false;
    snprintf(v->fence_name, sizeof v->fence_name, "%s-v%u-fence", b->prefix, v->id);
    WCHAR wn[128];
    wname(v->fence_name, wn, 128);
    if (FAILED(ID3D12Device_CreateSharedHandle(b->dev, (ID3D12DeviceChild *)v->fence, NULL, GENERIC_ALL, wn,
                                               &v->fence_handle)))
        return false;
    v->next_generation = 1;
    return ring_create(v, v->sw, v->sh);
}

static void view_gpu_free(ed_view *v) {
    if (g_views.bridge_ok)
        bridge_wait();
    for (u32 i = 0; i < v->ring_count; ++i)
        ring_free(&v->rings[i]);
    v->ring_count = 0;
    if (v->upload != NULL)
        ID3D12Resource_Release(v->upload);
    v->upload = NULL;
    if (v->fence_handle != NULL)
        CloseHandle(v->fence_handle);
    if (v->fence != NULL)
        ID3D12Fence_Release(v->fence);
    v->fence = NULL;
    v->fence_handle = NULL;
}

#endif /* ED_WIN_VIEWS */
#if ED_MAC_VIEWS
#include "editor_views_macos.inc"
#endif
#if ED_VIEWS
static void quat_rotate(const f32 q[4], const f64 v[3], f64 out[3]);

/* ---------- Overlay: debug draw and the gizmo, drawn into the frame on the CPU ---------- */

u32 de__ed_dd_lines(const de_draw_line_item **out);

/* World point to view pixels; false behind the near plane. DEngine axes: +X right, +Y forward,
 * +Z up. */
static bool project(const ed_view *v, de_dvec3 p, f64 *x, f64 *y, f64 *depth) {
    const f64 d[3] = {p.x - v->cam_pos[0], p.y - v->cam_pos[1], p.z - v->cam_pos[2]};
    const f32 inv[4] = {-v->cam_rot[0], -v->cam_rot[1], -v->cam_rot[2], v->cam_rot[3]};
    f64 l[3];
    quat_rotate(inv, d, l);
    const f64 nearz = v->camera.near > 0 ? v->camera.near : 0.1;
    *depth = l[1];
    if (l[1] < nearz)
        return false;
    const f64 aspect = (f64)v->sw / (f64)(v->sh ? v->sh : 1);
    f64 nx, ny;
    if (v->camera.fov_y > 0) {
        const f64 t = tan(v->camera.fov_y * 0.5);
        nx = l[0] / (l[1] * t * aspect);
        ny = l[2] / (l[1] * t);
    } else {
        const f64 hh = (v->camera.has_ortho_height ? v->camera.ortho_height : 10.0) * 0.5;
        nx = l[0] / (hh * aspect);
        ny = l[2] / hh;
    }
    *x = (nx * 0.5 + 0.5) * v->sw;
    *y = (0.5 - ny * 0.5) * v->sh;
    return true;
}

/* Linear [0, 1] to an sRGB byte (clamped). */
static u8 srgb_byte(f32 c) {
    if (!(c > 0.0f))
        return 0;
    if (c >= 1.0f)
        return 255;
    const f32 e = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (u8)(e * 255.0f + 0.5f);
}

/* Every half float as an sRGB byte (colour channels) and as a linear byte (alpha). */
static u8 g_half_srgb[65536], g_half_lin[65536];
static bool g_half_ready;

static f32 float_of_half(u16 h) {
    const u32 e = (h >> 10) & 31u, m = h & 1023u;
    f32 f = e == 0 ? (f32)m / 16777216.0f : e == 31 ? 65504.0f : (1.0f + (f32)m / 1024.0f) * ldexpf(1.0f, (i32)e - 15);
    return (h & 0x8000u) ? -f : f;
}

static void half_tables(void) {
    if (g_half_ready)
        return;
    for (u32 i = 0; i < 65536; ++i) {
        const f32 f = float_of_half((u16)i);
        g_half_srgb[i] = srgb_byte(f);
        g_half_lin[i] = f <= 0 ? 0 : f >= 1 ? 255 : (u8)(f * 255.0f + 0.5f);
    }
    g_half_ready = true;
}

static void plot(u8 *map, u32 pitch, u32 w, u32 h, i32 x, i32 y, const u8 rgba[4]) {
    if (x < 0 || y < 0 || (u32)x >= w || (u32)y >= h)
        return;
    memcpy(map + (size_t)y * pitch + (size_t)x * 4u, rgba, 4);
}

static void line_px(u8 *map, u32 pitch, u32 w, u32 h, f64 x0, f64 y0, f64 x1, f64 y1, de_vec4 c) {
    const u8 rgba[4] = {srgb_byte(c.x), srgb_byte(c.y), srgb_byte(c.z), 255};
    const f64 dx = x1 - x0, dy = y1 - y0;
    f64 n = fabs(dx) > fabs(dy) ? fabs(dx) : fabs(dy);
    if (n > 8192)
        n = 8192;
    for (i32 i = 0; i <= (i32)n; ++i) {
        const f64 t = n > 0 ? i / n : 0;
        plot(map, pitch, w, h, (i32)(x0 + dx * t), (i32)(y0 + dy * t), rgba);
    }
}

static void line_world(const ed_view *v, u8 *map, u32 pitch, u32 w, u32 h, de_dvec3 a, de_dvec3 b,
                       de_vec4 c) {
    f64 ax, ay, ad, bx, by, bd;
    const bool va = project(v, a, &ax, &ay, &ad), vb = project(v, b, &bx, &by, &bd);
    if (!va && !vb)
        return;
    if (!va || !vb) {
        /* Clip to the near plane: walk from the visible end. */
        de_dvec3 in = va ? a : b, out = va ? b : a;
        for (u32 k = 0; k < 20; ++k) {
            const de_dvec3 mid = {(in.x + out.x) * 0.5, (in.y + out.y) * 0.5, (in.z + out.z) * 0.5};
            f64 mx, my, md;
            if (project(v, mid, &mx, &my, &md))
                in = mid;
            else
                out = mid;
        }
        if (va)
            (void)project(v, in, &bx, &by, &bd);
        else
            (void)project(v, in, &ax, &ay, &ad);
    }
    line_px(map, pitch, w, h, ax, ay, bx, by, c);
}

static void overlay(ed_view *v, u8 *map, u32 pitch, u32 w, u32 h) {
    de__editor *ed = g_ed;
    if (v->game || ed == NULL)
        return;
    const de_draw_line_item *lines = NULL;
    const u32 n = de__ed_dd_lines(&lines);
    for (u32 i = 0; i < n; ++i)
        line_world(v, map, pitch, w, h, lines[i].a, lines[i].b, lines[i].color);
    /* The grid, outlines, camera frames and the gizmo. */
    de__ed_cam c;
    cam_of(v, &c);
    u64 skip[ED_MAX_VIEWS];
    const u32 ns = editor_cameras(skip);
    const de__ed_canvas cv = {map, pitch, w, h};
    de__ed_gizmo_draw(ed, v->id, &c, &cv, v->show, skip, ns);
}

/* Converts `src` (rows of `pitch` bytes, linear RGBA16F) to sRGB RGBA8 in the newest ring. */
#if ED_WIN_VIEWS
static bool present(ed_view *v, const u8 *src, u32 pitch, u32 w, u32 h, u32 *surface, u64 *fence) {
    ed_bridge *b = &g_views.bridge;
    ed_ring *r = &v->rings[v->ring_count - 1];
    if (r->w != w || r->h != h)
        return false;
    const u32 dpitch = (w * 4u + 255u) & ~255u;
    const u64 need = (u64)dpitch * h;
    if (v->upload == NULL || v->upload_bytes < need) {
        if (v->upload != NULL)
            ID3D12Resource_Release(v->upload);
        v->upload = NULL;
        D3D12_RESOURCE_DESC d;
        memset(&d, 0, sizeof d);
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = need;
        d.Height = 1;
        d.DepthOrArraySize = 1;
        d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES hp;
        memset(&hp, 0, sizeof hp);
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (FAILED(ID3D12Device_CreateCommittedResource(b->dev, &hp, D3D12_HEAP_FLAG_NONE, &d,
                                                        D3D12_RESOURCE_STATE_GENERIC_READ, NULL,
                                                        &IID_ID3D12Resource, (void **)&v->upload)))
            return false;
        v->upload_bytes = need;
        if (FAILED(ID3D12Resource_Map(v->upload, 0, NULL, (void **)&v->upload_map)))
            return false;
    }
    half_tables();
    for (u32 y = 0; y < h; ++y) {
        const u16 *in = (const u16 *)(src + (size_t)y * pitch);
        u8 *out = v->upload_map + (size_t)y * dpitch;
        for (u32 x = 0; x < w; ++x, in += 4, out += 4) {
            out[0] = g_half_srgb[in[0]];
            out[1] = g_half_srgb[in[1]];
            out[2] = g_half_srgb[in[2]];
            out[3] = 255; /* the view is opaque; the scene's alpha is not coverage */
        }
    }
    overlay(v, v->upload_map, dpitch, w, h);
    const u32 s = (u32)(v->frames % ED_RING);
    ID3D12CommandAllocator_Reset(b->alloc);
    ID3D12GraphicsCommandList_Reset(b->list, b->alloc, NULL);
    D3D12_TEXTURE_COPY_LOCATION dst, from;
    memset(&dst, 0, sizeof dst);
    memset(&from, 0, sizeof from);
    dst.pResource = r->tex[s];
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = v->upload;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    from.PlacedFootprint.Footprint.Width = w;
    from.PlacedFootprint.Footprint.Height = h;
    from.PlacedFootprint.Footprint.Depth = 1;
    from.PlacedFootprint.Footprint.RowPitch = dpitch;
    ID3D12GraphicsCommandList_CopyTextureRegion(b->list, &dst, 0, 0, 0, &from, NULL);
    ID3D12GraphicsCommandList_Close(b->list);
    ID3D12CommandList *lists[1] = {(ID3D12CommandList *)b->list};
    ID3D12CommandQueue_ExecuteCommandLists(b->queue, 1, lists);
    ID3D12CommandQueue_Signal(b->queue, v->fence, ++v->fence_value);
    r->last_fence = v->fence_value;
    bridge_wait(); /* the allocator and upload buffer are reused next frame */
    *surface = s;
    *fence = v->fence_value;
    return true;
}
#endif /* ED_WIN_VIEWS */
#if ED_MAC_VIEWS
static bool present(ed_view *v, const u8 *src, u32 pitch, u32 w, u32 h, u32 *surface, u64 *fence) {
    ed_ring *r = &v->rings[v->ring_count - 1];
    if (r->w != w || r->h != h) return false;
    half_tables();
    const u32 slot = (u32)(v->frames % ED_RING);
    u64 *sequence = (u64 *)r->map[slot];
    const u64 serial = (++v->fence_value) * 2u;
    __atomic_store_n(sequence, serial - 1u, __ATOMIC_RELEASE);
    u8 *pixels = (u8 *)r->map[slot] + 64;
    for (u32 y = 0; y < h; ++y) {
        const u16 *in = (const u16 *)(src + (size_t)y * pitch);
        u8 *out = pixels + (size_t)y * w * 4u;
        for (u32 x = 0; x < w; ++x, in += 4, out += 4) {
            out[0] = g_half_srgb[in[0]]; out[1] = g_half_srgb[in[1]];
            out[2] = g_half_srgb[in[2]]; out[3] = 255;
        }
    }
    overlay(v, pixels, w * 4u, w, h);
    __atomic_store_n(sequence, serial, __ATOMIC_RELEASE);
    r->last_fence = serial;
    *surface = slot; *fence = serial;
    return true;
}
#endif

#endif /* ED_VIEWS */

/* ---------- Engine side: renderer views, cameras, readback ---------- */

static void readback_free(de__editor *ed, ed_view *v) {
    for (u32 i = 0; i < ED_RB_SLOTS; ++i)
        if (DE_HANDLE_VALID(v->rb[i]))
            de_gpu_buffer_destroy(ed->e->gpu, v->rb[i]);
    memset(v->rb, 0, sizeof v->rb);
    memset((void *)v->rb_map, 0, sizeof v->rb_map);
}

#if ED_VIEWS
static bool readback_alloc(de__editor *ed, ed_view *v, u32 w, u32 h) {
    if (!w || !h || w > 8192 || h > 8192) return false;
    readback_free(ed, v);
    v->rb_w = w;
    v->rb_h = h;
    v->rb_pitch = (w * 8u + 255u) & ~255u;
    for (u32 i = 0; i < ED_RB_SLOTS; ++i) {
        v->rb[i] = de_gpu_buffer_create(ed->e->gpu, &(de_gpu_buffer_desc){.size = (u64)v->rb_pitch * h,
                                                                           .usage = DE_USAGE_COPY_DST,
                                                                           .memory = DE_MEM_READBACK});
        if (!DE_HANDLE_VALID(v->rb[i]))
            return false;
        v->rb_map[i] = (const u8 *)de_gpu_buffer_map(ed->e->gpu, v->rb[i]);
    }
    return true;
}

typedef struct rb_exec {
    de_rg_resource color, buf;
    u32 pitch;
} rb_exec;

static rb_exec g_exec[ED_MAX_VIEWS][ED_RB_SLOTS];

static void exec_copy(de_gpu_cmd *cmd, const de_rg_pass *pass, void *user) {
    const rb_exec *x = (const rb_exec *)user;
    de_gpu_cmd_copy_texture_to_buffer(cmd, de_rg_buffer(pass, x->buf), 0, x->pitch,
                                      de_rg_texture(pass, x->color), 0, 0);
}

/* Multiple Game panels share the main camera. Render at the fastest visible panel's cap;
 * each panel's readback is still capped separately. No panels restores the runtime defaults. */
static void apply_game_rate(de__editor *ed) {
    bool any = false, uncapped = false;
    u32 fps = 0;
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i) {
        ed_view *v = &g_views.views[i];
        if (!v->id || !v->game || v->paused)
            continue;
        any = true;
        uncapped |= v->max_fps == 0;
        if (v->max_fps > fps)
            fps = v->max_fps;
    }
    bool has_game = false;
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i)
        has_game |= g_views.views[i].id && g_views.views[i].game;
    const de_view main = de_renderer_main_view(ed->e->renderer);
    de_view_set_max_fps(ed->e->renderer, main, any && !uncapped ? fps : 0);
    de_view_set_paused(ed->e->renderer, main, has_game && !any);
}

/* Render thread: at the UI point of each view's graph, copy the views we serve. */
static void hook_ui(de_rg *g, de_view rv, void *user) {
    (void)user;
    de__editor *ed = g_ed;
    if (ed == NULL)
        return;
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i) {
        ed_view *v = &g_views.views[i];
        if (v->id == 0 || v->paused || v->rv.id != rv.id || !DE_HANDLE_VALID(v->rb[0]))
            continue;
        u32 w = 0, h = 0;
        de_view_size(ed->e->renderer, rv, &w, &h);
        if (w != v->rb_w || h != v->rb_h)
            continue; /* the main thread resizes the readback first */
        const u64 now = de_time_now_ns();
        if (v->game && v->max_fps && now < v->next_readback_ns)
            continue;
        if (v->game && v->max_fps)
            v->next_readback_ns = now + (1000000000ull + v->max_fps - 1u) / v->max_fps;
        const u64 seq = de_atomic_load_u64(&v->produced, DE_ATOMIC_ACQUIRE);
        const u32 slot = (u32)(seq % ED_RB_SLOTS);
        rb_exec *x = &g_exec[i][slot];
        x->color = de_view_color(ed->e->renderer, rv);
        x->buf = de_rg_import_buffer(g, v->rb[slot], DE_ACCESS_NONE);
        x->pitch = v->rb_pitch;
        de_rg_pass *p = de_rg_add_pass(g, "editor.readback", DE_QUEUE_GRAPHICS, exec_copy, x);
        de_rg_read(p, x->color, DE_ACCESS_COPY_SRC);
        de_rg_write(p, x->buf, DE_ACCESS_COPY_DST);
        de_rg_pass_side_effect(p);
        de_atomic_store_u64(&v->produced, seq + 1, DE_ATOMIC_RELEASE);
    }
}

#endif /* ED_VIEWS */

static de_component camera_component(de__editor *ed) {
    if (g_views.c_camera.id == 0)
        g_views.c_camera = de_component_find(ed->e->world, "DeCamera");
    return g_views.c_camera;
}

/* The editor camera entity follows the IDE's camera. */
static void camera_apply(de__editor *ed, ed_view *v) {
    if (v->game || !de_entity_alive(ed->e->world, v->cam))
        return;
    de_world *w = ed->e->world;
    const de_builtin_component_ids b = de_builtin_components(w);
    DeTransform *t = (DeTransform *)de_entity_get(w, v->cam, b.transform);
    DeCamera *c = (DeCamera *)de_entity_get(w, v->cam, camera_component(ed));
    if (t == NULL || c == NULL)
        return;
    t->position = (de_dvec3){v->cam_pos[0], v->cam_pos[1], v->cam_pos[2]};
    /* The protocol's camera looks along its local +Y with +Z up (the world's axes); a DeCamera
     * looks down its local -Z with +Y up (view space). +90 degrees about X turns the one into the
     * other, applied first. */
    const de_quat ide = {v->cam_rot[0], v->cam_rot[1], v->cam_rot[2], v->cam_rot[3]};
    t->rotation = de_quat_mul(ide, de_quat_axis_angle((de_vec3){1, 0, 0}, DE_PI * 0.5f));
    t->scale = (de_vec3){1, 1, 1};
    c->view = v->rv;
    if (v->camera.has_ortho_height || v->camera.fov_y <= 0) {
        c->projection = DE_PROJ_ORTHOGRAPHIC;
        c->ortho_height = v->camera.has_ortho_height ? v->camera.ortho_height : 10.0f;
    } else {
        c->projection = DE_PROJ_PERSPECTIVE;
        c->fov_y = v->camera.fov_y;
    }
    c->near_z = v->camera.near > 0 ? v->camera.near : 0.1f;
    c->exposure_ev100 = 0;
}

static void camera_store(ed_view *v, const de_ep_camera *c) {
    v->camera = *c;
    v->cam_dirty = true;
    for (u32 i = 0; i < 3; ++i)
        v->cam_pos[i] = i < c->position.count ? c->position.items[i] : 0.0;
    const f32 id[4] = {0, 0, 0, 1};
    for (u32 i = 0; i < 4; ++i)
        v->cam_rot[i] = c->rotation.count == 4 ? c->rotation.items[i] : id[i];
    /* The lists point into the request; keep only the scalars. */
    v->camera.position.items = NULL;
    v->camera.position.count = 0;
    v->camera.rotation.items = NULL;
    v->camera.rotation.count = 0;
}

static void view_release_all(de__editor *ed, ed_view *v) {
    if (ed->e->renderer != NULL)
        de__render_world_wait_idle(ed->e->renderer);
    readback_free(ed, v);
    if (!v->game) {
        if (DE_HANDLE_VALID(v->rv) && ed->e->renderer != NULL)
            de_view_destroy(ed->e->renderer, v->rv);
        if (DE_HANDLE_VALID(v->target))
            de_gpu_texture_destroy(ed->e->gpu, v->target);
        if (de_entity_alive(ed->e->world, v->cam))
            de_entity_despawn(ed->e->world, v->cam);
    }
#if ED_VIEWS
    view_gpu_free(v);
#endif
    const bool was_game = v->game;
    memset(v, 0, sizeof *v);
#if ED_VIEWS
    if (was_game)
        apply_game_rate(ed);
#else
    (void)was_game;
#endif
}

#if ED_VIEWS
static void write_surfaces(ed_view *v, de_ep_arena *a, u32 *generation, de_ep_list_surface *out) {
    memset(out, 0, sizeof *out);
    const ed_ring *r = &v->rings[v->ring_count - 1];
    *generation = r->generation;
    out->items = (de_ep_surface *)de__ed_alloc(a, sizeof(de_ep_surface) * ED_RING);
    for (u32 i = 0; i < ED_RING && out->items != NULL; ++i) {
        de_ep_surface *surface = &out->items[out->count++];
        memset(surface, 0, sizeof *surface);
        surface->index = i;
#if ED_WIN_VIEWS
        char *name = (char *)de__ed_alloc(a, 128);
        if (name == NULL) { --out->count; break; }
        snprintf(name, 128, "%s-v%u-g%u-s%u", g_views.bridge.prefix, v->id, r->generation, i);
        surface->handle = de_ep_cstr(name);
        surface->has_fence = true;
        surface->fence = de_ep_cstr(v->fence_name);
#else
        surface->handle = de_ep_cstr(r->path[i]);
#endif
        surface->format = de_ep_cstr("rgba8_srgb");
    }
}
#endif /* ED_VIEWS */

/* ---------- view.* ---------- */

static const char *const k_render_modes[DE_VIEW_RENDER_MODE_COUNT] = {
    "lit",      "unlit", "baseColor", "normals",       "roughness",
    "metallic", "ao",    "emissive",  "motionVectors", "depth"};
u32 de__ed_views_render_modes(const char *const **names) {
    *names = k_render_modes;
    return ED_VIEWS ? DE_VIEW_RENDER_MODE_COUNT : 0;
}
static bool render_mode(de_ep_str name, de_view_render_mode *out) {
    if (!name.len) {
        *out = DE_VIEW_RENDER_LIT;
        return true;
    }
    for (u32 i = 0; i < DE_VIEW_RENDER_MODE_COUNT; ++i)
        if (de_ep_str_eq(name, k_render_modes[i])) {
            *out = (de_view_render_mode)i;
            return true;
        }
    return false;
}

#if ED_VIEWS
static de_view_display display_policy(de_view_render_mode mode, u32 show) {
    return (de_view_display){.mode = mode,
                             .disabled_features =
                                 ((show & DE__ED_SHOW_UI) ? 0u : DE_VIEW_HIDE_UI) |
                                 ((show & DE__ED_SHOW_POST) ? 0u : DE_VIEW_DISABLE_POST_EFFECTS)};
}

#endif

static void on_create(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(view_create, de_ep_view_create_params, q);
#if ED_VIEWS
    if (ed->e->renderer == NULL || ed->e->gpu == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "this session has no renderer");
        return;
    }
    de_view_render_mode mode;
    if (!render_mode(q.render_mode, &mode) ||
        (q.has_game && q.game && mode != DE_VIEW_RENDER_LIT)) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "render mode is unavailable for this view");
        return;
    }
    char *adapter = q.has_adapter ? de__ed_cstr(a, q.adapter) : NULL;
    DE_INFO(ed->log, "view.create: %ux%u on adapter %s%s", q.width, q.height,
            adapter ? adapter : "(any)", q.has_game && q.game ? " (game view)" : "");
    if (!bridge_init(adapter)) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no D3D12 device for shared surfaces");
        return;
    }
#if ED_WIN_VIEWS
    const bool adapter_mismatch = adapter != NULL && _stricmp(adapter, g_views.bridge.adapter) != 0;
#else
    const bool adapter_mismatch = adapter != NULL && strcmp(adapter, g_views.bridge.adapter) != 0;
#endif
    if (adapter_mismatch) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "shared surfaces are on adapter %s, not %s",
                     g_views.bridge.adapter, adapter);
        return;
    }
    ed_view *v = NULL;
    for (u32 i = 0; i < ED_MAX_VIEWS && v == NULL; ++i)
        if (g_views.views[i].id == 0)
            v = &g_views.views[i];
    if (v == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many views");
        return;
    }
    memset(v, 0, sizeof *v);
    v->id = ++g_views.next_id;
    v->game = q.has_game && q.game;
    v->w = q.width ? q.width : 1;
    v->h = q.height ? q.height : 1;
    v->scale = q.scale;
    v->show = de__ed_show_flags(&q.show_flags);
    snprintf(v->render_mode, sizeof v->render_mode, "%.*s", (int)q.render_mode.len, q.render_mode.ptr);
    camera_store(v, &q.camera);
    DE_INFO(ed->log, "view.create: bridge on %s; waiting for the renderer to go idle",
            g_views.bridge.adapter);
    de__render_world_wait_idle(ed->e->renderer);
    DE_INFO(ed->log, "view.create: renderer idle");
    if (v->game) {
        v->rv = de_renderer_main_view(ed->e->renderer);
        u32 ow = 0, oh = 0;
        de_renderer_output_size(ed->e->renderer, &ow, &oh);
        v->sw = ow;
        v->sh = oh;
    } else {
        v->sw = v->w;
        v->sh = v->h;
        v->target = de_gpu_texture_create(
            ed->e->gpu, &(de_gpu_texture_desc){.dim = DE_TEX_2D,
                                               .format = DE_FMT_RGBA8_UNORM,
                                               .width = v->w,
                                               .height = v->h,
                                               .depth_or_layers = 1,
                                               .mip_count = 1,
                                               .sample_count = 1,
                                               .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                               .name = "editor view"});
        char name[32];
        snprintf(name, sizeof name, "editor%u", v->id);
        v->rv = DE_HANDLE_VALID(v->target)
                    ? de_view_create(ed->e->renderer, &(de_view_desc){.target = v->target, .order = -10 - (i32)v->id, .name = name})
                    : (de_view){0};
        if (DE_HANDLE_VALID(v->rv)) {
            de_view_set_render_scale(ed->e->renderer, v->rv, 1.0f);
            const de_view_display display = display_policy(mode, v->show);
            (void)de_view_set_display(ed->e->renderer, v->rv, &display);
            (void)de__view_set_hidden_policy(ed->e->renderer, v->rv, g_views.hidden);
            const de_builtin_component_ids b = de_builtin_components(ed->e->world);
            v->cam = de_entity_spawn(ed->e->world);
            (void)de_entity_add(ed->e->world, v->cam, b.transform);
            (void)de_entity_add(ed->e->world, v->cam, camera_component(ed));
            camera_apply(ed, v);
        }
    }
    if (!DE_HANDLE_VALID(v->rv) || !readback_alloc(ed, v, v->sw, v->sh) || !view_gpu_create(v)) {
        DE_WARN(ed->log, "view.create failed: view %d, %s", (int)DE_HANDLE_VALID(v->rv),
                de_error_last()->message);
        de_error_clear();
        view_release_all(ed, v);
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not create the view");
        return;
    }
    v->max_fps = 60;
    if (v->game)
        apply_game_rate(ed);
    else
        de_view_set_max_fps(ed->e->renderer, v->rv, v->max_fps);
    de_atomic_store_u64(&v->produced, 0, DE_ATOMIC_RELEASE);
    if (!g_views.hook) {
        de_render_add_hook(ed->e->renderer, DE_HOOK_UI, hook_ui, NULL);
        g_views.hook = true;
    }
    DE_INFO(ed->log, "view.create: view %u ready (%ux%u)", v->id, v->sw, v->sh);
    de_ep_view_created r;
    memset(&r, 0, sizeof r);
    r.view = v->id;
    write_surfaces(v, a, &r.generation, &r.surfaces);
    de_ep_write_response_view_create(w, req->id, &r);
#else
    (void)ed;
    (void)q;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "shared surfaces need Windows (D3D12)");
#endif
}

static ed_view *view_or_fail(const de_ep_request *req, mpack_writer_t *w, u32 id) {
    ed_view *v = view_find(id);
    if (v == NULL)
        de__ed_error(w, req->id, DE_EP_ERR_NOT_FOUND, "view %u", id);
    return v;
}

static void empty_response(mpack_writer_t *w, u64 id) {
    de_ep_write_result_begin(w, id);
    mpack_build_map(w);
    mpack_complete_map(w);
    de_ep_write_result_end(w);
}

static void on_resize(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(view_resize, de_ep_view_resize_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
#if ED_VIEWS
    v->w = q.width ? q.width : 1;
    v->h = q.height ? q.height : 1;
    v->scale = q.scale;
    if (!v->game) {
        de__render_world_wait_idle(ed->e->renderer);
        de_view_destroy(ed->e->renderer, v->rv);
        de_gpu_texture_destroy(ed->e->gpu, v->target);
        v->target = de_gpu_texture_create(
            ed->e->gpu, &(de_gpu_texture_desc){.dim = DE_TEX_2D, .format = DE_FMT_RGBA8_UNORM,
                                               .width = v->w, .height = v->h, .depth_or_layers = 1,
                                               .mip_count = 1, .sample_count = 1,
                                               .usage = DE_USAGE_RENDER_TARGET | DE_USAGE_SAMPLED,
                                               .name = "editor view"});
        char name[32];
        snprintf(name, sizeof name, "editor%u", v->id);
        v->rv = de_view_create(ed->e->renderer, &(de_view_desc){.target = v->target, .order = -10 - (i32)v->id, .name = name});
        de_view_set_render_scale(ed->e->renderer, v->rv, 1.0f);
        v->sw = v->w;
        v->sh = v->h;
        camera_apply(ed, v);
    }
    de__render_world_wait_idle(ed->e->renderer);
    if (!readback_alloc(ed, v, v->sw, v->sh) || !ring_create(v, v->sw, v->sh)) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "resize failed");
        return;
    }
    if (!v->game) {
        de_view_set_max_fps(ed->e->renderer, v->rv, v->max_fps);
        de_view_set_paused(ed->e->renderer, v->rv, v->paused);
        de_view_render_mode mode = DE_VIEW_RENDER_LIT;
        (void)render_mode(de_ep_cstr(v->render_mode), &mode);
        const de_view_display display = display_policy(mode, v->show);
        (void)de_view_set_display(ed->e->renderer, v->rv, &display);
        (void)de__view_set_hidden_policy(ed->e->renderer, v->rv, g_views.hidden);
    }
    v->consumed = de_atomic_load_u64(&v->produced, DE_ATOMIC_ACQUIRE);
    de_ep_view_resized r;
    memset(&r, 0, sizeof r);
    write_surfaces(v, a, &r.generation, &r.surfaces);
    de_ep_write_response_view_resize(w, req->id, &r);
#else
    (void)ed;
    (void)a;
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "no shared surfaces");
#endif
}

static void on_release(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(view_release, de_ep_view_release_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
#if ED_VIEWS
    for (u32 i = 0; i + 1 < v->ring_count; ++i)
        if (v->rings[i].generation == q.generation)
            v->rings[i].released = true;
    /* Free released rings whose last frame completed. */
#if ED_WIN_VIEWS
    const u64 done = v->fence != NULL ? ID3D12Fence_GetCompletedValue(v->fence) : 0;
#else
    const u64 done = v->fence_value * 2u;
#endif
    u32 k = 0;
    for (u32 i = 0; i < v->ring_count; ++i) {
        ed_ring *r = &v->rings[i];
        if (i + 1 < v->ring_count && r->released && done >= r->last_fence) {
            ring_free(r);
            continue;
        }
        if (k != i)
            v->rings[k] = *r;
        ++k;
    }
    v->ring_count = k;
#endif
    empty_response(w, req->id);
}

static void on_set_camera(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(view_set_camera, de_ep_view_camera_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    if (!v->game) {
        camera_store(v, &q.camera);
        camera_apply(ed, v);
    }
    empty_response(w, req->id);
}

static void on_render_mode(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(view_set_render_mode, de_ep_view_render_mode_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    de_view_render_mode mode;
    if (!render_mode(q.render_mode, &mode) || (v->game && mode != DE_VIEW_RENDER_LIT)) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "render mode is unavailable for this view");
        return;
    }
#if ED_VIEWS
    const de_view_display display = display_policy(mode, v->show);
    if (!v->game && de_view_set_display(ed->e->renderer, v->rv, &display) != DE_OK) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not apply render mode");
        return;
    }
#endif
    snprintf(v->render_mode, sizeof v->render_mode, "%.*s", (int)q.render_mode.len, q.render_mode.ptr);
    empty_response(w, req->id);
}

static void on_show_flags(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(view_set_show_flags, de_ep_view_show_flags_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    const u32 show = de__ed_show_flags(&q.show_flags);
#if ED_VIEWS
    de_view_render_mode mode = DE_VIEW_RENDER_LIT;
    (void)render_mode(de_ep_cstr(v->render_mode), &mode);
    const de_view_display display = display_policy(mode, show);
    if (v->game && display.disabled_features) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED,
                     "Game views share the runtime output; display overrides are unavailable");
        return;
    }
    if (!v->game && de_view_set_display(ed->e->renderer, v->rv, &display) != DE_OK) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not apply show flags");
        return;
    }
#endif
    v->show = show;
    empty_response(w, req->id);
}

static void on_max_fps(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                       mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(view_set_max_fps, de_ep_view_max_fps_params, q);
    if (!ED_VIEWS || ed->e->renderer == NULL || ed->e->gpu == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "views.frameRateLimit is unavailable");
        return;
    }
    if (q.fps > 240) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "fps must be between 0 and 240");
        return;
    }
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
#if ED_VIEWS
    de__render_world_wait_idle(ed->e->renderer);
    v->max_fps = q.fps;
    v->next_readback_ns = 0;
    if (v->game)
        apply_game_rate(ed);
    else
        de_view_set_max_fps(ed->e->renderer, v->rv, q.fps);
#endif
    empty_response(w, req->id);
}

static void on_paused(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(view_set_paused, de_ep_view_paused_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
#if ED_VIEWS
    de__render_world_wait_idle(ed->e->renderer);
    v->paused = q.paused;
    v->next_readback_ns = 0;
    if (v->game)
        apply_game_rate(ed);
    else
        de_view_set_paused(ed->e->renderer, v->rv, q.paused);
#else
    v->paused = q.paused;
#endif
    empty_response(w, req->id);
}

static void on_destroy(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(view_destroy, de_ep_view_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    view_release_all(ed, v);
    empty_response(w, req->id);
}

static void on_set_hidden(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(view_set_hidden, de_ep_entity_list, q);
#if ED_VIEWS
    if (!ed->e->renderer) {
        de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "hidden rendering requires a renderer");
        return;
    }
    const de__view_hidden *policy = NULL;
    if (de__view_hidden_create(ed->e->renderer->alloc, q.entities.items, q.entities.count,
                               &policy) != DE_OK) {
        de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "could not copy hidden entities");
        return;
    }
    /* Validate before changing any view, then share one immutable list across Scene views.
     * Game panels keep the canonical runtime image. New/resized Scene views inherit it too. */
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i) {
        const ed_view *v = &g_views.views[i];
        if (v->id && !v->game && !de__view_valid(ed->e->renderer, v->rv)) {
            de__view_hidden_release(policy);
            de__ed_error(w, req->id, DE_EP_ERR_INTERNAL, "stale renderer view");
            return;
        }
    }
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i) {
        const ed_view *v = &g_views.views[i];
        if (v->id && !v->game)
            (void)de__view_set_hidden_policy(ed->e->renderer, v->rv, policy);
    }
    de__view_hidden_release(g_views.hidden);
    g_views.hidden = policy;
    empty_response(w, req->id);
#else
    de__ed_error(w, req->id, DE_EP_ERR_UNSUPPORTED, "hidden rendering requires shared views");
#endif
}

/* ---------- Picking ---------- */

static bool hidden(u64 e) {
#if ED_VIEWS
    return de__view_hidden_contains(g_views.hidden, e);
#else
    (void)e;
    return false;
#endif
}

/* The source pixel of an IDE position. */
static void to_source(const ed_view *v, f32 x, f32 y, u32 *px, u32 *py) {
    const f32 fx = v->w ? x * (f32)v->sw / (f32)v->w : x, fy = v->h ? y * (f32)v->sh / (f32)v->h : y;
    *px = fx < 0 ? 0u : fx >= (f32)v->sw ? v->sw - 1 : (u32)fx;
    *py = fy < 0 ? 0u : fy >= (f32)v->sh ? v->sh - 1 : (u32)fy;
}

static bool pick_skipped(const ed_pick *p, u64 e) {
    if (hidden(e) || de__ed_views_owns(e))
        return true;
    for (u32 i = 0; i < p->skip_count; ++i)
        if (p->skip[i] == e)
            return true;
    return false;
}

/* Writes a pick response: `entity` (0: none) and the world point under (x, y), where an asset
 * dropped there lands. */
static void pick_respond(de__editor *ed, mpack_writer_t *w, u64 req_id, const ed_view *v, f32 x, f32 y,
                         u64 entity, u32 tri, bool has_tri) {
    de_ep_pick_result r;
    memset(&r, 0, sizeof r);
    if (entity != 0) {
        r.has_entity = true;
        r.entity = entity;
        r.has_triangle = has_tri;
        r.triangle = tri;
    }
    f64 pos[3];
    f32 nrm[3];
    if (v != NULL) {
        de__ed_cam c;
        cam_of(v, &c);
        if (de__ed_view_point(ed, &c, x, y, entity, pos, nrm)) {
            r.has_position = true;
            r.position.items = pos;
            r.position.count = 3;
            r.has_normal = true;
            r.normal.items = nrm;
            r.normal.count = 3;
        }
    }
    de_ep_write_response_pick(w, req_id, &r);
}

static void on_pick(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(pick, de_ep_pick_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    ed_pick *p = NULL;
    for (u32 i = 0; i < ED_MAX_PICKS && p == NULL; ++i)
        if (!g_views.picks[i].pending)
            p = &g_views.picks[i];
    if (p == NULL || ed->e->renderer == NULL) {
        de__ed_error(w, req->id, DE_EP_ERR_BUSY, "too many picks in flight");
        return;
    }
    u32 px, py;
    to_source(v, q.x, q.y, &px, &py);
    p->skip_count = 0;
    for (u32 i = 0; q.has_skip && i < q.skip.count && p->skip_count < ED_PICK_SKIP; ++i)
        p->skip[p->skip_count++] = q.skip.items[i];
    p->sprite = 0;
    p->x = (f32)px + 0.5f;
    p->y = (f32)py + 0.5f;
    if (!v->game) {
        de__ed_cam c;
        cam_of(v, &c);
        /* A camera's or a light's icon first: they draw nothing the GPU pick can hit. */
        u64 not_icons[ED_MAX_VIEWS + ED_PICK_SKIP];
        u32 ns = editor_cameras(not_icons);
        for (u32 i = 0; i < p->skip_count; ++i)
            not_icons[ns++] = p->skip[i];
        const u64 icon = de__ed_icon_pick(ed, &c, p->x, p->y, v->show, not_icons, ns);
        if (icon != 0 && !hidden(icon)) {
            pick_respond(ed, w, req->id, v, p->x, p->y, icon, 0, false);
            return;
        }
        /* The sprites under the cursor, by their rectangles: translucent ones too. */
        u64 sprites[64];
        const u32 n = de__ed_sprite_pick(ed, &c, p->x, p->y, sprites, 64);
        for (u32 i = 0; i < n && p->sprite == 0; ++i)
            if (!pick_skipped(p, sprites[i]))
                p->sprite = sprites[i];
        /* A 2D view is all sprites: the nearest one is the answer. */
        if (de__ed_cam_flat(&c) && p->sprite != 0) {
            pick_respond(ed, w, req->id, v, p->x, p->y, p->sprite, 0, false);
            return;
        }
    }
    p->req_id = req->id;
    p->render_id = ++g_views.next_pick;
    p->view = v->id;
    p->pending = true;
    p->frames = 0;
    de_view_request_pick(ed->e->renderer, v->rv, px, py, p->render_id);
    de__ed_defer(ed);
}

static void picks_tick(de__editor *ed) {
    for (u32 i = 0; i < ED_MAX_PICKS; ++i) {
        ed_pick *p = &g_views.picks[i];
        if (!p->pending)
            continue;
        u64 bits = 0;
        u32 tri = 0;
        const bool done = de_view_pick_result(ed->e->renderer, p->render_id, &bits, &tri);
        if (!done && ++p->frames < 30)
            continue;
        /* The GPU's hit, unless it is one to see through; else the sprite under the cursor. */
        const bool gpu = done && bits != 0 && !pick_skipped(p, bits);
        mpack_writer_t w;
        de__ed_respond_begin(ed, &w);
        pick_respond(ed, &w, p->req_id, view_find(p->view), p->x, p->y, gpu ? bits : p->sprite, tri, gpu);
        de__ed_send(ed, &w, DE_EP_KIND_RESPONSE);
        p->pending = false;
    }
}

static void on_pick_rect(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(pick_rect, de_ep_pick_rect_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    if (q.rect.count != 4) {
        de__ed_error(w, req->id, DE_EP_ERR_INVALID_PARAMS, "rect is [x, y, width, height]");
        return;
    }
    /* A grid of picks over the rectangle, resolved with one GPU round trip. */
    enum { N = 8 };
    u64 ids[N * N];
    for (u32 j = 0; j < N; ++j)
        for (u32 i = 0; i < N; ++i) {
            u32 px, py;
            to_source(v, q.rect.items[0] + q.rect.items[2] * ((f32)i + 0.5f) / N,
                      q.rect.items[1] + q.rect.items[3] * ((f32)j + 0.5f) / N, &px, &py);
            ids[j * N + i] = ++g_views.next_pick;
            de_view_request_pick(ed->e->renderer, v->rv, px, py, ids[j * N + i]);
        }
    /* Render frames until the picks are in (bounded). */
    enum { ORIGINS = 4096 };
    de_ep_entity_list out;
    memset(&out, 0, sizeof out);
    out.entities.items = (u64 *)de__ed_alloc(a, sizeof(u64) * (N * N + ORIGINS));
    for (u32 frame = 0; frame < 6 && out.entities.items != NULL; ++frame) {
        (void)de__render_extract_and_kick(ed->e->renderer, ed->e->world);
        de__render_world_wait_idle(ed->e->renderer);
    }
    for (u32 k = 0; k < N * N && out.entities.items != NULL; ++k) {
        u64 bits = 0;
        u32 tri = 0;
        if (!de_view_pick_result(ed->e->renderer, ids[k], &bits, &tri) || bits == 0 || hidden(bits))
            continue;
        bool seen = false;
        for (u32 x = 0; x < out.entities.count && !seen; ++x)
            seen = out.entities.items[x] == bits;
        if (!seen)
            out.entities.items[out.entities.count++] = bits;
    }
    /* And every entity whose origin is in the box (the editor's cameras and hidden ones aside). */
    if (out.entities.items != NULL && !v->game) {
        de__ed_cam c;
        cam_of(v, &c);
        u64 skip[ED_MAX_VIEWS];
        const u32 ns = editor_cameras(skip);
        u32 x0, y0, x1, y1;
        to_source(v, q.rect.items[0], q.rect.items[1], &x0, &y0);
        to_source(v, q.rect.items[0] + q.rect.items[2], q.rect.items[1] + q.rect.items[3], &x1, &y1);
        u64 *found = out.entities.items + out.entities.count;
        const u32 n = de__ed_origin_pick(ed, &c, (f32)x0, (f32)y0, (f32)x1 + 1.0f, (f32)y1 + 1.0f, skip, ns,
                                         found, ORIGINS);
        const u32 had = out.entities.count;
        for (u32 k = 0; k < n; ++k) {
            const u64 e = found[k];
            bool seen = hidden(e);
            for (u32 x = 0; x < had && !seen; ++x)
                seen = out.entities.items[x] == e;
            if (!seen)
                out.entities.items[out.entities.count++] = e;
        }
    }
    de_ep_write_response_pick_rect(w, req->id, &out);
}

/* ---------- Input ---------- */

static u16 hid_of(de_ep_str code) {
    char c[32];
    const u32 n = code.len < 31 ? code.len : 31;
    memcpy(c, code.ptr, n);
    c[n] = '\0';
    if (strncmp(c, "Key", 3) == 0 && c[3] >= 'A' && c[3] <= 'Z' && c[4] == '\0')
        return (u16)(4 + (c[3] - 'A'));
    if (strncmp(c, "Digit", 5) == 0 && c[5] >= '0' && c[5] <= '9')
        return (u16)(c[5] == '0' ? 39 : 30 + (c[5] - '1'));
    if (c[0] == 'F' && c[1] >= '1' && c[1] <= '9') {
        const int f = atoi(c + 1);
        if (f >= 1 && f <= 12)
            return (u16)(58 + f - 1);
    }
    static const struct {
        const char *name;
        u16 hid;
    } k[] = {{"Enter", 40},        {"Escape", 41},     {"Backspace", 42},   {"Tab", 43},
             {"Space", 44},        {"Minus", 45},      {"Equal", 46},       {"BracketLeft", 47},
             {"BracketRight", 48}, {"Backslash", 49},  {"Semicolon", 51},   {"Quote", 52},
             {"Backquote", 53},    {"Comma", 54},      {"Period", 55},      {"Slash", 56},
             {"ArrowRight", 79},   {"ArrowLeft", 80},  {"ArrowDown", 81},   {"ArrowUp", 82},
             {"Delete", 76},       {"Home", 74},       {"End", 77},         {"PageUp", 75},
             {"PageDown", 78},     {"ControlLeft", 224}, {"ShiftLeft", 225}, {"AltLeft", 226},
             {"MetaLeft", 227},    {"ControlRight", 228}, {"ShiftRight", 229}, {"AltRight", 230}};
    for (u32 i = 0; i < sizeof k / sizeof k[0]; ++i)
        if (strcmp(c, k[i].name) == 0)
            return k[i].hid;
    return 0;
}

static void on_input(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(input_events, de_ep_input_events_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    de_input *in = ed->e->input;
    const bool to_game = v->game && in != NULL;
    for (u32 i = 0; i < q.events.count; ++i) {
        const de_ep_input_event *e = &q.events.items[i];
        if (!v->game) {
            /* IDE pixels to source pixels. */
            de__ed_cam c;
            cam_of(v, &c);
            const f32 sx = v->w ? (f32)v->sw / (f32)v->w : 1.0f, sy = v->h ? (f32)v->sh / (f32)v->h : 1.0f;
            de__ed_gizmo_input(ed, v->id, &c, e, e->has_x ? e->x * sx : 0, e->has_y ? e->y * sy : 0);
            continue;
        }
        if (!to_game)
            continue;
        de__input_event ev;
        memset(&ev, 0, sizeof ev);
        ev.window = ed->e->window;
        ev.time_ns = de_time_now_ns();
        /* IDE pixels to the window's logical points. */
        u32 ww = 0, wh = 0;
        de_window_size_points(ed->e->window, &ww, &wh);
        const f32 sx = v->w ? (f32)ww / (f32)v->w : 1.0f, sy = v->h ? (f32)wh / (f32)v->h : 1.0f;
        const f32 x = e->has_x ? e->x * sx : 0, y = e->has_y ? e->y * sy : 0;
        if (de_ep_str_eq(e->type, "pointer")) {
            if (e->has_action && de_ep_str_eq(e->action, "move")) {
                ev.kind = DE_RAW_MOUSE_MOVE;
                ev.mouse_move = (de_raw_mouse_move){x, y, e->has_dx ? e->dx * sx : 0, e->has_dy ? e->dy * sy : 0};
            } else if (e->has_action && (de_ep_str_eq(e->action, "down") || de_ep_str_eq(e->action, "up"))) {
                ev.kind = DE_RAW_MOUSE_BUTTON;
                ev.mouse_button.x = x;
                ev.mouse_button.y = y;
                ev.mouse_button.button = (u8)(e->has_button ? e->button : 0);
                ev.mouse_button.down = de_ep_str_eq(e->action, "down");
                ev.mouse_button.clicks = 1;
            } else {
                continue;
            }
        } else if (de_ep_str_eq(e->type, "wheel")) {
            ev.kind = DE_RAW_MOUSE_WHEEL;
            ev.mouse_wheel = (de_raw_mouse_wheel){e->has_dx ? e->dx : 0, e->has_dy ? -e->dy : 0, x, y};
        } else if (de_ep_str_eq(e->type, "key") && e->has_code) {
            const u16 hid = hid_of(e->code);
            if (hid == 0)
                continue;
            ev.kind = DE_RAW_KEY;
            ev.key.code = hid;
            ev.key.down = !(e->has_action && de_ep_str_eq(e->action, "up"));
        } else if (de_ep_str_eq(e->type, "text") && e->has_text) {
            ev.kind = DE_RAW_TEXT;
            const u32 n = e->text.len < 31 ? e->text.len : 31;
            memcpy(ev.text.utf8, e->text.ptr, n);
        } else {
            continue;
        }
        de__input_inject(in, &ev);
    }
    empty_response(w, req->id);
}

static void send_capture(de__editor *ed, ed_view *v) {
    de_ep_capture_changed d = {v->id, v->captured};
    mpack_writer_t nw;
    const u64 seq = de__ed_note_begin(ed, &nw);
    de_ep_write_notify_input_capture_changed(&nw, seq, &d);
    de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
}

static void on_focus(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    DE__ED_PARAMS(input_focus, de_ep_input_focus_params, q);
    ed_view *v = view_or_fail(req, w, q.view);
    if (v == NULL)
        return;
    v->focused = q.focused;
    /* A Game view in Play takes the input while it has focus. */
    const bool capture = v->game && q.focused && ed->mode == DE_EP_WORLD_MODE_PLAY;
    empty_response(w, req->id);
    if (capture != v->captured) {
        v->captured = capture;
        send_capture(ed, v);
    }
}

/* ---------- Gizmo ---------- */

static void on_gizmo_tool(de__editor *ed, const de_ep_request *req, de_ep_arena *a, mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(gizmo_set_tool, de_ep_gizmo_tool_params, q);
    de__ed_gizmo_set_tool(&q);
    empty_response(w, req->id);
}

static void on_gizmo_selection(de__editor *ed, const de_ep_request *req, de_ep_arena *a,
                               mpack_writer_t *w) {
    (void)ed;
    DE__ED_PARAMS(gizmo_set_selection, de_ep_entity_list, q);
    de__ed_gizmo_set_selection(q.entities.items, q.entities.count);
    empty_response(w, req->id);
}

#if ED_VIEWS
static void quat_rotate(const f32 q[4], const f64 v[3], f64 out[3]) {
    const f64 x = q[0], y = q[1], z = q[2], ww = q[3];
    const f64 tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + ww * tx + (y * tz - z * ty);
    out[1] = v[1] + ww * ty + (z * tx - x * tz);
    out[2] = v[2] + ww * tz + (x * ty - y * tx);
}
#endif

/* ---------- Per frame ---------- */

static void tick(de__editor *ed) {
    picks_tick(ed);
#if ED_VIEWS
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i) {
        ed_view *v = &g_views.views[i];
        if (v->id == 0 || v->paused)
            continue;
        if (v->game) {
            /* The window changed size: follow it with a new readback (the ring stays; frames of
             * another size are skipped until the IDE resizes). */
            u32 ow = 0, oh = 0;
            de_renderer_output_size(ed->e->renderer, &ow, &oh);
            if ((ow != v->sw || oh != v->sh) && ow && oh) {
                de__render_world_wait_idle(ed->e->renderer);
                v->sw = ow;
                v->sh = oh;
                (void)readback_alloc(ed, v, ow, oh);
                v->consumed = de_atomic_load_u64(&v->produced, DE_ATOMIC_ACQUIRE);
            }
        } else if (v->cam_dirty) {
            /* Only when it moved: each write is a change the IDE's deltas carry. */
            v->cam_dirty = false;
            camera_apply(ed, v);
        }
        const u64 produced = de_atomic_load_u64(&v->produced, DE_ATOMIC_ACQUIRE);
        /* The newest readback whose frame is complete: two newer ones were recorded since. */
        if (produced < v->consumed + 3)
            continue;
        const u64 k = produced - 3;
        v->consumed = k + 1;
        u32 surface = 0;
        u64 fence = 0;
        if (!present(v, v->rb_map[k % ED_RB_SLOTS], v->rb_pitch, v->rb_w, v->rb_h, &surface, &fence))
            continue;
        de_ep_view_frame d;
        memset(&d, 0, sizeof d);
        d.view = v->id;
        d.generation = v->rings[v->ring_count - 1].generation;
        d.surface = surface;
        d.frame = ++v->frames;
        d.has_fence_value = true;
        d.fence_value = fence;
        mpack_writer_t nw;
        const u64 seq = de__ed_note_begin(ed, &nw);
        de_ep_write_notify_view_frame(&nw, seq, &d);
        de__ed_send(ed, &nw, DE_EP_KIND_NOTIFICATION);
    }
#endif
}

static void reset(de__editor *ed) {
    for (u32 i = 0; i < ED_MAX_VIEWS; ++i)
        if (g_views.views[i].id != 0)
            view_release_all(ed, &g_views.views[i]);
    memset(g_views.picks, 0, sizeof g_views.picks);
#if ED_VIEWS
    de__view_hidden_release(g_views.hidden);
    g_views.hidden = NULL;
#endif
    de__ed_gizmo_reset(ed);
}

void de__ed_views_register(de__editor *ed) {
    memset(&g_views, 0, sizeof g_views);
    ed->views = (struct de__ed_views *)&g_views;
    g_ed = ed;
#if ED_VIEWS
    if (ed->e->renderer != NULL && ed->e->gpu != NULL) {
        de__ed_add_capability(ed, "views.sharedTexture");
        de__ed_add_capability(ed, "views.frameRateLimit");
    }
#endif
    de__ed_on(ed, DE_EP_METHOD_VIEW_CREATE, on_create);
    de__ed_on(ed, DE_EP_METHOD_VIEW_RESIZE, on_resize);
    de__ed_on(ed, DE_EP_METHOD_VIEW_RELEASE, on_release);
    de__ed_on(ed, DE_EP_METHOD_VIEW_SET_CAMERA, on_set_camera);
    de__ed_on(ed, DE_EP_METHOD_VIEW_SET_RENDER_MODE, on_render_mode);
    de__ed_on(ed, DE_EP_METHOD_VIEW_SET_SHOW_FLAGS, on_show_flags);
    de__ed_on(ed, DE_EP_METHOD_VIEW_SET_PAUSED, on_paused);
    de__ed_on(ed, DE_EP_METHOD_VIEW_SET_MAX_FPS, on_max_fps);
    de__ed_on(ed, DE_EP_METHOD_VIEW_DESTROY, on_destroy);
    de__ed_on(ed, DE_EP_METHOD_VIEW_SET_HIDDEN, on_set_hidden);
    de__ed_on(ed, DE_EP_METHOD_PICK, on_pick);
    de__ed_on(ed, DE_EP_METHOD_PICK_RECT, on_pick_rect);
    de__ed_on(ed, DE_EP_METHOD_INPUT_EVENTS, on_input);
    de__ed_on(ed, DE_EP_METHOD_INPUT_FOCUS, on_focus);
    de__ed_on(ed, DE_EP_METHOD_GIZMO_SET_TOOL, on_gizmo_tool);
    de__ed_on(ed, DE_EP_METHOD_GIZMO_SET_SELECTION, on_gizmo_selection);
    de__ed_on_tick(ed, tick);
    de__ed_on_reset(ed, reset);
}

/** Frees the views and the bridge device (service stop). */
void de__ed_views_shutdown(de__editor *ed);
void de__ed_views_shutdown(de__editor *ed) {
    reset(ed);
#if ED_WIN_VIEWS
    ed_bridge *b = &g_views.bridge;
    if (g_views.bridge_ok) {
        CloseHandle(b->event);
        ID3D12Fence_Release(b->wait);
        ID3D12GraphicsCommandList_Release(b->list);
        ID3D12CommandAllocator_Release(b->alloc);
        ID3D12CommandQueue_Release(b->queue);
        ID3D12Device_Release(b->dev);
        g_views.bridge_ok = false;
    }
#endif
    g_ed = NULL;
}
