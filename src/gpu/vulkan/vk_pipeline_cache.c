/* vk_pipeline_cache.c — the persistent Vulkan pipeline cache (DENG-323; SPEC §12.1).
 *
 * One VkPipelineCache per device, used by every pipeline creation. At device creation it starts
 * from the file `<dir>/vulkan-<vendor>-<device>-<driverVersion>.bin` (hex IDs; `dir` from
 * de_gpu_device_desc.pipeline_cache_dir, NULL = "cache:/pipelines", "" = no file) and at device
 * destroy its data is written back atomically (de_file_write_all_atomic), so a crash leaves the
 * old file or the new one. A device that created no pipeline leaves the file alone.
 *
 * File: a 32-byte de__vk_pipeline_cache_file header (magic "DVPC", version, data size, XXH3 of the
 * data, vendor and device IDs), then the vkGetPipelineCacheData bytes, which start with
 * VkPipelineCacheHeaderVersionOne. Before the data reaches the driver both are checked: our size
 * and checksum (a truncated or damaged file), then the Vulkan header against this device (header
 * size and version, vendor, device, pipelineCacheUUID). A file that fails is ignored with a
 * warning and the cache starts empty; it is replaced at device destroy. A missing file is normal
 * (first run, new driver) and only logged at info level. The driver keys by driverVersion too,
 * through pipelineCacheUUID, and the name has it, so a driver update starts a new file.
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_DIR "cache:/pipelines"

const void *de__vk_pipeline_cache_check(const void *file, u64 size,
                                        const VkPhysicalDeviceProperties *props, u64 *data_size,
                                        char *why, size_t why_cap) {
    *data_size = 0;
    de__vk_pipeline_cache_file h;
    if (!file || size < sizeof h) {
        snprintf(why, why_cap, "%llu bytes is shorter than the %u-byte file header",
                 (unsigned long long)size, (unsigned)sizeof h);
        return NULL;
    }
    memcpy(&h, file, sizeof h);
    if (h.magic != DE__VK_PIPELINE_CACHE_MAGIC || h.version != DE__VK_PIPELINE_CACHE_VERSION) {
        snprintf(why, why_cap, "not a DEngine Vulkan pipeline cache (magic 0x%08x, version %u)",
                 h.magic, h.version);
        return NULL;
    }
    if (h.data_size != size - sizeof h) {
        snprintf(why, why_cap, "truncated: the header says %llu data bytes, the file has %llu",
                 (unsigned long long)h.data_size, (unsigned long long)(size - sizeof h));
        return NULL;
    }
    const u8 *data = (const u8 *)file + sizeof h;
    if (de_hash64(data, (size_t)h.data_size, DE__VK_PIPELINE_CACHE_MAGIC) != h.data_hash) {
        snprintf(why, why_cap, "checksum mismatch: the data is damaged");
        return NULL;
    }
    if (h.vendor_id != props->vendorID || h.device_id != props->deviceID) {
        snprintf(why, why_cap, "made for vendor 0x%x device 0x%x, not this device", h.vendor_id,
                 h.device_id);
        return NULL;
    }
    VkPipelineCacheHeaderVersionOne vh;
    if (h.data_size < sizeof vh) {
        snprintf(why, why_cap, "the Vulkan cache data (%llu bytes) has no header",
                 (unsigned long long)h.data_size);
        return NULL;
    }
    memcpy(&vh, data, sizeof vh);
    if (vh.headerSize < sizeof vh || vh.headerSize > h.data_size ||
        vh.headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) {
        snprintf(why, why_cap, "bad Vulkan cache header (size %u, version %d)", vh.headerSize,
                 (int)vh.headerVersion);
        return NULL;
    }
    if (vh.vendorID != props->vendorID || vh.deviceID != props->deviceID ||
        memcmp(vh.pipelineCacheUUID, props->pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        snprintf(why, why_cap,
                 "the Vulkan cache header is for another device or driver (vendor 0x%x device "
                 "0x%x, or another pipelineCacheUUID)",
                 vh.vendorID, vh.deviceID);
        return NULL;
    }
    *data_size = h.data_size;
    return data;
}

static de_result create_cache(de_gpu_device *d, const void *data, u64 size) {
    de__vk_device *vk = de__vk(d);
    VkPipelineCacheCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = (size_t)size;
    ci.pInitialData = size ? data : NULL;
    VkResult vr = vk->fn.vkCreatePipelineCache(vk->device, &ci, NULL, &vk->pipeline_cache);
    if (vr != VK_SUCCESS) {
        vk->pipeline_cache = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "vulkan: vkCreatePipelineCache (%llu initial bytes)",
                           (unsigned long long)size);
    }
    return DE_OK;
}

de_result de__vk_pipeline_cache_init(de_gpu_device *d, const char *dir) {
    de__vk_device *vk = de__vk(d);
    const VkPhysicalDeviceProperties *props = &vk->phys.props.properties;
    vk->pipeline_cache_state = DE__VK_PIPELINE_CACHE_OFF;
    vk->pipeline_cache_loaded = 0;
    de_atomic_init_u32(&vk->pipelines_created, 0);
    vk->pipeline_cache_path[0] = '\0';
    if (!dir)
        dir = DEFAULT_DIR;
    if (!dir[0])
        return create_cache(d, NULL, 0);

    int w = snprintf(vk->pipeline_cache_path, sizeof vk->pipeline_cache_path,
                     "%s/vulkan-%04x-%08x-%08x.bin", dir, props->vendorID, props->deviceID,
                     props->driverVersion);
    if (w < 0 || (size_t)w >= sizeof vk->pipeline_cache_path) {
        DE_WARN(d->log, "vulkan: pipeline cache directory \"%s\" is too long; no persistent cache",
                dir);
        vk->pipeline_cache_path[0] = '\0';
        return create_cache(d, NULL, 0);
    }

    void *file = NULL;
    u64 size = 0;
    de_result r = de_file_read_all(vk->pipeline_cache_path, d->alloc, &file, &size);
    if (r != DE_OK) {
        de_error_clear(); /* a missing file is the normal first run */
        vk->pipeline_cache_state = DE__VK_PIPELINE_CACHE_EMPTY;
        DE_INFO(d->log, "vulkan: no pipeline cache at %s (%s); starting empty",
                vk->pipeline_cache_path, de_result_str(r));
        return create_cache(d, NULL, 0);
    }
    char why[200];
    u64 data_size = 0;
    const void *data = de__vk_pipeline_cache_check(file, size, props, &data_size, why, sizeof why);
    if (data) {
        r = create_cache(d, data, data_size);
        if (r == DE_OK) {
            vk->pipeline_cache_state = DE__VK_PIPELINE_CACHE_LOADED;
            vk->pipeline_cache_loaded = data_size;
            DE_INFO(d->log, "vulkan: pipeline cache %s loaded (%llu bytes)",
                    vk->pipeline_cache_path, (unsigned long long)data_size);
        } else {
            /* The driver refused data that passed our checks: start empty. */
            DE_WARN(d->log, "vulkan: the driver refused pipeline cache %s; ignored",
                    vk->pipeline_cache_path);
            de_error_clear();
            data = NULL;
        }
    } else {
        DE_WARN(d->log, "vulkan: pipeline cache %s ignored: %s", vk->pipeline_cache_path, why);
    }
    DE_FREE(d->alloc, file, (size_t)size + 1u);
    if (!data) {
        vk->pipeline_cache_state = DE__VK_PIPELINE_CACHE_REJECTED;
        return create_cache(d, NULL, 0);
    }
    return DE_OK;
}

/* Writes the cache file: header + vkGetPipelineCacheData. */
static void save(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    const VkPhysicalDeviceProperties *props = &vk->phys.props.properties;
    size_t size = 0;
    VkResult vr = vk->fn.vkGetPipelineCacheData(vk->device, vk->pipeline_cache, &size, NULL);
    if (vr != VK_SUCCESS || size == 0) {
        if (vr != VK_SUCCESS)
            DE_WARN(d->log, "vulkan: pipeline cache not saved: vkGetPipelineCacheData: %s",
                    de__vk_result_name(vr));
        return;
    }
    de__vk_pipeline_cache_file h;
    const size_t total = sizeof h + size;
    u8 *buf = (u8 *)DE_ALLOC(d->alloc, total, 16, DE_MEM_RENDER);
    if (!buf) {
        DE_WARN(d->log, "vulkan: pipeline cache not saved: out of memory (%zu bytes)", total);
        return;
    }
    vr = vk->fn.vkGetPipelineCacheData(vk->device, vk->pipeline_cache, &size, buf + sizeof h);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        DE_WARN(d->log, "vulkan: pipeline cache not saved: vkGetPipelineCacheData: %s",
                de__vk_result_name(vr));
        DE_FREE(d->alloc, buf, total);
        return;
    }
    memset(&h, 0, sizeof h);
    h.magic = DE__VK_PIPELINE_CACHE_MAGIC;
    h.version = DE__VK_PIPELINE_CACHE_VERSION;
    h.data_size = size;
    h.data_hash = de_hash64(buf + sizeof h, size, DE__VK_PIPELINE_CACHE_MAGIC);
    h.vendor_id = props->vendorID;
    h.device_id = props->deviceID;
    memcpy(buf, &h, sizeof h);

    /* The directory: the path without its last component. */
    char dir[sizeof vk->pipeline_cache_path];
    snprintf(dir, sizeof dir, "%s", vk->pipeline_cache_path);
    char *slash = strrchr(dir, '/');
    if (slash && slash > dir && slash[-1] != ':' && slash[-1] != '/')
        *slash = '\0';
    else
        dir[0] = '\0';
    de_result r = dir[0] ? de_dir_create(dir) : DE_OK;
    if (r == DE_OK)
        r = de_file_write_all_atomic(vk->pipeline_cache_path, buf, sizeof h + size);
    if (r == DE_OK) {
        vk->pipeline_cache_saved = size;
        DE_INFO(d->log, "vulkan: pipeline cache %s saved (%zu bytes)", vk->pipeline_cache_path,
                size);
    } else {
        DE_WARN(d->log, "vulkan: pipeline cache %s not saved: %s", vk->pipeline_cache_path,
                de_result_str(r));
        de_error_clear();
    }
    DE_FREE(d->alloc, buf, total);
}

void de__vk_pipeline_cache_shutdown(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    if (!vk->pipeline_cache || !vk->device)
        return;
    /* Only when this run created pipelines: a device that made none has nothing new to keep. */
    if (vk->pipeline_cache_path[0] && !de_atomic_load_u32(&vk->lost, DE_ATOMIC_ACQUIRE) &&
        de_atomic_load_u32(&vk->pipelines_created, DE_ATOMIC_RELAXED) > 0)
        save(d);
    vk->fn.vkDestroyPipelineCache(vk->device, vk->pipeline_cache, NULL);
    vk->pipeline_cache = VK_NULL_HANDLE;
}

u32 de__gpu_vk_pipeline_cache_state(de_gpu_device *d, u64 *loaded_bytes, const char **path) {
    const bool ok = d && d->vt == &de__gpu_vulkan_vtable;
    if (loaded_bytes)
        *loaded_bytes = ok ? de__vk(d)->pipeline_cache_loaded : 0u;
    if (path)
        *path = ok ? de__vk(d)->pipeline_cache_path : "";
    return ok ? de__vk(d)->pipeline_cache_state : (u32)DE__VK_PIPELINE_CACHE_OFF;
}
