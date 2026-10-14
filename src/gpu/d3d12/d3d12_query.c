/* d3d12_query.c — D3D12 timestamp queries (DENG-326; SPEC §12.1, §12.3).
 *
 * A de_gpu_query_heap is one ID3D12QueryHeap: D3D12_QUERY_HEAP_TYPE_TIMESTAMP for graphics and
 * compute lists, D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP for copy lists (the desc's queue; the
 * front-end keeps copy heaps on copy lists). A copy heap needs
 * D3D12_FEATURE_DATA_D3D12_OPTIONS3.CopyQueueTimestampQueriesSupported, else DE_ERR_UNSUPPORTED.
 *
 * de_gpu_cmd_timestamp is EndQuery (a timestamp query has no Begin); the resolve is
 * ResolveQueryData into the destination buffer, which must be in COPY_DEST (the caller's barrier).
 * Ticks run at each queue's GetTimestampFrequency (de_gpu_timestamp_frequency asks the queue: the
 * copy queue may differ from the direct queue that caps.timestamp_frequency reports), and
 * GetClockCalibration gives the GPU time now for profiler calibration.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <string.h>

static d3d12_query_heap *payload(de__gpu_query_heap *h) {
    return DE__GPU_PAYLOAD(d3d12_query_heap, h);
}

de_result de__d3d12_query_heap_create(de_gpu_device *d, de__gpu_query_heap *h,
                                      const de_gpu_query_heap_desc *desc) {
    d3d12_device *dd = de__d3d12(d);
    const bool copy = desc->queue == DE_QUEUE_COPY;
    if (copy) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS3 o3;
        memset(&o3, 0, sizeof o3);
        if (FAILED(ID3D12Device_CheckFeatureSupport(dd->dev, D3D12_FEATURE_D3D12_OPTIONS3, &o3,
                                                    sizeof o3)) ||
            !o3.CopyQueueTimestampQueriesSupported)
            return de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                                "de_gpu_query_heap_create(\"%s\"): d3d12: \"%s\" has no copy-queue "
                                "timestamps (CopyQueueTimestampQueriesSupported)",
                                h->name, d->caps.device_name);
    }
    D3D12_QUERY_HEAP_DESC qd;
    memset(&qd, 0, sizeof qd);
    qd.Type = copy ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = h->count;
    ID3D12QueryHeap *heap = NULL;
    HRESULT hr = ID3D12Device_CreateQueryHeap(dd->dev, &qd, &IID_ID3D12QueryHeap, (void **)&heap);
    if (FAILED(hr))
        return de__d3d12_fail(hr, "de_gpu_query_heap_create(\"%s\"): d3d12: CreateQueryHeap (%u)",
                              h->name, h->count);
    payload(h)->heap = heap;
    return DE_OK;
}

void de__d3d12_query_heap_destroy(de_gpu_device *d, de__gpu_query_heap *h) {
    (void)d;
    d3d12_query_heap *qh = payload(h);
    if (qh->heap)
        ID3D12QueryHeap_Release(qh->heap);
    qh->heap = NULL;
}

void de__d3d12_cmd_timestamp(de_gpu_cmd *c, de__gpu_query_heap *h, u32 index) {
    ID3D12GraphicsCommandList_EndQuery(de__d3d12_cmd(c)->list, payload(h)->heap,
                                       D3D12_QUERY_TYPE_TIMESTAMP, index);
}

void de__d3d12_cmd_resolve_queries(de_gpu_cmd *c, de__gpu_query_heap *h, u32 first, u32 count,
                                   de__gpu_buffer *dst, u64 dst_off) {
    ID3D12GraphicsCommandList_ResolveQueryData(de__d3d12_cmd(c)->list, payload(h)->heap,
                                               D3D12_QUERY_TYPE_TIMESTAMP, first, count,
                                               DE__GPU_PAYLOAD(d3d12_buffer, dst)->res, dst_off);
}

u64 de__d3d12_queue_timestamp_frequency(de_gpu_device *d, de_gpu_queue q) {
    UINT64 f = 0;
    ID3D12CommandQueue *queue = de__d3d12(d)->queues[q].queue;
    if (!queue || FAILED(ID3D12CommandQueue_GetTimestampFrequency(queue, &f)))
        return 0;
    return f;
}

bool de__d3d12_queue_timestamp_now(de_gpu_device *d, de_gpu_queue q, u64 *ticks) {
    UINT64 gpu = 0, cpu = 0;
    ID3D12CommandQueue *queue = de__d3d12(d)->queues[q].queue;
    if (!queue || FAILED(ID3D12CommandQueue_GetClockCalibration(queue, &gpu, &cpu)))
        return false;
    *ticks = gpu;
    return gpu != 0;
}
