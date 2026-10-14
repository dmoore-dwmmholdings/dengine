/* test_d3d12_warp.c — the vendored WARP is the one the D3D12 tests run on (DENG-405).
 *
 * Built into test_gpu_d3d12 only when third_party/d3d12_warp copied the vendored d3d10warp.dll
 * (test-only, Microsoft.Direct3D.WARP) next to the executable: Windows x64 with
 * DE_D3D12_TEST_WARP=ON (de_d3d12_warp_deploy defines DE_D3D12_TEST_WARP_DLL).
 *
 *   - d3d12.vendored_warp: opens the WARP adapter, then checks that the d3d10warp.dll in this
 *     process is the copy next to the executable, not the one of Windows, and that this WARP has
 *     what the D3D12 tests need: the Base tier (shader model 6.6, binding tier 3) so the pipeline
 *     tests (DENG-324) run, enhanced barriers so the enhanced-barrier path (DENG-286) runs, and
 *     mesh shaders. Without this test, a WARP that silently fell back to the OS copy would only
 *     turn those tests back into SKIPs.
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "de_test.h"

#include <dengine/de_gpu.h>

#include "gpu/d3d12/d3d12_internal.h"

#include <stdio.h>
#include <string.h>

/* The directory part of a path, in place: "C:\a\b.dll" -> "C:\a". */
static void cut_to_dir(char *path) {
    char *slash = strrchr(path, '\\');
    char *fwd = strrchr(path, '/');
    if (fwd && (!slash || fwd > slash))
        slash = fwd;
    if (slash)
        *slash = '\0';
}

static bool is_warp(const de_gpu_caps *c) {
    return c->vendor_id == 0x1414u && c->device_id == 0x8cu;
}

TEST(d3d12, vendored_warp) {
    /* The WARP adapter: the software adapters are last in the backend's list. */
    de_gpu_device *d = NULL;
    for (u32 n = 1; !d; ++n) {
        de_gpu_device_desc desc = {.backend = DE_GPU_D3D12, .adapter = n, .debug_layer = true};
        de_gpu_device *c = NULL;
        de_result r = de_gpu_device_create(&desc, &c);
        ASSERT_NE(r, DE_ERR_INVALID_ARGUMENT); /* past the end of the list: no WARP at all */
        ASSERT_LT(n, 64u);
        if (r != DE_OK)
            continue;
        if (is_warp(de_gpu_get_caps(c)))
            d = c;
        else
            de_gpu_device_destroy(c);
    }

    /* Creating the WARP device loaded d3d10warp.dll: it must be the copy in our directory. */
    char exe[MAX_PATH], warp[MAX_PATH];
    HMODULE m = GetModuleHandleA("d3d10warp.dll");
    ASSERT_TRUE(m != NULL);
    DWORD n_exe = GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe);
    DWORD n_warp = GetModuleFileNameA(m, warp, (DWORD)sizeof warp);
    ASSERT_TRUE(n_exe > 0 && n_exe < sizeof exe);
    ASSERT_TRUE(n_warp > 0 && n_warp < sizeof warp);
    printf("WARP driver: %s\n", warp);
    cut_to_dir(exe);
    cut_to_dir(warp);
    EXPECT_EQ(CompareStringA(LOCALE_INVARIANT, NORM_IGNORECASE, exe, -1, warp, -1), CSTR_EQUAL);

    const de_gpu_caps *c = de_gpu_get_caps(d);
    const char *why = NULL;
    const bool pipelines = de__d3d12_pipelines_supported(d, &why);
    printf("WARP \"%s\": base tier %s%s%s; pipelines %s; enhanced barriers %d; mesh shaders %d\n",
           c->device_name, c->base_tier_ok ? "ok" : "NO", c->base_tier_ok ? "" : ": ",
           c->base_tier_reason, pipelines ? "yes" : why, c->enhanced_barriers, c->mesh_shaders);
    EXPECT_TRUE(c->base_tier_ok);
    EXPECT_TRUE(pipelines);
    EXPECT_TRUE(c->enhanced_barriers);
    EXPECT_TRUE(c->mesh_shaders);

    de_gpu_wait_idle(d);
    EXPECT_EQ(de__d3d12_debug_error_count(d), 0u);
    de_gpu_device_destroy(d);
}
