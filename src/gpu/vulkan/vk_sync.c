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
 *
 * Deferred submits (DENG-426). de_gpu lets a wait be submitted before its signal. Vulkan runs a
 * VkQueue's submissions in order, so when de_gpu queues share one VkQueue (lavapipe has a single
 * queue: graphics, compute and copy all map to it) a wait whose signal is submitted later to the
 * same VkQueue would block it forever. On a shared VkQueue, a submit with a wait whose value no
 * submission has been asked to signal yet (de__vk_fence.submitted; fences are only signalled by
 * submits) is held back, as are the later submits of the same de_gpu queue (each queue's internal
 * timeline must be signalled in order). Every successful submit that signals a fence then
 * re-checks the held-back submits of every queue lock and hands over those whose waits are now
 * covered, in order. On distinct VkQueues nothing is held back: Vulkan's wait-before-signal works
 * there. Swapchain present (vk_swapchain.c) does not go through this: it must not follow a
 * graphics submit that is still held back.
 */
#include "gpu/vulkan/vk_internal.h"

#include <dengine/de_platform.h>
#include <stdio.h>
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
    de_atomic_init_u64(&vf->submitted, initial);
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

/* ---------- Deferred submits (DENG-426; file comment) ---------- */

typedef struct de__vk_deferred {
    struct de__vk_deferred *next;
    de_gpu_queue q;
    u32 ncb, nwait, nsig;
    size_t bytes; /**< of this allocation (the arrays follow the struct) */
    VkCommandBufferSubmitInfo *cbs;
    VkSemaphoreSubmitInfo *waits, *sigs;
    de__vk_fence **wait_fences; /**< per wait: its fence, NULL for a swapchain acquire */
    de__vk_fence *signal_fence; /**< the de_gpu_submit signal, or NULL */
} de__vk_deferred;

/* True when every fence wait of the submit is covered: some submission handed to the VkQueues
 * signals a value at least as large. */
static bool waits_covered(de__vk_fence *const *fences, const VkSemaphoreSubmitInfo *waits, u32 n) {
    for (u32 i = 0; i < n; ++i)
        if (fences[i] &&
            de_atomic_load_u64(&fences[i]->submitted, DE_ATOMIC_ACQUIRE) < waits[i].value)
            return false;
    return true;
}

static void note_signal(de__vk_fence *f, u64 value) {
    if (!f)
        return;
    u64 cur = de_atomic_load_u64(&f->submitted, DE_ATOMIC_RELAXED);
    while (cur < value && !de_atomic_cas_weak_u64(&f->submitted, &cur, value, DE_ATOMIC_RELEASE,
                                                  DE_ATOMIC_RELAXED))
        ;
}

void de__gpu_vk_sync_diagnostics_enable(de_gpu_device *d) {
    if (d && d->vt == &de__gpu_vulkan_vtable)
        de__vk(d)->sync_diagnostics = true;
}

void de__gpu_vk_sync_diagnostics_dump(de_gpu_device *d, de_gpu_fence fence, const char *stage,
                                      bool query_completed) {
    if (!d || d->vt != &de__gpu_vulkan_vtable || !de__vk(d)->sync_diagnostics)
        return;
    de__vk_device *vk = de__vk(d);
    const u64 begin = de_time_now_ns();
    fprintf(stderr,
            "vk-sync snapshot-begin t_ns=%llu stage=%s query_counters=%u validation=%u "
            "gpu_av=%u\n",
            (unsigned long long)begin, stage, (unsigned)query_completed,
            (unsigned)vk->on.validation, (unsigned)vk->gpu_av);
    /* One coherent CPU submission snapshot. Never wait for GPU progress or flush held work. */
    for (u32 l = 0; l < vk->queue_lock_count; ++l)
        de_mutex_lock(&vk->queue_locks[l]);
    de__gpu_fence *f = DE_HANDLE_VALID(fence) ? de__gpu_get_fence(d, fence) : NULL;
    if (f) {
        de__vk_fence *vf = DE__GPU_PAYLOAD(de__vk_fence, f);
        u64 completed = 0;
        const VkResult vr =
            query_completed
                ? vk->fn.vkGetSemaphoreCounterValue(vk->device, vf->semaphore, &completed)
                : VK_NOT_READY;
        fprintf(stderr,
                "vk-sync t_ns=%llu stage=%s fence=%u submitted=%llu completed=%llu query=%d\n",
                (unsigned long long)de_time_now_ns(), stage, fence.id,
                (unsigned long long)de_atomic_load_u64(&vf->submitted, DE_ATOMIC_ACQUIRE),
                (unsigned long long)completed, (int)vr);
    }
    for (u32 q = 0; q < DE_QUEUE_COUNT_; ++q) {
        u64 completed = 0;
        const VkResult vr =
            query_completed
                ? vk->fn.vkGetSemaphoreCounterValue(vk->device, vk->queues[q].timeline, &completed)
                : VK_NOT_READY;
        fprintf(stderr,
                "vk-sync queue=%s lock=%u shared=%u accepted=%llu submitted=%llu completed=%llu "
                "query=%d\n",
                queue_name((de_gpu_queue)q), vk->queues[q].lock,
                (unsigned)vk->lock_shared[vk->queues[q].lock],
                (unsigned long long)vk->last_value[q], (unsigned long long)vk->sync_submitted[q],
                (unsigned long long)completed, (int)vr);
    }
    for (u32 l = 0; l < vk->queue_lock_count; ++l) {
        u32 count = 0;
        for (const de__vk_deferred *e = vk->deferred[l]; e; e = e->next) {
            if (count++ >= 32u)
                continue;
            fprintf(stderr,
                    "vk-sync held lock=%u queue=%s signal=%llu commands=%u waits=%u covered=%u\n",
                    l, queue_name(e->q), (unsigned long long)e->sigs[0].value, e->ncb, e->nwait,
                    (unsigned)waits_covered(e->wait_fences, e->waits, e->nwait));
            for (u32 w = 0; w < e->nwait && w < 8u; ++w)
                if (e->wait_fences[w])
                    fprintf(stderr, "vk-sync held-wait value=%llu submitted=%llu\n",
                            (unsigned long long)e->waits[w].value,
                            (unsigned long long)de_atomic_load_u64(&e->wait_fences[w]->submitted,
                                                                   DE_ATOMIC_ACQUIRE));
        }
        fprintf(stderr, "vk-sync held-total lock=%u count=%u details_limit=32 waits_limit=8\n", l,
                count);
    }
    for (u32 l = vk->queue_lock_count; l > 0; --l)
        de_mutex_unlock(&vk->queue_locks[l - 1u]);
    fprintf(stderr, "vk-sync snapshot-end t_ns=%llu stage=%s elapsed_ns=%llu\n",
            (unsigned long long)de_time_now_ns(), stage,
            (unsigned long long)(de_time_now_ns() - begin));
}

static VkResult submit_now(de__vk_device *vk, VkQueue queue, const VkCommandBufferSubmitInfo *cbs,
                           u32 ncb, const VkSemaphoreSubmitInfo *waits, u32 nwait,
                           const VkSemaphoreSubmitInfo *sigs, u32 nsig) {
    if (de__vk_lost(vk))
        return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo2 si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount = nwait;
    si.pWaitSemaphoreInfos = nwait ? waits : NULL;
    si.commandBufferInfoCount = ncb;
    si.pCommandBufferInfos = ncb ? cbs : NULL;
    si.signalSemaphoreInfoCount = nsig;
    si.pSignalSemaphoreInfos = sigs;
    return vk->fn.vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE);
}

/* Copies a submit into a held-back entry at the end of `lock`'s list (the lock is held). */
static bool defer(de_gpu_device *d, u32 lock, de_gpu_queue q, const VkCommandBufferSubmitInfo *cbs,
                  u32 ncb, const VkSemaphoreSubmitInfo *waits, de__vk_fence *const *wait_fences,
                  u32 nwait, const VkSemaphoreSubmitInfo *sigs, u32 nsig,
                  de__vk_fence *signal_fence) {
    de__vk_device *vk = de__vk(d);
    const size_t bytes = sizeof(de__vk_deferred) + (size_t)ncb * sizeof *cbs +
                         (size_t)(nwait + nsig) * sizeof *waits +
                         (size_t)nwait * sizeof(de__vk_fence *);
    de__vk_deferred *e = (de__vk_deferred *)DE_ALLOC(d->alloc, bytes, 16, DE_MEM_RENDER);
    if (!e)
        return false;
    memset(e, 0, sizeof *e);
    e->q = q;
    e->ncb = ncb;
    e->nwait = nwait;
    e->nsig = nsig;
    e->bytes = bytes;
    u8 *p = (u8 *)(e + 1);
    e->cbs = (VkCommandBufferSubmitInfo *)p;
    p += (size_t)ncb * sizeof *cbs;
    e->waits = (VkSemaphoreSubmitInfo *)p;
    p += (size_t)nwait * sizeof *waits;
    e->sigs = (VkSemaphoreSubmitInfo *)p;
    p += (size_t)nsig * sizeof *waits;
    e->wait_fences = (de__vk_fence **)p;
    if (ncb)
        memcpy(e->cbs, cbs, (size_t)ncb * sizeof *cbs);
    if (nwait) {
        memcpy(e->waits, waits, (size_t)nwait * sizeof *waits);
        memcpy(e->wait_fences, wait_fences, (size_t)nwait * sizeof *wait_fences);
    }
    memcpy(e->sigs, sigs, (size_t)nsig * sizeof *sigs);
    e->signal_fence = signal_fence;
    de__vk_deferred **tail = &vk->deferred[lock];
    while (*tail)
        tail = &(*tail)->next;
    *tail = e;
    return true;
}

/* Hands over the held-back submits of `lock` whose waits are covered, in order (an entry waits
 * while an earlier one of its de_gpu queue is still held). Takes the lock. True when any went. */
static bool flush_lock(de_gpu_device *d, u32 lock) {
    de__vk_device *vk = de__vk(d);
    de_mutex_lock(&vk->queue_locks[lock]);
    bool any = false;
    bool progress = true;
    while (progress && vk->deferred[lock]) {
        progress = false;
        bool blocked[DE_QUEUE_COUNT_] = {false};
        for (de__vk_deferred **at = &vk->deferred[lock]; *at;) {
            de__vk_deferred *e = *at;
            if (blocked[e->q] || !waits_covered(e->wait_fences, e->waits, e->nwait)) {
                blocked[e->q] = true;
                at = &e->next;
                continue;
            }
            const VkResult vr = submit_now(vk, vk->queues[e->q].queue, e->cbs, e->ncb, e->waits,
                                           e->nwait, e->sigs, e->nsig);
            if (vr != VK_SUCCESS)
                mark_lost(vk); /* every value counts as complete from now on */
            else {
                vk->sync_submitted[e->q] = e->sigs[0].value;
                if (e->signal_fence)
                    note_signal(e->signal_fence, e->sigs[e->nsig - 1u].value);
            }
            if (vk->sync_diagnostics && vk->sync_trace_count[e->q]++ < 128u)
                fprintf(stderr, "vk-sync t_ns=%llu flush queue=%s value=%llu result=%d\n",
                        (unsigned long long)de_time_now_ns(), queue_name(e->q),
                        (unsigned long long)e->sigs[0].value, (int)vr);
            *at = e->next;
            DE_FREE(d->alloc, e, e->bytes);
            progress = any = true;
        }
    }
    de_mutex_unlock(&vk->queue_locks[lock]);
    return any;
}

/* Until nothing more goes: a submit handed over on one lock may cover a wait on another. */
static void flush_all(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    bool any = true;
    while (any) {
        any = false;
        for (u32 l = 0; l < vk->queue_lock_count; ++l)
            if (vk->lock_shared[l])
                any |= flush_lock(d, l);
    }
}

void de__vk_deferred_free(de_gpu_device *d) {
    de__vk_device *vk = de__vk(d);
    for (u32 l = 0; l < DE_QUEUE_COUNT_; ++l) {
        while (vk->deferred[l]) {
            de__vk_deferred *e = vk->deferred[l];
            vk->deferred[l] = e->next;
            DE_FREE(d->alloc, e, e->bytes);
        }
    }
}

de_result de__vk_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                              u64 queue_value) {
    de__vk_device *vk = de__vk(d);
    de__vk_queue *vq = &vk->queues[q];

    VkCommandBufferSubmitInfo cb_local[LOCAL];
    VkSemaphoreSubmitInfo wait_local[LOCAL + DE__VK_MAX_PENDING_ACQUIRES];
    de__vk_fence *wait_fence_local[LOCAL + DE__VK_MAX_PENDING_ACQUIRES];
    de__vk_fence **wait_fences = wait_fence_local;
    VkCommandBufferSubmitInfo *cbs = cb_local;
    VkSemaphoreSubmitInfo *waits = wait_local;
    const u32 wait_cap = s->wait_count + DE__VK_MAX_PENDING_ACQUIRES;
    const size_t cb_bytes = (size_t)s->cmd_count * sizeof *cbs;
    const size_t wait_bytes = (size_t)wait_cap * sizeof *waits;
    if (s->cmd_count > LOCAL)
        cbs = (VkCommandBufferSubmitInfo *)DE_ALLOC(d->alloc, cb_bytes, 8, DE_MEM_RENDER);
    const size_t wf_bytes = (size_t)wait_cap * sizeof *wait_fences;
    if (wait_cap > LOCAL + DE__VK_MAX_PENDING_ACQUIRES) {
        waits = (VkSemaphoreSubmitInfo *)DE_ALLOC(d->alloc, wait_bytes, 8, DE_MEM_RENDER);
        wait_fences = (de__vk_fence **)DE_ALLOC(d->alloc, wf_bytes, 8, DE_MEM_RENDER);
    }
    de_result r = DE_OK;
    if (!cbs || !waits || !wait_fences) {
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
        wait_fences[nwait] = DE__GPU_PAYLOAD(de__vk_fence, f);
        waits[nwait].semaphore = wait_fences[nwait]->semaphore;
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
    de__vk_fence *signal_fence = NULL;
    if (r == DE_OK && s->signal.id) {
        de__gpu_fence *f = de__gpu_get_fence(d, s->signal);
        if (f) {
            signal_fence = DE__GPU_PAYLOAD(de__vk_fence, f);
            sig[nsig].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            sig[nsig].semaphore = DE__GPU_PAYLOAD(de__vk_fence, f)->semaphore;
            sig[nsig].value = s->signal_value;
            sig[nsig].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            ++nsig;
        }
    }

    VkResult vr = VK_ERROR_DEVICE_LOST;
    de_mutex_lock(&vk->queue_locks[vq->lock]);
    const bool trace = vk->sync_diagnostics && vk->sync_trace_count[q]++ < 128u;
    const u64 submit_begin = trace ? de_time_now_ns() : 0;
    if (trace)
        fprintf(stderr,
                "vk-sync t_ns=%llu submit-begin queue=%s value=%llu commands=%u waits=%u "
                "fence=%u fence_value=%llu\n",
                (unsigned long long)submit_begin, queue_name(q), (unsigned long long)queue_value,
                ncb, nwait, s->signal.id, (unsigned long long)s->signal_value);
    if (trace) {
        for (u32 i = 0; i < s->cmd_count && i < 4u; ++i)
            fprintf(stderr, "vk-sync command=%s\n", s->cmds[i]->name);
        for (u32 i = 0; i < nwait && i < 8u; ++i)
            if (wait_fences[i])
                fprintf(stderr, "vk-sync submit-wait value=%llu submitted=%llu\n",
                        (unsigned long long)waits[i].value,
                        (unsigned long long)de_atomic_load_u64(&wait_fences[i]->submitted,
                                                               DE_ATOMIC_ACQUIRE));
    }
    /* Swapchain acquires wait for the next graphics submit (guarded by the graphics lock). */
    const bool takes_acquires = r == DE_OK && q == DE_QUEUE_GRAPHICS;
    u32 nacq = 0;
    if (takes_acquires) {
        for (u32 i = 0; i < vk->pending_acquire_count; ++i) {
            memset(&waits[nwait], 0, sizeof waits[nwait]);
            waits[nwait].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
            waits[nwait].semaphore = vk->pending_acquire[i]->semaphore;
            waits[nwait].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            wait_fences[nwait] = NULL; /* a binary semaphore an acquire signalled */
            ++nwait;
            ++nacq;
        }
    }
    /* DENG-426: on a shared VkQueue, hold the submit back while a wait is not covered yet, or
     * while an earlier submit of this queue is held back. */
    bool held = false;
    if (vk->lock_shared[vq->lock] && !de__vk_lost(vk)) {
        bool earlier = false;
        for (const de__vk_deferred *e = vk->deferred[vq->lock]; e && !earlier; e = e->next)
            earlier = e->q == q;
        if (earlier || !waits_covered(wait_fences, waits, nwait)) {
            held = defer(d, vq->lock, q, cbs, ncb, waits, wait_fences, nwait, sig, nsig,
                         nsig > 1u ? signal_fence : NULL);
            if (!held && r == DE_OK)
                r = DE_FAIL_WRAP(DE_ERR_OUT_OF_MEMORY, "de_gpu_queue_submit: a held-back submit");
            vr = held ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    if (!held && vr != VK_ERROR_OUT_OF_HOST_MEMORY) {
        vr = submit_now(vk, vq->queue, cbs, ncb, waits, nwait, sig, nsig);
        if (vr == VK_SUCCESS)
            vk->sync_submitted[q] = queue_value;
        if (vr == VK_SUCCESS && nsig > 1u)
            note_signal(signal_fence, sig[1].value);
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
    if (trace)
        fprintf(
            stderr,
            "vk-sync t_ns=%llu submit-end queue=%s value=%llu held=%u result=%d elapsed_ns=%llu\n",
            (unsigned long long)de_time_now_ns(), queue_name(q), (unsigned long long)queue_value,
            (unsigned)held, (int)vr, (unsigned long long)(de_time_now_ns() - submit_begin));
    de_mutex_unlock(&vk->queue_locks[vq->lock]);

    /* A signal went in: submits held back on shared VkQueues may be covered now. */
    if (vr == VK_SUCCESS && !held && nsig > 1u)
        flush_all(d);
    if (cbs != cb_local && cbs)
        DE_FREE(d->alloc, cbs, cb_bytes);
    if (waits != wait_local && waits)
        DE_FREE(d->alloc, waits, wait_bytes);
    if (wait_fences != wait_fence_local && wait_fences)
        DE_FREE(d->alloc, wait_fences, wf_bytes);
    if (vr != VK_SUCCESS) {
        /* The value can no longer be signalled in order: report every value as complete. */
        mark_lost(vk);
        if (r == DE_OK)
            r = de__vk_fail(d, vr, "vulkan: vkQueueSubmit2 on the %s queue", queue_name(q));
    }
    return r;
}
