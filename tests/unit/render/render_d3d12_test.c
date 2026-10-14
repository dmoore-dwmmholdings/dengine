/* render_d3d12_test.c: render_d3d12_report (render_d3d12_test.h; DENG-420, DENG-423). */
#include "render_d3d12_test.h"

#include "de_test.h"

#if defined(_WIN32) && DE_HAS_GPU_D3D12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "gpu/d3d12/d3d12_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The directory of `path` in place ("C:\a\b.dll" -> "C:\a"). */
static inline void render_d3d12__cut_dir(char *path) {
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    char *cut = a > b ? a : b;
    if (cut != NULL)
        *cut = '\0';
}

/* The path of loaded module `dll` (false when it is not loaded); prints it with its version. */
static inline bool render_d3d12__module(const char *dll, char *path, size_t cap) {
    path[0] = '\0';
    HMODULE m = GetModuleHandleA(dll);
    if (m == NULL) {
        printf("  %s: not loaded\n", dll);
        return false;
    }
    const DWORD n = GetModuleFileNameA(m, path, (DWORD)cap);
    if (n == 0 || n >= cap) {
        path[0] = '\0';
        printf("  %s: loaded (path unknown)\n", dll);
        return false;
    }
    char version[64] = "?";
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeA(path, &handle);
    void *info = size != 0 ? malloc(size) : NULL;
    if (info != NULL && GetFileVersionInfoA(path, 0, size, info)) {
        VS_FIXEDFILEINFO *ffi = NULL;
        UINT len = 0;
        if (VerQueryValueA(info, "\\", (void **)&ffi, &len) && ffi != NULL && len != 0)
            snprintf(version, sizeof version, "%u.%u.%u.%u", (unsigned)HIWORD(ffi->dwFileVersionMS),
                     (unsigned)LOWORD(ffi->dwFileVersionMS), (unsigned)HIWORD(ffi->dwFileVersionLS),
                     (unsigned)LOWORD(ffi->dwFileVersionLS));
    }
    free(info);
    printf("  %s: %s (version %s)\n", dll, path, version);
    return true;
}

/* True when `dll_dir` is `exe_dir` + `sub` ("" or "\\D3D12"), ignoring case. */
static inline bool render_d3d12__in(const char *dll_dir, const char *exe_dir, const char *sub) {
    char want[MAX_PATH];
    snprintf(want, sizeof want, "%s%s", exe_dir, sub);
    return CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, dll_dir, -1, want, -1) == CSTR_EQUAL;
}

/** Logs the D3D12 device `d` and the runtime and WARP of this process; on the vendored WARP also
 * checks them (see the header). Nothing for a device of another backend. */
void render_d3d12_report(de_gpu_device *d) {
    const de_gpu_caps *c = de_gpu_get_caps(d);
    if (c == NULL || c->backend != DE_GPU_D3D12)
        return;
    const char *why = NULL;
    const bool pipes = de__d3d12_pipelines_supported(d, &why);
    printf("D3D12 adapter \"%s\" (vendor 0x%04x, device 0x%04x): base tier %s%s%s; pipelines %s\n",
           c->device_name, (unsigned)c->vendor_id, (unsigned)c->device_id,
           c->base_tier_ok ? "ok" : "NO", c->base_tier_ok ? "" : ": ", c->base_tier_reason,
           pipes ? "yes" : (why != NULL ? why : "no"));
    char core[MAX_PATH], warp[MAX_PATH], exe[MAX_PATH];
    const bool have_core = render_d3d12__module("D3D12Core.dll", core, sizeof core);
    const bool have_warp = render_d3d12__module("d3d10warp.dll", warp, sizeof warp);
    const DWORD n = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    if (n == 0 || n >= sizeof exe)
        exe[0] = '\0';
    render_d3d12__cut_dir(exe);
    render_d3d12__cut_dir(core);
    render_d3d12__cut_dir(warp);
    const bool agility = have_core && render_d3d12__in(core, exe, "\\D3D12");
    printf("  runtime: %s\n",
           agility ? "Agility SDK (D3D12\\ next to the executable)" : "the OS D3D12 runtime");
#if defined(DE_D3D12_TEST_WARP_DLL)
    if (c->vendor_id == 0x1414u && c->device_id == 0x8cu) {
        /* CI's vendored WARP (DENG-405): the Agility runtime, this WARP, the Base tier. */
        EXPECT_TRUE(agility);
        EXPECT_TRUE(have_warp && render_d3d12__in(warp, exe, ""));
        EXPECT_TRUE(c->base_tier_ok);
        EXPECT_TRUE(pipes);
    }
#else
    (void)have_warp;
#endif
}
#else
void render_d3d12_report(de_gpu_device *d) {
    (void)d;
}
#endif
