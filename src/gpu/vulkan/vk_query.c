/* vk_query.c — Vulkan timestamp queries (DENG-326; SPEC §12.1, §12.3).
 *
 * A de_gpu_query_heap is one VkQueryPool of VK_QUERY_TYPE_TIMESTAMP. A timestamp resets its
 * query and writes it at VK_PIPELINE_STAGE_2_ALL_COMMANDS (the GPU time once every earlier
 * command has finished), so a query can be written again in any later list without a separate
 * reset call. The front-end refuses timestamps inside render passes, where vkCmdResetQueryPool is
 * not allowed. Ticks are timestampPeriod nanoseconds (caps.timestamp_frequency = 1e9 / period);
 * a queue family whose timestampValidBits is 0 cannot write them (the copy family on some
 * devices), which heap creation reports for the heap's queue. vkCmdResetQueryPool and
 * vkCmdCopyQueryPoolResults need a graphics or compute family, so a copy-queue heap is refused
 * (DE_ERR_UNSUPPORTED) when the copy queue is on a transfer-only family, as on most desktop GPUs:
 * time copies from the graphics or compute queue there.
 *
 * Resolve: vkCmdCopyQueryPoolResults as 64-bit values with WAIT, a transfer write into the
 * destination buffer, ordered by the caller's barriers like any copy (vk_sync.c makes READBACK
 * buffers visible to the host at the end of each submission).
 */
#include "gpu/vulkan/vk_internal.h"

#include <string.h>

#define MAX_FAMILIES 32u

static de__vk_query_heap *payload(de__gpu_query_heap *h) {
    return DE__GPU_PAYLOAD(de__vk_query_heap, h);
}

static const char *queue_name(de_gpu_queue q) {
    return q == DE_QUEUE_GRAPHICS ? "graphics" : q == DE_QUEUE_COMPUTE ? "compute" : "copy";
}

de_result de__vk_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                   const de_gpu_query_heap_desc *desc) {
    de__vk_device *vk = de__vk(d);
    de__vk_query_heap *qh = payload(h);

    VkQueueFamilyProperties props[MAX_FAMILIES];
    u32 n = MAX_FAMILIES;
    vk->fn.vkGetPhysicalDeviceQueueFamilyProperties(vk->phys.handle, &n, props);
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        const u32 fam = vk->queues[q].family;
        qh->valid_bits[q] = fam < n ? props[fam].timestampValidBits : 0u;
    }
    const u32 fam = vk->queues[desc->queue].family;
    const VkQueueFlags flags = fam < n ? props[fam].queueFlags : 0u;
    if (!(flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_query_heap_create(\"%s\"): vulkan: queue family %u of the %s "
                            "queue is transfer-only: it cannot reset or resolve queries",
                            h->name, fam, queue_name(desc->queue));
    if (qh->valid_bits[desc->queue] == 0)
        return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "de_gpu_query_heap_create(\"%s\"): vulkan: queue family %u of the %s "
                            "queue has no timestamps (timestampValidBits 0)",
                            h->name, vk->queues[desc->queue].family, queue_name(desc->queue));

    VkQueryPoolCreateInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = h->count;
    VkResult vr = vk->fn.vkCreateQueryPool(vk->device, &ci, NULL, &qh->pool);
    if (vr != VK_SUCCESS) {
        qh->pool = VK_NULL_HANDLE;
        return de__vk_fail(d, vr, "de_gpu_query_heap_create(\"%s\"): vkCreateQueryPool (%u)",
                           h->name, h->count);
    }
    return DE_OK;
}

void de__vk_query_heap_destroy(de_gpu_device *d, de__gpu_query_heap *h) {
    de__vk_device *vk = de__vk(d);
    de__vk_query_heap *qh = payload(h);
    if (qh->pool)
        vk->fn.vkDestroyQueryPool(vk->device, qh->pool, NULL);
    qh->pool = VK_NULL_HANDLE;
}

void de__vk_cmd_timestamp(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index) {
    de__vk_device *vk = de__vk(c->device);
    de__vk_query_heap *qh = payload(h);
    if (qh->valid_bits[c->queue] == 0) {
        de__gpu_fail(c->device, DE_ERR_UNSUPPORTED,
                     "de_gpu_cmd_timestamp(\"%s\"): vulkan: the %s queue's family has no "
                     "timestamps",
                     c->name, queue_name(c->queue));
        return;
    }
    VkCommandBuffer cb = de__vk_cmd_of(c)->cb;
    vk->fn.vkCmdResetQueryPool(cb, qh->pool, index, 1);
    vk->fn.vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, qh->pool, index);
}

void de__vk_cmd_resolve_queries(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                de__gpu_buffer *dst, u64 dst_off) {
    de__vk_device *vk = de__vk(c->device);
    vk->fn.vkCmdCopyQueryPoolResults(de__vk_cmd_of(c)->cb, payload(h)->pool, first, count,
                                     DE__GPU_PAYLOAD(de__vk_buffer, dst)->buffer, dst_off,
                                     sizeof(u64),
                                     VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
}
