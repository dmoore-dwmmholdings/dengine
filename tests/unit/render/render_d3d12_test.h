/*
 * render_d3d12_test.h: what a render test's D3D12 device runs on, logged and checked (DENG-420,
 * DENG-423). Add render_d3d12_test.c to the sources of a render test executable that creates
 * D3D12 devices (it keeps <windows.h> out of the test files: its macros, such as `small`, clash
 * with test code).
 *
 *     de_gpu_device *d = ...;                     // a D3D12 device
 *     render_d3d12_report(d);                     // logs; on CI's WARP also checks
 *
 * render_d3d12_report prints the adapter, the Base tier (and why not), pipeline support, and the
 * path and file version of the D3D12Core.dll and d3d10warp.dll the process loaded. When the
 * vendored WARP is deployed next to the executable (de_d3d12_warp_deploy: DE_D3D12_TEST_WARP_DLL)
 * and the device is WARP, it also requires:
 *   - the Agility runtime: D3D12Core.dll from <exe dir>/D3D12/ (de_d3d12_agility_deploy and the
 *     exports it adds), not System32's;
 *   - the vendored d3d10warp.dll from <exe dir>;
 *   - the Base tier (shader model 6.6), so pipelines can be made.
 * A test that silently fell back to the OS runtime (shader model 6.5 on that WARP, DENG-420) then
 * fails instead of passing with less coverage. Elsewhere (macOS, Linux, a hardware D3D12 device)
 * the functions do nothing or only log.
 *
 * Link `version` on Windows (the file-version query): tests/unit/render/CMakeLists.txt does.
 */
#ifndef DE_TEST_RENDER_D3D12_TEST_H
#define DE_TEST_RENDER_D3D12_TEST_H

#include <dengine/de_gpu.h>

/** Logs the D3D12 device `d` and the runtime and WARP of this process; on the vendored WARP also
 * checks them (see above). Nothing for a device of another backend, or outside Windows. */
void render_d3d12_report(de_gpu_device *d);

#endif /* DE_TEST_RENDER_D3D12_TEST_H */
