/* vk_sync.c — Vulkan submission and timeline fences (DENG-284; SPEC §12.1).
 *
 * Fences: a de_gpu_fence is a timeline semaphore (VK_SEMAPHORE_TYPE_TIMELINE) starting at its
 * initial value. de_gpu_fence_value reads its counter; de_gpu_fence_wait is vkWaitSemaphores
 * (timeout 0 polls). After a device loss every fence counts as reached (UINT64_MAX) and waits
 * return DE_ERR_DEVICE_LOST.
 *
 * Submit: one vkQueueSubmit2 per de_gpu_queue_submit, under the VkQueue's mutex:
 *   - each command list gets a barrier that makes its writes visible to the host (READBACK
 *     buffers) and is ended (vkEndCommandBuffer), in order;
 *   - each de_gpu_wait waits on its fence's semaphore at its value, at ALL_COMMANDS;
 *   - on the graphics queue, each pending swapchain acquire (vk_swapchain.c) is waited on too,
 *     at ALL_COMMANDS, so barriers whose `before` is NONE or PRESENT chain after it;
 *   - it signals the queue's internal timeline at `queue_value` (deferred destruction, command
 *     list recycling) and, when set, `signal` at `signal_value`.
 * The front-end has published `queue_value` before the call, so the value must complete even
 * when the submit fails: a failure (or VK_ERROR_DEVICE_LOST) marks the device lost, and a lost
 * device reports every value as complete. A command buffer that cannot be ended is left out and
 * the submit is reported failed after the timeline signal went in.
 */
#include "gpu/vulkan/vk_internal.h"

#include <string.h>

bool de__vk_lost(de__vk_device *vk) {
    return de_atomic_load_u32(&vk->lost, DE_ATOMIC_ACQUIRE) != 0;
}

static void mark_lost(de__vk_device *vk) {
    de_atomic_store_u32(&vk->lost, 1u, DE_ATOMIC_RELEASE);
}

static const char *queue_name(de_gpu_queue q) {
    return q == DE_QUEUE_GRAPHICS ? "graphics" : q == DE_QUEUE_COMPUTE ? "compute" : "copy";
}

static VkSemaphore make_semaphore(de_gpu_device *d, VkSemaphoreType type, u64 initial,
                                  const char *what) {
    de__vk_device *vk = de__vk(d);
    VkSemaphoreTypeCreateInfo ti;
    memset(&ti, 0, sizeof ti);
    ti.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    ti.semaphoreType = type;
    ti.initialValue = initial;
    VkSemaphoreCreateInfo si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    si.pNext = &ti;
    VkSemaphore s = VK_NULL_HANDLE;
    VkResult vr = vk->fn.vkCreateSemaphore(vk->device, &si, NULL, &s);
    if (vr != VK_SUCCESS) {
        de__vk_fail(d, vr, "vulkan: vkCreateSemaphore (%s)", what);
        return VK_NULL_HANDLE;
    }
    return s;
}

VkSemaphore de__vk_binary_semaphore(de_gpu_device *d, const char *what) {
    return make_semaphore(d, VK_SEMAPHORE_TYPE_BINARY, 0, what);
}

bool de__vk_wait_queue_value(de__vk_device *vk, de_gpu_queue q, u64 value) {
    if (value == 0)
        return true;
    if (de__vk_lost(vk))
        return false;
    VkSemaphoreWaitInfo wi;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &vk->queues[q].timeline;
    wi.pValues = &value;
    VkResult vr = vk->fn.vkWaitSemaphores(vk->device, &wi, UINT64_MAX);
    if (vr == VK_ERROR_DEVICE_LOST)
        mark_lost(vk);
    return vr == VK_SUCCESS;
}

/* ---------- Fences ---------- */

de_result de__vk_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial) {
    de__vk_fence *vf = DE__GPU_PAYLOAD(de__vk_fence, f);
    vf->semaphore = make_semaphore(d, VK_SEMAPHORE_TYPE_TIMELINE, initial, "fence");
    return vf->semaphore ? DE_OK : DE_ERR_OUT_OF_MEMORY;
}

void de__vk_fence_destroy(de_gpu_device *d, de__gpu_fence *f) {
    de__vk_device *vk = de__vk(d);
    de__vk_fence *vf = DE__GPU_PAYLOAD(de__vk_fence, f);
    if (vf->semaphore)
        vk->fn.vkDestroySemaphore(vk->device, vf->semaphore, NULL);
    vf->semaphore = VK_NULL_HANDLE;
}

u64 de__vk_fence_value(de_gpu_device *d, de__gpu_fence *f) {
    de__vk_device *vk = de__vk(d);
    if (de__vk_lost(vk))
        return UINT64_MAX;
    u64 value = 0;
    VkResult vr = vk->fn.vkGetSemaphoreCounterValue(
        vk->device, DE__GPU_PAYLOAD(de__vk_fence, f)->semaphore, &value);
    if (vr != VK_SUCCESS) {
        mark_lost(vk);
        return UINT64_MAX;
    }
    return value;
}

de_result de__vk_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns) {
    de__vk_device *vk = de__vk(d);
    if (de__vk_lost(vk))
        return de__gpu_fail(d, DE_ERR_DEVICE_LOST, "de_gpu_fence_wait: the Vulkan device is lost");
    VkSemaphore s = DE__GPU_PAYLOAD(de__vk_fence, f)->semaphore;
    VkSemaphoreWaitInfo wi;
    memset(&wi, 0, sizeof wi);
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &s;
    wi.pValues = &value;
    VkResult vr = vk->fn.vkWaitSemaphores(vk->device, &wi, timeout_ns);
    if (vr == VK_SUCCESS)
        return DE_OK;
    if (vr == VK_TIMEOUT) {
        u64 now = 0;
        vk->fn.vkGetSemaphoreCounterValue(vk->device, s, &now);
        /* An expected outcome (polling): the error context only, no log line. */
        return DE_FAIL(DE_ERR_TIMEOUT, "de_gpu_fence_wait: fence is at %llu, not %llu",
                       (unsigned long long)now, (unsigned long long)value);
    }
    return de__vk_fail(d, vr, "de_gpu_fence_wait: vkWaitSemaphores");
}

/* ---------- Submit ---------- */

/* Makes every write of the list available to the host. A fence or semaphore wait on the CPU
 * covers device accesses only; READBACK buffers also need a barrier to the HOST stage (D3D12 and
 * Metal do this implicitly). One global barrier at the end of each list: cheap, and de_gpu_access
 * has no host access for the caller to name. */
static void host_visibility_barrier(de__vk_device *vk, VkCommandBuffer cb) {
    VkMemoryBarrier2 mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo di;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vk->fn.vkCmdPipelineBarrier2(cb, &di);
}

/* Small submits use the stack; larger ones (rare) the device allocator. */
#define LOCAL 16u

de_result de__vk_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                              u64 queue_value) {
    de__vk_device *vk = de__vk(d);
    de__vk_queue *vq = &vk->queues[q];

    VkCommandBufferSubmitInfo cb_local[LOCAL];
    VkSemaphoreSubmitInfo wait_local[LOCAL + DE__VK_MAX_PENDING_ACQUIRES];
    VkCommandBufferSubmitInfo *cbs = cb_local;
    VkSemaphoreSubmitInfo *waits = wait_local;
    const u32 wait_cap = s->wait_count + DE__VK_MAX_PENDING_ACQUIRES;
    const size_t cb_bytes = (size_t)s->cmd_count * sizeof *cbs;
    const size_t wait_bytes = (size_t)wait_cap * sizeof *waits;
    if (s->cmd_count > LOCAL)
        cbs = (VkCommandBufferSubmitInfo *)DE_ALLOC(d->alloc, cb_bytes, 8, DE_MEM_RENDER);
    if (wait_cap > LOCAL + DE__VK_MAX_PENDING_ACQUIRES)
        waits = (VkSemaphoreSubmitInfo *)DE_ALLOC(d->alloc, wait_bytes, 8, DE_MEM_RENDER);
    de_result r = DE_OK;
    if (!cbs || !waits) {
        /* Still signal the value below (with no work), then report. */
        r = DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_queue_submit: submit info");
    }

    /* End the command buffers (outside the queue lock: each list belongs to this caller). */
    u32 ncb = 0;
    for (u32 i = 0; r == DE_OK && i < s->cmd_count; ++i) {
        de__vk_cmd *vc = de__vk_cmd_of(s->cmds[i]);
        host_visibility_barrier(vk, vc->cb);
        VkResult vr = vk->fn.vkEndCommandBuffer(vc->cb);
        if (vr != VK_SUCCESS) {
            r = de__vk_fail(d, vr, "de_gpu_queue_submit: vkEndCommandBuffer(\"%s\")",
                            s->cmds[i]->name);
            ncb = 0;
            break;
        }
        memset(&cbs[ncb], 0, sizeof cbs[ncb]);
        cbs[ncb].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbs[ncb].commandBuffer = vc->cb;
        ++ncb;
    }

    u32 nwait = 0;
    for (u32 i = 0; r == DE_OK && i < s->wait_count; ++i) {
        de__gpu_fence *f = de__gpu_get_fence(d, s->waits[i].fence);
        if (!f)
            continue; /* the front-end reported it */
        memset(&waits[nwait], 0, sizeof waits[nwait]);
        waits[nwait].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[nwait].semaphore = DE__GPU_PAYLOAD(de__vk_fence, f)->semaphore;
        waits[nwait].value = s->waits[i].value;
        waits[nwait].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++nwait;
    }

    VkSemaphoreSubmitInfo sig[2];
    memset(sig, 0, sizeof sig);
    u32 nsig = 0;
    sig[nsig].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    sig[nsig].semaphore = vq->timeline;
    sig[nsig].value = queue_value;
    sig[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ++nsig;
    if (r == DE_OK && s->signal.id) {
        de__gpu_fence *f = de__gpu_get_fence(d, s->signal);
        if (f) {
            sig[nsig].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            sig[nsig].semaphore = DE__GPU_PAYLOAD(de__vk_fence, f)->semaphore;
            sig[nsig].value = s->signal_value;
            sig[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++nsig;
        }
    }

    VkResult vr = VK_ERROR_DEVICE_LOST;
    de_mutex_lock(&vk->queue_locks[vq->lock]);
    /* Swapchain acquires wait for the next graphics submit (guarded by the graphics lock). */
    const bool takes_acquires = r == DE_OK && q == DE_QUEUE_GRAPHICS;
    u32 nacq = 0;
    if (takes_acquires) {
        for (u32 i = 0; i < vk->pending_acquire_count; ++i) {
            memset(&waits[nwait], 0, sizeof waits[nwait]);
            waits[nwait].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[nwait].semaphore = vk->pending_acquire[i]->semaphore;
            waits[nwait].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++nwait;
            ++nacq;
        }
    }
    if (!de__vk_lost(vk)) {
        VkSubmitInfo2 si;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        si.waitSemaphoreInfoCount = nwait;
        si.pWaitSemaphoreInfos = nwait ? waits : NULL;
        si.commandBufferInfoCount = ncb;
        si.pCommandBufferInfos = ncb ? cbs : NULL;
        si.signalSemaphoreInfoCount = nsig;
        si.pSignalSemaphoreInfos = sig;
        vr = vk->fn.vkQueueSubmit2(vq->queue, 1, &si, VK_NULL_HANDLE);
    }
    if (vr == VK_SUCCESS) {
        vk->last_value[q] = queue_value;
        for (u32 i = 0; i < nacq; ++i) {
            vk->pending_acquire[i]->pending = false;
            vk->pending_acquire[i]->consumed_value = queue_value;
        }
        if (nacq)
            vk->pending_acquire_count = 0;
    }
    de_mutex_unlock(&vk->queue_locks[vq->lock]);

    if (cbs != cb_local && cbs)
        DE_FREE(d->alloc, cbs, cb_bytes);
    if (waits != wait_local && waits)
        DE_FREE(d->alloc, waits, wait_bytes);
    if (vr != VK_SUCCESS) {
        /* The value can no longer be signalled in order: report every value as complete. */
        mark_lost(vk);
        if (r == DE_OK)
            r = de__vk_fail(d, vr, "vulkan: vkQueueSubmit2 on the %s queue", queue_name(q));
    }
    return r;
}
