/* d3d12_agility_exports.h — the D3D12 Agility SDK exports of an executable (DENG-285).
 *
 * The D3D12 loader (d3d12.dll) reads two exported variables from the process's .exe to find the
 * Agility runtime: D3D12SDKVersion (must equal the SDK version of the D3D12Core.dll it loads) and
 * D3D12SDKPath (the directory of that DLL, relative to the .exe). They must be in the executable,
 * not in a DLL, so each executable that creates D3D12 devices includes this header in exactly one
 * of its C files, and its CMakeLists.txt calls de_d3d12_agility_deploy(<target>)
 * (third_party/d3d12_agility/CMakeLists.txt), which copies the DLLs to <exe dir>/D3D12/.
 *
 * Without the exports, or without the DLLs in D3D12/, the OS D3D12 runtime is used; the backend
 * still works there but reports what that runtime lacks (shader model, enhanced barriers).
 *
 * Define DE_D3D12_AGILITY_NO_EXPORTS before including it to get only the version number (the
 * backend checks it against the d3d12.h it compiles with).
 */
#ifndef DE_GPU_D3D12_AGILITY_VERSION_H
#define DE_GPU_D3D12_AGILITY_VERSION_H
/** The Agility SDK version pinned in third_party/vendor.toml (d3d12_agility 1.619.6). */
#define DE_D3D12_AGILITY_SDK_VERSION 619
#endif

/* The exports have their own guard, so a file that first included this header for the version
 * only (d3d12_internal.h) can still include it for the exports. */
#if defined(_WIN32) && !defined(DE_D3D12_AGILITY_NO_EXPORTS) &&                                    \
    !defined(DE_GPU_D3D12_AGILITY_EXPORTS_H)
#define DE_GPU_D3D12_AGILITY_EXPORTS_H
/* UINT and LPCSTR, without <windows.h>. In C a const object at file scope has external linkage;
 * C++ needs `extern` for it (and C warns about `extern` with an initializer). */
#ifdef __cplusplus
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = DE_D3D12_AGILITY_SDK_VERSION;
__declspec(dllexport) extern const char *D3D12SDKPath = ".\\D3D12\\";
}
#else
__declspec(dllexport) const unsigned int D3D12SDKVersion = DE_D3D12_AGILITY_SDK_VERSION;
__declspec(dllexport) const char *D3D12SDKPath = ".\\D3D12\\";
#endif
#endif
