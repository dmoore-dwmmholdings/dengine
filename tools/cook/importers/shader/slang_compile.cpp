/*
 * slang_compile.cpp: the Slang shim (DENG-289). The only C++ file of the shader importer and the
 * only engine file that includes slang.h. See slang_compile.h for the contract.
 *
 * Slang is opened at run time (de_dylib_open), so no target links it. The COM interfaces work
 * through their vtables, which needs no symbol. The reflection API is a set of plain exported
 * functions (spReflection*): slang.h's inline wrappers would need them at link time, so this file
 * never calls the wrappers and resolves the functions it uses by name instead (k_fns below).
 *
 * Determinism (SPEC §11.5): the session gets no debug information and only relative paths; the
 * importer serves every file. The same source, defines and target give the same bytes.
 */
#include "cook/importers/shader/slang_compile.h"

#include "gpu/shader/shader_format.h"

#include <dengine/de_base.h>
#include <dengine/de_memory.h>
#include <dengine/de_platform.h>

#include <slang-tag-version.h>
#include <slang.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace {

/* ---- Library and global session ------------------------------------------------------------ */

using create_global_session_fn = SlangResult (*)(const SlangGlobalSessionDesc *,
                                                 slang::IGlobalSession **);
using build_tag_fn = const char *(*)();

/* The reflection functions this file calls, resolved by name. */
struct reflection_fns {
    decltype(&spReflection_getEntryPointCount) entry_point_count;
    decltype(&spReflection_getEntryPointByIndex) entry_point_by_index;
    decltype(&spReflection_GetParameterCount) parameter_count;
    decltype(&spReflection_GetParameterByIndex) parameter_by_index;
    decltype(&spReflectionEntryPoint_getName) entry_name;
    decltype(&spReflectionEntryPoint_getStage) entry_stage;
    decltype(&spReflectionEntryPoint_getComputeThreadGroupSize) entry_threadgroup;
    decltype(&spReflectionEntryPoint_getParameterCount) entry_parameter_count;
    decltype(&spReflectionEntryPoint_getParameterByIndex) entry_parameter_by_index;
    decltype(&spReflectionVariableLayout_GetTypeLayout) var_type_layout;
    decltype(&spReflectionVariableLayout_GetOffset) var_offset;
    decltype(&spReflectionTypeLayout_GetSize) type_size;
    decltype(&spReflectionTypeLayout_GetElementTypeLayout) type_element;
    decltype(&spReflectionTypeLayout_GetParameterCategory) type_category;
    decltype(&spReflectionTypeLayout_GetFieldCount) field_count;
    decltype(&spReflectionTypeLayout_GetFieldByIndex) field_by_index;
};

bool same_uuid(const SlangUUID &a, const SlangUUID &b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

/* One library Slang opened through dir_loader. */
class shared_lib final : public ISlangSharedLibrary {
  public:
    explicit shared_lib(de_dylib *lib) : lib_(lib) {}

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(SlangUUID const &uuid,
                                                          void **out) override {
        void *p = castAs(uuid);
        if (p != nullptr)
            addRef();
        *out = p;
        return p != nullptr ? SLANG_OK : SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override {
        return ++refs_;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        uint32_t n = --refs_;
        if (n == 0)
            delete this;
        return n;
    }
    SLANG_NO_THROW void *SLANG_MCALL castAs(const SlangUUID &uuid) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) ||
            same_uuid(uuid, ISlangCastable::getTypeGuid()) ||
            same_uuid(uuid, ISlangSharedLibrary::getTypeGuid()))
            return static_cast<ISlangSharedLibrary *>(this);
        return nullptr;
    }
    SLANG_NO_THROW void *SLANG_MCALL findSymbolAddressByName(char const *name) override {
        return de_dylib_symbol(lib_, name);
    }

  private:
    ~shared_lib() {
        de_dylib_close(lib_);
    }
    de_dylib *lib_;
    std::atomic<uint32_t> refs_{1};
};

/* Slang's shared-library loader (slang-glslang for SPIR-V, dxcompiler and dxil for DXIL): first
 * the directory of the Slang library, then the system search. Slang's own loader opens these by
 * bare file name and relies on its rpath, which fails when a sanitizer runtime makes the dlopen
 * (ASan on macOS), so SPIR-V would report unsupported. `name` is unadorned ("slang-glslang-...").
 */
class dir_loader final : public ISlangSharedLibraryLoader {
  public:
    explicit dir_loader(std::string dir) : dir_(std::move(dir)) {}

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(SlangUUID const &uuid,
                                                          void **out) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) ||
            same_uuid(uuid, ISlangSharedLibraryLoader::getTypeGuid())) {
            *out = static_cast<ISlangSharedLibraryLoader *>(this);
            return SLANG_OK;
        }
        *out = nullptr;
        return SLANG_E_NO_INTERFACE;
    }
    /* Lives as long as the process: no reference count. */
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override {
        return 1;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        return 1;
    }
    SLANG_NO_THROW SlangResult SLANG_MCALL loadSharedLibrary(const char *name,
                                                             ISlangSharedLibrary **out) override {
        *out = nullptr;
#if defined(_WIN32)
        const std::string file = std::string(name) + ".dll";
#elif defined(__APPLE__)
        const std::string file = std::string("lib") + name + ".dylib";
#else
        const std::string file = std::string("lib") + name + ".so";
#endif
        const bool has_dir =
            std::strchr(name, '/') != nullptr || std::strchr(name, '\\') != nullptr;
        de_dylib *lib = nullptr;
        if (has_dir || dir_.empty() || de_dylib_open((dir_ + "/" + file).c_str(), &lib) != DE_OK) {
            lib = nullptr;
            if (de_dylib_open(has_dir ? name : file.c_str(), &lib) != DE_OK) {
                lib = nullptr;
#if defined(_WIN32)
                /* DXC ships with the Windows SDK; a developer prompt names its bin directory even
                 * when it is not on PATH. Only where the library is looked up, never in output. */
                char *sdk = nullptr;
                size_t n = 0;
                if (!has_dir && _dupenv_s(&sdk, &n, "WindowsSdkVerBinPath") == 0 &&
                    sdk != nullptr) {
                    std::string path = std::string(sdk) + "x64\\" + file;
                    std::free(sdk);
                    if (de_dylib_open(path.c_str(), &lib) != DE_OK)
                        lib = nullptr;
                }
#endif
                if (lib == nullptr)
                    return SLANG_E_NOT_FOUND;
            }
        }
        *out = new (std::nothrow) shared_lib(lib);
        if (*out == nullptr) {
            de_dylib_close(lib);
            return SLANG_E_OUT_OF_MEMORY;
        }
        return SLANG_OK;
    }

  private:
    std::string dir_;
};

struct slang_state {
    std::once_flag once;
    std::mutex lock; /* every use of the global session and its sessions */
    bool ok = false;
    char why[512] = {0};
    char version[32] = {0};
    de_dylib *lib = nullptr;
    dir_loader *loader = nullptr;
    slang::IGlobalSession *global = nullptr;
    reflection_fns fn = {};
    bool target_ok[DE__SLANG_TARGET_COUNT_] = {};
};

slang_state &state() {
    /* Never destroyed: the global session lives until the process exits, and the library is
     * never closed (Slang's own atexit work runs from it; on Windows see load() for the DLLs
     * that must outlive it, DENG-418). */
    static slang_state *s = new slang_state();
    return *s;
}

template <class F> bool resolve(de_dylib *lib, const char *name, F &out) {
    de_dylib_fn f = de_dylib_function(lib, name);
    out = reinterpret_cast<F>(f);
    return f != nullptr;
}

void load(slang_state &s) {
#ifndef DE_SLANG_LIBRARY
    std::snprintf(s.why, sizeof s.why,
                  "the Slang compiler is not available in this build (no prebuilt Slang for "
                  "this host in third_party/vendor.toml)");
    return;
#else
#if defined(_WIN32)
    /* DENG-418: bcrypt.dll and bcryptprimitives.dll before Slang, never closed. The Windows
     * slang-compiler.dll links its C runtime statically and loads bcrypt only when it first needs
     * random bytes, so bcrypt and bcryptprimitives would be initialised after it, and Windows
     * shuts DLLs down in the reverse order: at process exit they would be gone when Slang's own
     * shutdown runs (its static and thread_local destructors, from DLL_PROCESS_DETACH), which
     * calls BCryptGenRandom. That read freed RNG state and crashed the process now and then
     * (an access violation in bcryptprimitives at exit; Windows Debug CI, cook_builtin.project and
     * cook_cli.builtin_project). Loaded first, they are shut down after Slang. Both are system
     * libraries (System32), so loading them by name finds no other copy. */
    {
        de_dylib *keep = nullptr;
        (void)de_dylib_open("bcrypt.dll", &keep);
        keep = nullptr;
        (void)de_dylib_open("bcryptprimitives.dll", &keep);
        de_error_clear(); /* a failure only leaves the old order; Slang still loads */
    }
#endif
    if (de_dylib_open(DE_SLANG_LIBRARY, &s.lib) != DE_OK) {
        const de_error *e = de_error_last();
        std::snprintf(s.why, sizeof s.why, "cannot open the Slang compiler \"%s\": %.200s",
                      DE_SLANG_LIBRARY, e != nullptr ? e->message : "unknown error");
        return;
    }
    create_global_session_fn create = nullptr;
    build_tag_fn tag = nullptr;
    reflection_fns &f = s.fn;
    bool all =
        resolve(s.lib, "slang_createGlobalSession2", create) &&
        resolve(s.lib, "spGetBuildTagString", tag) &&
        resolve(s.lib, "spReflection_getEntryPointCount", f.entry_point_count) &&
        resolve(s.lib, "spReflection_getEntryPointByIndex", f.entry_point_by_index) &&
        resolve(s.lib, "spReflection_GetParameterCount", f.parameter_count) &&
        resolve(s.lib, "spReflection_GetParameterByIndex", f.parameter_by_index) &&
        resolve(s.lib, "spReflectionEntryPoint_getName", f.entry_name) &&
        resolve(s.lib, "spReflectionEntryPoint_getStage", f.entry_stage) &&
        resolve(s.lib, "spReflectionEntryPoint_getComputeThreadGroupSize", f.entry_threadgroup) &&
        resolve(s.lib, "spReflectionEntryPoint_getParameterCount", f.entry_parameter_count) &&
        resolve(s.lib, "spReflectionEntryPoint_getParameterByIndex", f.entry_parameter_by_index) &&
        resolve(s.lib, "spReflectionVariableLayout_GetTypeLayout", f.var_type_layout) &&
        resolve(s.lib, "spReflectionVariableLayout_GetOffset", f.var_offset) &&
        resolve(s.lib, "spReflectionTypeLayout_GetSize", f.type_size) &&
        resolve(s.lib, "spReflectionTypeLayout_GetElementTypeLayout", f.type_element) &&
        resolve(s.lib, "spReflectionTypeLayout_GetParameterCategory", f.type_category) &&
        resolve(s.lib, "spReflectionTypeLayout_GetFieldCount", f.field_count) &&
        resolve(s.lib, "spReflectionTypeLayout_GetFieldByIndex", f.field_by_index);
    if (!all) {
        std::snprintf(s.why, sizeof s.why,
                      "the Slang compiler \"%s\" lacks a function the importer needs (expected "
                      "Slang %s)",
                      DE_SLANG_LIBRARY, SLANG_TAG_VERSION);
        return;
    }
    std::snprintf(s.version, sizeof s.version, "%s", tag() != nullptr ? tag() : "");
    SlangGlobalSessionDesc gd = {};
    gd.enableGLSL = false;
    if (SLANG_FAILED(create(&gd, &s.global)) || s.global == nullptr) {
        std::snprintf(s.why, sizeof s.why, "cannot create the Slang global session (Slang %s)",
                      s.version);
        return;
    }
    std::string dir(DE_SLANG_LIBRARY);
    size_t slash = dir.find_last_of("/\\");
    dir.resize(slash != std::string::npos ? slash : 0);
    s.loader = new (std::nothrow) dir_loader(dir);
    if (s.loader != nullptr)
        s.global->setSharedLibraryLoader(s.loader);
    s.target_ok[DE__SLANG_SPIRV] =
        SLANG_SUCCEEDED(s.global->checkCompileTargetSupport(SLANG_SPIRV));
    s.target_ok[DE__SLANG_DXIL] = SLANG_SUCCEEDED(s.global->checkCompileTargetSupport(SLANG_DXIL));
    s.target_ok[DE__SLANG_METALLIB] =
        SLANG_SUCCEEDED(s.global->checkCompileTargetSupport(SLANG_METAL_LIB));
    s.target_ok[DE__SLANG_METAL] =
        SLANG_SUCCEEDED(s.global->checkCompileTargetSupport(SLANG_METAL));
    s.ok = true;
#endif
}

slang_state *get() {
    slang_state &s = state();
    std::call_once(s.once, [&s] { load(s); });
    return s.ok ? &s : nullptr;
}

/* ---- COM objects: a blob and the importer's file system -------------------------------------- */

/* A heap blob with a real reference count: Slang keeps file blobs past loadFile. */
class byte_blob final : public ISlangBlob {
  public:
    byte_blob(const void *data, size_t size)
        : bytes_(static_cast<const unsigned char *>(data),
                 static_cast<const unsigned char *>(data) + size) {}

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(SlangUUID const &uuid,
                                                          void **out) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) ||
            same_uuid(uuid, ISlangBlob::getTypeGuid())) {
            addRef();
            *out = static_cast<ISlangBlob *>(this);
            return SLANG_OK;
        }
        *out = nullptr;
        return SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override {
        return ++refs_;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        uint32_t n = --refs_;
        if (n == 0)
            delete this;
        return n;
    }
    SLANG_NO_THROW void const *SLANG_MCALL getBufferPointer() override {
        return bytes_.empty() ? static_cast<const void *>("") : bytes_.data();
    }
    SLANG_NO_THROW size_t SLANG_MCALL getBufferSize() override {
        return bytes_.size();
    }

  private:
    ~byte_blob() = default;
    std::vector<unsigned char> bytes_;
    std::atomic<uint32_t> refs_{1};
};

/* The directory the in-memory modules appear in. A relative name no asset path can have (asset
 * paths never start with '@'), and a search path of the session. */
const char k_module_dir[] = "@de-common";

/* Serves the in-memory modules and passes every other path to the importer's reader. It lives on
 * the compile's stack and outlives the session, so its reference count is a formality. */
class import_fs final : public ISlangFileSystem {
  public:
    explicit import_fs(const de__slang_compile_desc &d) : d_(d) {}

    SLANG_NO_THROW SlangResult SLANG_MCALL queryInterface(SlangUUID const &uuid,
                                                          void **out) override {
        void *p = castAs(uuid);
        *out = p;
        return p != nullptr ? SLANG_OK : SLANG_E_NO_INTERFACE;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL addRef() override {
        return 1;
    }
    SLANG_NO_THROW uint32_t SLANG_MCALL release() override {
        return 1;
    }
    SLANG_NO_THROW void *SLANG_MCALL castAs(const SlangUUID &uuid) override {
        if (same_uuid(uuid, ISlangUnknown::getTypeGuid()) ||
            same_uuid(uuid, ISlangCastable::getTypeGuid()) ||
            same_uuid(uuid, ISlangFileSystem::getTypeGuid()))
            return static_cast<ISlangFileSystem *>(this);
        return nullptr;
    }
    SLANG_NO_THROW SlangResult SLANG_MCALL loadFile(char const *path, ISlangBlob **out) override {
        *out = nullptr;
        const size_t dir_len = sizeof k_module_dir - 1u;
        if (std::strncmp(path, k_module_dir, dir_len) == 0 && path[dir_len] == '/') {
            const char *file = path + dir_len + 1u;
            for (u32 i = 0; i < d_.module_count; ++i) {
                const de__slang_module &m = d_.modules[i];
                size_t n = std::strlen(m.name);
                if (std::strncmp(file, m.name, n) == 0 && std::strcmp(file + n, ".slang") == 0) {
                    *out = new (std::nothrow) byte_blob(m.source, m.size);
                    return *out != nullptr ? SLANG_OK : SLANG_E_OUT_OF_MEMORY;
                }
            }
            return SLANG_E_NOT_FOUND;
        }
        /* Probes that never reach the project's files: precompiled modules (the engine cooks
         * from source only), and the engine modules' names in any directory (they are reserved,
         * so a project file cannot shadow them; Slang also tries '-' for '_'). Each read would
         * otherwise become a missing extra source of the cook. */
        if (ends_with(path, ".slang-module") || is_module_name(path))
            return SLANG_E_NOT_FOUND;
        const void *data = nullptr;
        u64 size = 0;
        de_result r = d_.read(d_.read_user, path, &data, &size);
        if (r == DE_ERR_NOT_FOUND)
            return SLANG_E_NOT_FOUND;
        if (r != DE_OK)
            return SLANG_FAIL;
        *out = new (std::nothrow) byte_blob(data, static_cast<size_t>(size));
        return *out != nullptr ? SLANG_OK : SLANG_E_OUT_OF_MEMORY;
    }

  private:
    static bool ends_with(const char *s, const char *suffix) {
        size_t n = std::strlen(s), k = std::strlen(suffix);
        return n >= k && std::strcmp(s + n - k, suffix) == 0;
    }
    /* True when the file name of `path` is "<engine module>.slang" ('-' matching '_'). */
    bool is_module_name(const char *path) const {
        const char *file = std::strrchr(path, '/');
        file = file != nullptr ? file + 1 : path;
        for (u32 i = 0; i < d_.module_count; ++i) {
            const char *m = d_.modules[i].name;
            size_t n = std::strlen(m), k = 0;
            for (; k < n; ++k) {
                char a = file[k], b = m[k];
                if (a != b && !(a == '-' && b == '_'))
                    break;
            }
            if (k == n && std::strcmp(file + n, ".slang") == 0)
                return true;
        }
        return false;
    }
    const de__slang_compile_desc &d_;
};

/* Releases a COM pointer at scope exit. */
template <class T> struct com_ref {
    T *p = nullptr;
    com_ref() = default;
    com_ref(const com_ref &) = delete;
    com_ref &operator=(const com_ref &) = delete;
    ~com_ref() {
        if (p != nullptr)
            p->release();
    }
    T **put() {
        return &p;
    }
    T *operator->() const {
        return p;
    }
};

/* ---- Helpers --------------------------------------------------------------------------------- */

/* Appends Slang's diagnostics. When they do not fit, the oldest text goes: errors come last. */
void append_diag(de__slang_compile_result *out, ISlangBlob *diag) {
    if (diag == nullptr || diag->getBufferSize() == 0)
        return;
    const size_t cap = sizeof out->diagnostics - 1u;
    const char *text = static_cast<const char *>(diag->getBufferPointer());
    size_t n = diag->getBufferSize();
    if (n > 0 && text[n - 1u] == '\0')
        --n;
    if (n > cap) {
        text += n - cap;
        n = cap;
    }
    size_t used = std::strlen(out->diagnostics);
    if (used + n > cap) {
        size_t drop = used + n - cap;
        std::memmove(out->diagnostics, out->diagnostics + drop, used - drop);
        used -= drop;
    }
    std::memcpy(out->diagnostics + used, text, n);
    out->diagnostics[used + n] = '\0';
}

void set_diag(de__slang_compile_result *out, const char *fmt, const char *a = "",
              const char *b = "") {
    std::snprintf(out->diagnostics, sizeof out->diagnostics, fmt, a, b);
}

SlangStage to_slang_stage(u32 stage) {
    switch (stage) {
    case DE__SHADER_STAGE_VERTEX:
        return SLANG_STAGE_VERTEX;
    case DE__SHADER_STAGE_PIXEL:
        return SLANG_STAGE_FRAGMENT;
    case DE__SHADER_STAGE_COMPUTE:
        return SLANG_STAGE_COMPUTE;
    case DE__SHADER_STAGE_MESH:
        return SLANG_STAGE_MESH;
    case DE__SHADER_STAGE_AMPLIFICATION:
        return SLANG_STAGE_AMPLIFICATION;
    case DE__SHADER_STAGE_RAYGEN:
        return SLANG_STAGE_RAY_GENERATION;
    case DE__SHADER_STAGE_MISS:
        return SLANG_STAGE_MISS;
    case DE__SHADER_STAGE_CLOSEST_HIT:
        return SLANG_STAGE_CLOSEST_HIT;
    case DE__SHADER_STAGE_ANY_HIT:
        return SLANG_STAGE_ANY_HIT;
    default:
        return SLANG_STAGE_NONE;
    }
}

u32 from_slang_stage(SlangStage stage) {
    switch (stage) {
    case SLANG_STAGE_VERTEX:
        return DE__SHADER_STAGE_VERTEX;
    case SLANG_STAGE_FRAGMENT:
        return DE__SHADER_STAGE_PIXEL;
    case SLANG_STAGE_COMPUTE:
        return DE__SHADER_STAGE_COMPUTE;
    case SLANG_STAGE_MESH:
        return DE__SHADER_STAGE_MESH;
    case SLANG_STAGE_AMPLIFICATION:
        return DE__SHADER_STAGE_AMPLIFICATION;
    case SLANG_STAGE_RAY_GENERATION:
        return DE__SHADER_STAGE_RAYGEN;
    case SLANG_STAGE_MISS:
        return DE__SHADER_STAGE_MISS;
    case SLANG_STAGE_CLOSEST_HIT:
        return DE__SHADER_STAGE_CLOSEST_HIT;
    case SLANG_STAGE_ANY_HIT:
        return DE__SHADER_STAGE_ANY_HIT;
    default:
        return DE__SHADER_STAGE_NONE;
    }
}

/* The uniform bytes a value of type `t` uses: for a struct, the end of its last field (its
 * trailing padding is not data: a 132-byte block reports 132, not its 144-byte std430 size);
 * otherwise its size. */
size_t used_bytes(const reflection_fns &f, SlangReflectionTypeLayout *t) {
    uint32_t n = f.field_count(t);
    if (n == 0)
        return f.type_size(t, SLANG_PARAMETER_CATEGORY_UNIFORM);
    size_t end = 0;
    for (uint32_t i = 0; i < n; ++i) {
        SlangReflectionVariableLayout *v = f.field_by_index(t, i);
        SlangReflectionTypeLayout *ft = v != nullptr ? f.var_type_layout(v) : nullptr;
        if (ft == nullptr)
            continue;
        size_t size = f.type_size(ft, SLANG_PARAMETER_CATEGORY_UNIFORM);
        if (size == 0)
            continue;
        size_t at = f.var_offset(v, SLANG_PARAMETER_CATEGORY_UNIFORM) + size;
        end = at > end ? at : end;
    }
    return end;
}

/* Push-constant bytes of an entry point on the linked layout: the end of its uniform parameters,
 * plus what global push-constant buffers ([vk::push_constant]) use. */
u32 push_constant_bytes(const reflection_fns &f, SlangReflection *refl,
                        SlangReflectionEntryPoint *ep) {
    size_t end = 0;
    unsigned n = f.entry_parameter_count(ep);
    for (unsigned i = 0; i < n; ++i) {
        SlangReflectionVariableLayout *v = f.entry_parameter_by_index(ep, i);
        SlangReflectionTypeLayout *t = v != nullptr ? f.var_type_layout(v) : nullptr;
        if (t == nullptr || f.type_size(t, SLANG_PARAMETER_CATEGORY_UNIFORM) == 0)
            continue;
        size_t at = f.var_offset(v, SLANG_PARAMETER_CATEGORY_UNIFORM) + used_bytes(f, t);
        end = at > end ? at : end;
    }
    unsigned g = f.parameter_count(refl);
    for (unsigned i = 0; i < g; ++i) {
        SlangReflectionVariableLayout *v = f.parameter_by_index(refl, i);
        SlangReflectionTypeLayout *t = v != nullptr ? f.var_type_layout(v) : nullptr;
        if (t == nullptr || f.type_category(t) != SLANG_PARAMETER_CATEGORY_PUSH_CONSTANT_BUFFER)
            continue;
        SlangReflectionTypeLayout *e = f.type_element(t);
        if (e != nullptr)
            end += used_bytes(f, e);
    }
    return end > 0xffffffffu ? 0xffffffffu : static_cast<u32>(end);
}

char *dup_str(const de_allocator *a, const char *s) {
    size_t n = std::strlen(s) + 1u;
    char *p = static_cast<char *>(DE_ALLOC(a, n, 1, DE_MEM_ASSET));
    if (p != nullptr)
        std::memcpy(p, s, n);
    return p;
}

/* The module name Slang gets for the source: its path without the extension, '/' kept. */
std::string module_name_of(const char *path) {
    std::string name(path);
    size_t dot = name.rfind('.');
    size_t slash = name.rfind('/');
    if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
        name.resize(dot);
    return name;
}

de_result compile_locked(slang_state &s, const de__slang_compile_desc &d,
                         de__slang_compile_result *out) {
    static const SlangCompileTarget k_formats[DE__SLANG_TARGET_COUNT_] = {
        SLANG_SPIRV, SLANG_DXIL, SLANG_METAL_LIB, SLANG_METAL};
    static const char *const k_target_define[DE__SLANG_TARGET_COUNT_] = {
        "DE_SHADER_TARGET_SPIRV", "DE_SHADER_TARGET_DXIL", "DE_SHADER_TARGET_METAL",
        "DE_SHADER_TARGET_METAL"};

    /* Defines: the target's, then the permutation's ("NAME" or "NAME=VALUE"). */
    std::vector<std::string> names, values;
    names.emplace_back(k_target_define[d.target]);
    values.emplace_back("1");
    for (u32 i = 0; i < d.define_count; ++i) {
        const char *def = d.defines[i];
        const char *eq = std::strchr(def, '=');
        names.emplace_back(eq != nullptr ? std::string(def, static_cast<size_t>(eq - def))
                                         : std::string(def));
        values.emplace_back(eq != nullptr ? std::string(eq + 1) : std::string("1"));
    }
    std::vector<slang::PreprocessorMacroDesc> macros(names.size());
    for (size_t i = 0; i < names.size(); ++i)
        macros[i] = {names[i].c_str(), values[i].c_str()};

    slang::CompilerOptionEntry opts[2];
    opts[0].name = slang::CompilerOptionName::DebugInformation;
    opts[0].value.kind = slang::CompilerOptionValueKind::Int;
    opts[0].value.intValue0 = SLANG_DEBUG_INFO_LEVEL_NONE;
    /* E41012 "profile implicitly upgraded": the bindless accessors need capabilities beyond the
     * base SPIR-V 1.6 / SM 6.6 profile; Slang adds them, which is what we want. */
    opts[1].name = slang::CompilerOptionName::DisableWarning;
    opts[1].value.kind = slang::CompilerOptionValueKind::String;
    opts[1].value.stringValue0 = "41012";

    slang::TargetDesc td = {};
    td.format = k_formats[d.target];
    if (d.target == DE__SLANG_SPIRV)
        td.profile = s.global->findProfile("spirv_1_6");
    else if (d.target == DE__SLANG_DXIL)
        td.profile = s.global->findProfile(
            "sm_6_6"); // RT stages emit DXIL libraries (lib_6_6), not compute entry blobs.
    td.lineDirectiveMode = SLANG_LINE_DIRECTIVE_MODE_NONE;

    const char *search[] = {k_module_dir};
    import_fs fs(d);
    slang::SessionDesc sd = {};
    sd.targets = &td;
    sd.targetCount = 1;
    sd.defaultMatrixLayoutMode = SLANG_MATRIX_LAYOUT_COLUMN_MAJOR;
    sd.searchPaths = search;
    sd.searchPathCount = 1;
    sd.preprocessorMacros = macros.data();
    sd.preprocessorMacroCount = static_cast<SlangInt>(macros.size());
    sd.fileSystem = &fs;
    sd.compilerOptionEntries = opts;
    sd.compilerOptionEntryCount = 2;

    com_ref<slang::ISession> session;
    if (SLANG_FAILED(s.global->createSession(sd, session.put())) || session.p == nullptr) {
        set_diag(out, "cannot create a Slang session for target %s",
                 de__slang_target_name(d.target));
        return DE_ERR_INTERNAL;
    }

    /* The in-memory modules first: their global parameters get the first slots. */
    std::vector<slang::IComponentType *> parts;
    for (u32 i = 0; i < d.preload_count && i < d.module_count; ++i) {
        com_ref<ISlangBlob> diag;
        slang::IModule *m = session->loadModule(d.modules[i].name, diag.put());
        append_diag(out, diag.p);
        if (m == nullptr) {
            if (out->diagnostics[0] == '\0')
                set_diag(out, "cannot load the engine module \"%s\"", d.modules[i].name);
            return DE_ERR_PARSE;
        }
        parts.push_back(m);
    }

    std::string text(d.source, static_cast<size_t>(d.source_size));
    std::string module_name = module_name_of(d.path);
    slang::IModule *module = nullptr;
    {
        com_ref<ISlangBlob> diag;
        module = session->loadModuleFromSourceString(module_name.c_str(), d.path, text.c_str(),
                                                     diag.put());
        append_diag(out, diag.p);
    }
    if (module == nullptr) {
        if (out->diagnostics[0] == '\0')
            set_diag(out, "\"%s\" does not compile", d.path);
        return DE_ERR_PARSE;
    }
    parts.push_back(module);

    /* The entry points: as asked, or every [shader("...")] function of the source. */
    std::vector<com_ref<slang::IEntryPoint>> eps;
    std::vector<std::string> ep_names;
    if (d.entry_count == 0) {
        SlangInt32 n = module->getDefinedEntryPointCount();
        eps = std::vector<com_ref<slang::IEntryPoint>>(static_cast<size_t>(n));
        for (SlangInt32 i = 0; i < n; ++i) {
            if (SLANG_FAILED(module->getDefinedEntryPoint(i, eps[static_cast<size_t>(i)].put()))) {
                set_diag(out, "cannot read the entry points of \"%s\"", d.path);
                return DE_ERR_PARSE;
            }
            ep_names.emplace_back(); /* named from the layout below */
        }
        if (n == 0) {
            set_diag(out,
                     "\"%s\" has no [shader(\"...\")] entry point; mark one, or list them in the "
                     "`entry_points` setting",
                     d.path);
            return DE_ERR_PARSE;
        }
    } else {
        eps = std::vector<com_ref<slang::IEntryPoint>>(d.entry_count);
        for (u32 i = 0; i < d.entry_count; ++i) {
            const de__slang_entry &e = d.entries[i];
            com_ref<ISlangBlob> diag;
            SlangResult r = e.stage == DE__SHADER_STAGE_NONE
                                ? module->findEntryPointByName(e.name, eps[i].put())
                                : module->findAndCheckEntryPoint(e.name, to_slang_stage(e.stage),
                                                                 eps[i].put(), diag.put());
            if (SLANG_FAILED(r) || eps[i].p == nullptr) {
                set_diag(out,
                         "error: entry point \"%s\" not found in \"%s\" (without a stage in "
                         "`entry_points`, it needs a [shader(\"...\")] attribute)\n",
                         e.name, d.path);
                append_diag(out, diag.p);
                return DE_ERR_PARSE;
            }
            append_diag(out, diag.p);
            ep_names.emplace_back(e.name);
        }
    }

    const size_t count = eps.size();
    const de_allocator *a = d.allocator;
    out->alloc_ = a;
    out->entries = static_cast<de__slang_entry_out *>(DE_ALLOC(
        a, sizeof(de__slang_entry_out) * count, DE_ALIGNOF(de__slang_entry_out), DE_MEM_ASSET));
    out->entry_names =
        static_cast<char **>(DE_ALLOC(a, sizeof(char *) * count, DE_ALIGNOF(char *), DE_MEM_ASSET));
    if (out->entries == nullptr || out->entry_names == nullptr) {
        set_diag(out, "out of memory");
        return DE_ERR_OUT_OF_MEMORY;
    }
    std::memset(out->entries, 0, sizeof(de__slang_entry_out) * count);
    std::memset(out->entry_names, 0, sizeof(char *) * count);
    out->entry_count = static_cast<u32>(count);

    /* One program per entry point: its code and its layout are its own. */
    for (size_t i = 0; i < count; ++i) {
        std::vector<slang::IComponentType *> comps(parts);
        comps.push_back(eps[i].p);
        com_ref<slang::IComponentType> composite, linked;
        com_ref<ISlangBlob> diag1, diag2, diag3;
        SlangResult r = session->createCompositeComponentType(
            comps.data(), static_cast<SlangInt>(comps.size()), composite.put(), diag1.put());
        append_diag(out, diag1.p);
        if (SLANG_SUCCEEDED(r))
            r = composite->link(linked.put(), diag2.put());
        append_diag(out, diag2.p);
        com_ref<ISlangBlob> code;
        if (SLANG_SUCCEEDED(r))
            r = linked->getEntryPointCode(0, 0, code.put(), diag3.put());
        append_diag(out, diag3.p);
        if (SLANG_FAILED(r) || code.p == nullptr) {
            if (out->diagnostics[0] == '\0')
                set_diag(out, "entry point \"%s\" of \"%s\" does not compile", ep_names[i].c_str(),
                         d.path);
            return DE_ERR_PARSE;
        }

        com_ref<ISlangBlob> diag4;
        slang::ProgramLayout *layout = linked->getLayout(0, diag4.put());
        append_diag(out, diag4.p);
        SlangReflection *refl = reinterpret_cast<SlangReflection *>(layout);
        SlangReflectionEntryPoint *ep = refl != nullptr && s.fn.entry_point_count(refl) > 0
                                            ? s.fn.entry_point_by_index(refl, 0)
                                            : nullptr;
        if (ep == nullptr) {
            set_diag(out, "no reflection for entry point \"%s\" of \"%s\"", ep_names[i].c_str(),
                     d.path);
            return DE_ERR_INTERNAL;
        }
        if (ep_names[i].empty()) {
            const char *n = s.fn.entry_name(ep);
            ep_names[i] = n != nullptr ? n : "";
        }

        de__slang_entry_out &o = out->entries[i];
        o.stage = from_slang_stage(s.fn.entry_stage(ep));
        if (o.stage == DE__SHADER_STAGE_COMPUTE || o.stage == DE__SHADER_STAGE_MESH ||
            o.stage == DE__SHADER_STAGE_AMPLIFICATION) {
            SlangUInt tg[3] = {0, 0, 0};
            s.fn.entry_threadgroup(ep, 3, tg);
            for (int k = 0; k < 3; ++k)
                o.threadgroup[k] = tg[k] > 0xffffffffu ? 0xffffffffu : static_cast<u32>(tg[k]);
        }
        o.push_constant_bytes = push_constant_bytes(s.fn, refl, ep);

        size_t size = code->getBufferSize();
        o.code = DE_ALLOC(a, size != 0 ? size : 1u, DE__SHADER_CODE_ALIGN, DE_MEM_ASSET);
        if (o.code != nullptr)
            o.code_size = size;
        out->entry_names[i] = dup_str(a, ep_names[i].c_str());
        if (o.code == nullptr || out->entry_names[i] == nullptr) {
            set_diag(out, "out of memory");
            return DE_ERR_OUT_OF_MEMORY;
        }
        std::memcpy(o.code, code->getBufferPointer(), size);
    }
    return DE_OK;
}

} // namespace

/* ---- C interface ----------------------------------------------------------------------------- */

extern "C" {

const char *de__slang_target_name(de__slang_target t) {
    switch (t) {
    case DE__SLANG_SPIRV:
        return "spirv";
    case DE__SLANG_DXIL:
        return "dxil";
    case DE__SLANG_METALLIB:
        return "metallib";
    case DE__SLANG_METAL:
        return "metal";
    default:
        return "unknown";
    }
}

bool de__slang_available(char *msg, size_t cap) {
    slang_state *s = get();
    if (s == nullptr && msg != nullptr && cap != 0)
        std::snprintf(msg, cap, "%s", state().why);
    return s != nullptr;
}

bool de__slang_target_supported(de__slang_target t) {
    slang_state *s = get();
    return s != nullptr && static_cast<unsigned>(t) < DE__SLANG_TARGET_COUNT_ && s->target_ok[t];
}

const char *de__slang_version(void) {
    slang_state *s = get();
    return s != nullptr ? s->version : "";
}

de_result de__slang_compile(const de__slang_compile_desc *d, de__slang_compile_result *out) {
    if (out == nullptr)
        return DE_FAIL(DE_ERR_INVALID_ARGUMENT, "de__slang_compile: NULL result");
    std::memset(out, 0, sizeof *out);
    if (d == nullptr || d->allocator == nullptr || d->read == nullptr || d->path == nullptr ||
        (d->source == nullptr && d->source_size != 0) ||
        static_cast<unsigned>(d->target) >= DE__SLANG_TARGET_COUNT_ ||
        (d->entries == nullptr && d->entry_count != 0)) {
        set_diag(out, "de__slang_compile: invalid argument");
        return DE_ERR_INVALID_ARGUMENT;
    }
    slang_state *s = get();
    if (s == nullptr) {
        set_diag(out, "%s", state().why);
        return DE_ERR_UNSUPPORTED;
    }
    if (!s->target_ok[d->target]) {
        set_diag(out, "this host cannot compile Slang to %s", de__slang_target_name(d->target));
        return DE_ERR_UNSUPPORTED;
    }
    de_result r;
    try {
        std::lock_guard<std::mutex> guard(s->lock);
        r = compile_locked(*s, *d, out);
    } catch (const std::bad_alloc &) {
        set_diag(out, "out of memory");
        r = DE_ERR_OUT_OF_MEMORY;
    }
    if (r != DE_OK) {
        /* The errors only: Slang's warnings come first and would push them out of the
         * caller's message. */
        const char *first = std::strstr(out->diagnostics, "error");
        while (first != nullptr && first != out->diagnostics && first[-1] != '\n' &&
               first[-1] != ' ')
            first = std::strstr(first + 1, "error");
        if (first != nullptr && first != out->diagnostics)
            std::memmove(out->diagnostics, first, std::strlen(first) + 1u);
        char keep[sizeof out->diagnostics];
        std::memcpy(keep, out->diagnostics, sizeof keep);
        de__slang_compile_result_free(out);
        std::memcpy(out->diagnostics, keep, sizeof keep);
    }
    return r;
}

void de__slang_compile_result_free(de__slang_compile_result *r) {
    if (r == nullptr || r->alloc_ == nullptr) {
        if (r != nullptr)
            std::memset(r, 0, sizeof *r);
        return;
    }
    const de_allocator *a = r->alloc_;
    for (u32 i = 0; i < r->entry_count; ++i) {
        if (r->entries != nullptr && r->entries[i].code != nullptr)
            DE_FREE(a, r->entries[i].code,
                    r->entries[i].code_size != 0 ? static_cast<size_t>(r->entries[i].code_size)
                                                 : 1u);
        if (r->entry_names != nullptr && r->entry_names[i] != nullptr)
            DE_FREE(a, r->entry_names[i], std::strlen(r->entry_names[i]) + 1u);
    }
    if (r->entries != nullptr)
        DE_FREE(a, r->entries, sizeof(de__slang_entry_out) * r->entry_count);
    if (r->entry_names != nullptr)
        DE_FREE(a, r->entry_names, sizeof(char *) * r->entry_count);
    std::memset(r, 0, sizeof *r);
}

} /* extern "C" */
