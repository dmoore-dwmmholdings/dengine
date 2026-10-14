/* mtl_binary_archive.m — the persistent pipeline cache on Metal: one MTLBinaryArchive per device
 * (DENG-325, SPEC §12.1; Decision: Pipelines share fixed state on every backend).
 *
 * File: `<dir>/metal-<registryID>-<os build>.metallib`, `dir` from
 * de_gpu_device_desc.pipeline_cache_dir (NULL = "cache:/pipelines", "" = no cache). The registry
 * ID (16 hex digits) names the GPU, the OS build (kern.osversion, e.g. 25G83) the driver, so an
 * OS update starts a new file. A device without a registry ID uses its name (letters and digits).
 * The virtual path is resolved to a native one (de_fs_resolve): Metal reads and writes archives
 * by URL. When the root is not mounted there is no archive, and pipelines compile every run.
 *
 * At device creation an existing file is opened as the archive. A file Metal does not accept
 * (damaged, truncated, not an archive) is ignored with a warning, and a new, empty archive takes
 * its place. Pipeline creation (mtl_pipeline.m) first asks the archive for the pipeline
 * (MTLPipelineOptionFailOnBinaryArchiveMiss): a hit skips the compile. On a miss it compiles the
 * pipeline's functions into the archive (add*PipelineFunctionsWithDescriptor:) and takes the
 * pipeline from there. Additions run under a device lock. Mesh pipelines are archived from macOS
 * 15 on (addMeshRenderPipelineFunctionsWithDescriptor:).
 *
 * At device destroy, when pipelines were added, the archive is serialized to a temporary file in
 * the same directory and renamed over the old one, so a crash leaves the old file or the new one.
 * The log line says how many pipelines came from the archive and how long creation took.
 *
 * With Metal shader validation on (MTL_SHADER_VALIDATION) there is no archive: validation
 * instruments every pipeline, and instrumented pipelines never match archived ones.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define DEFAULT_DIR "cache:/pipelines"

static void keep_alnum(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (const char *p = in ? in : ""; *p && n + 1 < cap; ++p)
        if (isalnum((unsigned char)*p))
            out[n++] = *p;
    out[n] = '\0';
}

void de__mtl_archive_file_name(u64 registry_id, const char *device_name, const char *os_build,
                               char *out, size_t cap) {
    char build[64], name[128];
    keep_alnum(os_build, build, sizeof build);
    if (!build[0])
        snprintf(build, sizeof build, "unknown");
    if (registry_id != 0) {
        snprintf(out, cap, "metal-%016llx-%s.metallib", (unsigned long long)registry_id, build);
        return;
    }
    keep_alnum(device_name, name, sizeof name);
    snprintf(out, cap, "metal-%s-%s.metallib", name[0] ? name : "device", build);
}

static void os_build(char *buf, size_t cap) {
    size_t len = cap;
    if (sysctlbyname("kern.osversion", buf, &len, NULL, 0) != 0 || len == 0)
        buf[0] = '\0';
    buf[cap - 1] = '\0';
}

static id<MTLBinaryArchive> archive_of(mtl_device *m) {
    NSArray *list = (__bridge NSArray *)m->archive;
    return list.count ? list[0] : nil;
}

void de__mtl_archive_open(de_gpu_device *d, const de_gpu_device_desc *desc) {
    mtl_device *m = de__mtl_dev(d);
    m->archive_state = DE__MTL_ARCHIVE_NONE;
    m->archive_vpath[0] = m->archive_path[0] = '\0';
    const char *dir = desc->pipeline_cache_dir ? desc->pipeline_cache_dir : DEFAULT_DIR;
    if (!dir[0]) {
        DE_INFO(d->log, "metal: no persistent pipeline cache (pipeline_cache_dir is \"\")");
        return;
    }
    /* Shader validation instruments every pipeline, and an instrumented pipeline is never in an
     * archive: each creation would miss, compile into the archive and compile again. */
    const char *sv = getenv("MTL_SHADER_VALIDATION");
    if (sv && sv[0] && strcmp(sv, "0") != 0) {
        DE_INFO(d->log, "metal: no persistent pipeline cache while Metal shader validation is on "
                        "(MTL_SHADER_VALIDATION)");
        return;
    }
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
        char build[64], name[256];
        os_build(build, sizeof build);
        de__mtl_archive_file_name(dev.registryID, dev.name.UTF8String, build, name, sizeof name);
        const size_t dl = strlen(dir);
        const int n = snprintf(m->archive_vpath, sizeof m->archive_vpath, "%s%s%s", dir,
                               dl && dir[dl - 1] == '/' ? "" : "/", name);
        if (n < 0 || (size_t)n >= sizeof m->archive_vpath) {
            DE_WARN(d->log, "metal: pipeline cache directory \"%s\" is too long; no cache", dir);
            m->archive_vpath[0] = '\0';
            return;
        }
        if (de_fs_resolve(m->archive_vpath, m->archive_path, sizeof m->archive_path) != DE_OK) {
            DE_INFO(d->log, "metal: no persistent pipeline cache: \"%s\" does not resolve (%s)",
                    m->archive_vpath, de_error_last() ? de_error_last()->message : "not mounted");
            de_error_clear(); /* an expected outcome: not the caller's error */
            m->archive_vpath[0] = m->archive_path[0] = '\0';
            return;
        }
        MTLBinaryArchiveDescriptor *bd = [MTLBinaryArchiveDescriptor new];
        id<MTLBinaryArchive> archive = nil;
        u32 state = DE__MTL_ARCHIVE_EMPTY;
        if (de_path_exists(m->archive_vpath)) {
            bd.url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:m->archive_path]];
            NSError *err = nil;
            archive = [dev newBinaryArchiveWithDescriptor:bd error:&err];
            if (archive != nil) {
                state = DE__MTL_ARCHIVE_LOADED;
            } else {
                DE_WARN(d->log,
                        "metal: pipeline cache \"%s\" is ignored (%s); a new one replaces it",
                        m->archive_vpath,
                        err ? err.localizedDescription.UTF8String : "Metal did not load it");
                state = DE__MTL_ARCHIVE_REJECTED;
                bd.url = nil;
            }
        }
        if (archive == nil) {
            NSError *err = nil;
            archive = [dev newBinaryArchiveWithDescriptor:bd error:&err];
            if (archive == nil) {
                DE_WARN(d->log, "metal: no pipeline cache: a new binary archive failed (%s)",
                        err ? err.localizedDescription.UTF8String : "no reason given");
                return;
            }
        }
        archive.label = @"de pipeline cache";
        m->archive = (void *)CFBridgingRetain(@[ archive ]);
        m->archive_state = state;
        DE_INFO(d->log, "metal: pipeline cache \"%s\" %s", m->archive_vpath,
                state == DE__MTL_ARCHIVE_LOADED ? "loaded" : "started empty");
    }
}

void *de__mtl_archive_list(de_gpu_device *d) {
    return de__mtl_dev(d)->archive;
}

void de__mtl_archive_hit(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->archive_lock);
    ++m->archive_hits;
    de_mutex_unlock(&m->archive_lock);
}

bool de__mtl_archive_add(de_gpu_device *d, void *descriptor) {
    mtl_device *m = de__mtl_dev(d);
    id<MTLBinaryArchive> archive = archive_of(m);
    if (archive == nil)
        return false;
    id desc = (__bridge id)descriptor;
    NSError *err = nil;
    BOOL ok = NO;
    de_mutex_lock(&m->archive_lock);
    ++m->archive_misses;
    if ([desc isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
        ok = [archive addRenderPipelineFunctionsWithDescriptor:desc error:&err];
    } else if ([desc isKindOfClass:[MTLComputePipelineDescriptor class]]) {
        ok = [archive addComputePipelineFunctionsWithDescriptor:desc error:&err];
    } else {
#if defined(__MAC_15_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0
        if (@available(macOS 15.0, *))
            if ([desc isKindOfClass:[MTLMeshRenderPipelineDescriptor class]])
                ok = [archive addMeshRenderPipelineFunctionsWithDescriptor:desc error:&err];
#endif
    }
    if (ok)
        m->archive_dirty = true;
    de_mutex_unlock(&m->archive_lock);
    if (!ok && err != nil)
        DE_WARN(d->log, "metal: pipeline cache: a pipeline was not added (%s)",
                err.localizedDescription.UTF8String);
    return ok;
}

void de__mtl_archive_save(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    const double ms = (double)m->pipeline_ns / 1e6;
    if (m->archive_hits + m->archive_misses > 0)
        DE_INFO(d->log,
                "metal: pipeline creation took %.3f ms: %u pipelines from the binary archive, "
                "%u compiled",
                ms, m->archive_hits, m->archive_misses);
    id<MTLBinaryArchive> archive = archive_of(m);
    if (archive == nil || !m->archive_dirty)
        return;
    /* The directory: the virtual path without its file name. */
    char dir[sizeof m->archive_vpath];
    snprintf(dir, sizeof dir, "%s", m->archive_vpath);
    char *slash = strrchr(dir, '/');
    if (slash && slash > dir && slash[-1] != '/' && slash[-1] != ':')
        *slash = '\0';
    else if (slash)
        slash[1] = '\0';
    (void)de_dir_create(dir);
    de_error_clear();
    char tmp[sizeof m->archive_path + 32];
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", m->archive_path, (int)getpid());
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:tmp]];
        NSError *err = nil;
        if (![archive serializeToURL:url error:&err]) {
            DE_WARN(d->log, "metal: pipeline cache \"%s\" not saved: %s", m->archive_vpath,
                    err ? err.localizedDescription.UTF8String : "no reason given");
            (void)unlink(tmp);
            return;
        }
    }
    if (rename(tmp, m->archive_path) != 0) {
        DE_WARN(d->log, "metal: pipeline cache \"%s\" not saved: rename failed", m->archive_vpath);
        (void)unlink(tmp);
        return;
    }
    m->archive_dirty = false;
    DE_INFO(d->log, "metal: pipeline cache \"%s\" saved (%u pipelines added)", m->archive_vpath,
            m->archive_misses);
}

void de__mtl_archive_close(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    if (m->archive) {
        @autoreleasepool {
            (void)CFBridgingRelease(m->archive);
        }
    }
    m->archive = NULL;
}

u32 de__mtl_archive_state(de_gpu_device *d, u32 *hits, u32 *misses, const char **path) {
    if (!de__mtl_device_object(d)) {
        if (hits)
            *hits = 0;
        if (misses)
            *misses = 0;
        if (path)
            *path = "";
        return DE__MTL_ARCHIVE_NONE;
    }
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->archive_lock);
    if (hits)
        *hits = m->archive_hits;
    if (misses)
        *misses = m->archive_misses;
    de_mutex_unlock(&m->archive_lock);
    if (path)
        *path = m->archive_vpath;
    return m->archive_state;
}
