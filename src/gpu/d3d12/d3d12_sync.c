/* d3d12_sync.c — D3D12 submission and timeline fences (DENG-286; SPEC §12.1).
 *
 * Fences: a de_gpu_fence is an ID3D12Fence, which is a timeline (a 64-bit value that only the
 * signals move). fence_value is GetCompletedValue; fence_wait polls (timeout 0), blocks on
 * SetEventOnCompletion with an event of its own (several threads may wait on one fence), or with
 * no event for an infinite wait. A removed device completes every value (UINT64_MAX): the wait
 * returns DE_ERR_DEVICE_LOST.
 *
 * Submit, all on the queue, in order: a Wait for each de_gpu_wait (a wait for a value nobody has
 * signalled yet is fine: the queue stalls until it is), ExecuteCommandLists, Signal of the user
 * fence, Signal of the queue's internal timeline fence (gpu_backend.h). With enhanced barriers the
 * lists go in one ExecuteCommandLists call; with legacy barriers each list gets its own, so that a
 * list boundary is where buffers decay to COMMON, which the legacy state tracking relies on
 * (d3d12_barriers.c). A list that fails to Close (a bad recorded command) is not executed and the
 * submit fails, but the signals are still queued, so nothing waits for them forever.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

/* ---------- Submission ---------- */

static void note_signaled(d3d12_queue *dq, u64 value) {
    u64 cur = de_atomic_load_u64(&dq->signaled, DE_ATOMIC_RELAXED);
    while (cur < value && !de_atomic_cas_weak_u64(&dq->signaled, &cur, value, DE_ATOMIC_RELEASE,
                                                  DE_ATOMIC_RELAXED)) {
    }
}

/* Lists executed per ExecuteCommandLists call (more go in further calls). */
#define EXEC_CHUNK 32u

de_result de__d3d12_queue_submit(de_gpu_device *d, de_gpu_queue q, const de_gpu_submit *s,
                                 u64 queue_value) {
    d3d12_device *dd = de__d3d12(d);
    d3d12_queue *dq = &dd->queues[q];
    de_result r = DE_OK;

    for (u32 i = 0; i < s->wait_count; ++i) {
        de__gpu_fence *f = de__gpu_get_fence(d, s->waits[i].fence);
        if (!f)
            continue; /* validated by the front-end */
        HRESULT hr = ID3D12CommandQueue_Wait(dq->queue, DE__GPU_PAYLOAD(d3d12_fence, f)->fence,
                                             s->waits[i].value);
        if (FAILED(hr) && r == DE_OK)
            r = de__d3d12_fail(hr, "de_gpu_queue_submit: ID3D12CommandQueue::Wait (wait %u)", i);
    }

    /* Close every list first: a list that fails is left out. */
    ID3D12CommandList *lists[EXEC_CHUNK];
    u32 n = 0;
    const bool one_call = s->cmd_count > 0 && de__d3d12_use_enhanced_barriers(d);
    for (u32 i = 0; i < s->cmd_count; ++i) {
        de_gpu_cmd *c = s->cmds[i];
        de_result cr = de__d3d12_cmd_close(c);
        if (cr != DE_OK) {
            if (r == DE_OK)
                r = cr;
            continue;
        }
        lists[n++] = (ID3D12CommandList *)de__d3d12_cmd(c)->list;
        if (n == EXEC_CHUNK || !one_call) {
            ID3D12CommandQueue_ExecuteCommandLists(dq->queue, n, lists);
            n = 0;
        }
    }
    if (n)
        ID3D12CommandQueue_ExecuteCommandLists(dq->queue, n, lists);

    if (s->signal.id) {
        de__gpu_fence *f = de__gpu_get_fence(d, s->signal);
        if (f) {
            HRESULT hr = ID3D12CommandQueue_Signal(
                dq->queue, DE__GPU_PAYLOAD(d3d12_fence, f)->fence, s->signal_value);
            if (FAILED(hr) && r == DE_OK)
                r = de__d3d12_fail(hr, "de_gpu_queue_submit: ID3D12CommandQueue::Signal (fence)");
        }
    }
    /* The front-end has published queue_value: it must complete, even after a failure. */
    HRESULT hr = ID3D12CommandQueue_Signal(dq->queue, dq->fence, queue_value);
    if (FAILED(hr)) {
        ID3D12Fence_Signal(dq->fence, queue_value); /* from the CPU, so nothing waits forever */
        if (r == DE_OK)
            r = de__d3d12_fail(hr, "de_gpu_queue_submit: ID3D12CommandQueue::Signal");
    }
    note_signaled(dq, queue_value);
    de__d3d12_drain_messages(dd);
    return r;
}

/* ---------- Fences ---------- */

de_result de__d3d12_fence_create(de_gpu_device *d, de__gpu_fence *f, u64 initial) {
    d3d12_fence *df = DE__GPU_PAYLOAD(d3d12_fence, f);
    HRESULT hr = ID3D12Device_CreateFence(de__d3d12(d)->dev, initial, D3D12_FENCE_FLAG_NONE,
                                          &IID_ID3D12Fence, (void **)&df->fence);
    if (FAILED(hr)) {
        df->fence = NULL;
        return de__d3d12_fail(hr, "de_gpu_fence_create: ID3D12Device::CreateFence");
    }
    de__d3d12_set_name((ID3D12Object *)df->fence, "de fence");
    return DE_OK;
}

void de__d3d12_fence_destroy(de_gpu_device *d, de__gpu_fence *f) {
    (void)d;
    d3d12_fence *df = DE__GPU_PAYLOAD(d3d12_fence, f);
    if (df->fence)
        ID3D12Fence_Release(df->fence);
    df->fence = NULL;
}

u64 de__d3d12_fence_value(de_gpu_device *d, de__gpu_fence *f) {
    (void)d;
    return ID3D12Fence_GetCompletedValue(DE__GPU_PAYLOAD(d3d12_fence, f)->fence);
}

static de_result device_lost(de_gpu_device *d) {
    HRESULT reason = ID3D12Device_GetDeviceRemovedReason(de__d3d12(d)->dev);
    return de__d3d12_fail(reason, "de_gpu_fence_wait: the device was removed");
}

de_result de__d3d12_fence_wait(de_gpu_device *d, de__gpu_fence *f, u64 value, u64 timeout_ns) {
    ID3D12Fence *fence = DE__GPU_PAYLOAD(d3d12_fence, f)->fence;
    u64 done = ID3D12Fence_GetCompletedValue(fence);
    if (done == UINT64_MAX)
        return device_lost(d);
    if (done >= value)
        return DE_OK;
    if (timeout_ns == 0)
        return DE_ERR_TIMEOUT;
    if (timeout_ns == UINT64_MAX) {
        /* No event: SetEventOnCompletion blocks until the value is reached. */
        HRESULT hr = ID3D12Fence_SetEventOnCompletion(fence, value, NULL);
        if (FAILED(hr))
            return de__d3d12_fail(hr, "de_gpu_fence_wait: SetEventOnCompletion");
        return ID3D12Fence_GetCompletedValue(fence) == UINT64_MAX ? device_lost(d) : DE_OK;
    }
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!ev)
        return DE_FAIL(DE_ERR_OUT_OF_MEMORY, "de_gpu_fence_wait: CreateEvent failed (%lu)",
                       (unsigned long)GetLastError());
    de_result r = DE_OK;
    HRESULT hr = ID3D12Fence_SetEventOnCompletion(fence, value, ev);
    if (FAILED(hr)) {
        r = de__d3d12_fail(hr, "de_gpu_fence_wait: SetEventOnCompletion");
    } else {
        /* Round up to whole milliseconds; below INFINITE. */
        const u64 ms64 = (timeout_ns + 999999u) / 1000000u;
        const DWORD ms = ms64 >= (u64)INFINITE ? INFINITE - 1u : (DWORD)ms64;
        const DWORD w = WaitForSingleObject(ev, ms);
        if (w == WAIT_TIMEOUT)
            r = ID3D12Fence_GetCompletedValue(fence) >= value ? DE_OK : DE_ERR_TIMEOUT;
        else if (w != WAIT_OBJECT_0)
            r = DE_FAIL(DE_ERR_INTERNAL, "de_gpu_fence_wait: WaitForSingleObject failed (%lu)",
                        (unsigned long)GetLastError());
        if (r == DE_OK && ID3D12Fence_GetCompletedValue(fence) == UINT64_MAX)
            r = device_lost(d);
    }
    CloseHandle(ev);
    return r;
}
