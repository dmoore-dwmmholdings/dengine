/* d3d12_dred.c — GPU crash diagnostics on D3D12 (DENG-328; SPEC §18.5).
 *
 * DRED (Device Removed Extended Data): with de_gpu_device_desc.crash_breadcrumbs,
 * de__d3d12_dred_enable turns on auto-breadcrumbs, breadcrumb contexts (the PIX event names of
 * DENG-326's markers) and page-fault reporting through ID3D12DeviceRemovedExtendedDataSettings1.
 * The settings are process-wide and only apply to devices created after them, so d3d12_device.c
 * calls it right before D3D12CreateDevice (select_adapter).
 *
 * Breadcrumbs: WriteBufferImmediate (ID3D12GraphicsCommandList2) in MARKER_OUT mode, which writes
 * once every earlier command of the list has completed, to the ring slot's GPU address. The ring
 * is a READBACK buffer, which stays in COPY_DEST, the state WriteBufferImmediate needs. Allowed on
 * every queue type, inside render passes too (the front-end still keeps breadcrumbs outside).
 *
 * Device lost: the device counts as lost once GetDeviceRemovedReason is not S_OK (polled by the
 * front-end). The report section gives the removal reason, the DRED device state, the
 * auto-breadcrumb op history of every command list that had not finished (with the last
 * completed op marked, and the marker contexts), and the page-fault address with the allocations
 * that were at it or recently freed there.
 */
#include "gpu/d3d12/d3d12_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** Command lists, ops around the last completed one, and allocations shown per section. */
#define DRED_NODES       16u
#define DRED_OPS_BEFORE  6u
#define DRED_OPS_AFTER   6u
#define DRED_ALLOCATIONS 8u

/* ---------- Setup ---------- */

void de__d3d12_dred_enable(d3d12_device *dd) {
    ID3D12DeviceRemovedExtendedDataSettings1 *s1 = NULL;
    HRESULT hr =
        D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings1, (void **)&s1);
    if (SUCCEEDED(hr) && s1) {
        ID3D12DeviceRemovedExtendedDataSettings1_SetAutoBreadcrumbsEnablement(
            s1, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings1_SetPageFaultEnablement(
            s1, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings1_SetBreadcrumbContextEnablement(
            s1, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings1_Release(s1);
        dd->dred = true;
        return;
    }
    /* Older runtimes: the version without breadcrumb contexts. */
    ID3D12DeviceRemovedExtendedDataSettings *s = NULL;
    hr = D3D12GetDebugInterface(&IID_ID3D12DeviceRemovedExtendedDataSettings, (void **)&s);
    if (SUCCEEDED(hr) && s) {
        ID3D12DeviceRemovedExtendedDataSettings_SetAutoBreadcrumbsEnablement(
            s, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings_SetPageFaultEnablement(
            s, D3D12_DRED_ENABLEMENT_FORCED_ON);
        ID3D12DeviceRemovedExtendedDataSettings_Release(s);
        dd->dred = true;
        return;
    }
    DE_WARN(dd->log,
            "d3d12: DRED settings are not available (HRESULT 0x%08lx); device-lost reports "
            "will have no DRED data",
            (unsigned long)hr);
}

/* ---------- Breadcrumbs ---------- */

void de__d3d12_cmd_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value) {
    d3d12_cmd *dc = de__d3d12_cmd(c);
    if (!dc->list2) {
        ID3D12GraphicsCommandList2 *l2 = NULL;
        if (FAILED(ID3D12GraphicsCommandList_QueryInterface(
                dc->list, &IID_ID3D12GraphicsCommandList2, (void **)&l2)))
            return;
        /* Not counted: the list holds the reference (as list6, d3d12_draw.c). */
        ID3D12GraphicsCommandList2_Release(l2);
        dc->list2 = l2;
    }
    ID3D12Resource *res = DE__GPU_PAYLOAD(d3d12_buffer, ring)->res;
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p;
    p.Dest = ID3D12Resource_GetGPUVirtualAddress(res) + offset;
    p.Value = value;
    D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    ID3D12GraphicsCommandList2_WriteBufferImmediate(dc->list2, 1, &p, &mode);
}

/* ---------- Device lost ---------- */

bool de__d3d12_device_lost(de_gpu_device *d) {
    ID3D12Device *dev = de__d3d12(d)->dev;
    return dev && ID3D12Device_GetDeviceRemovedReason(dev) != S_OK;
}

typedef struct text_buf {
    char *out;
    size_t cap, len;
} text_buf;

static void put(text_buf *b, const char *fmt, ...) DE_PRINTF_FMT(2, 3);
static void put(text_buf *b, const char *fmt, ...) {
    if (b->len + 1u >= b->cap)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->out + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (w > 0)
        b->len += (size_t)w < b->cap - b->len ? (size_t)w : b->cap - b->len - 1u;
}

/* UTF-8 of a narrow or wide debug name ("" when neither). */
static const char *dred_name(const char *a, const wchar_t *w, char *buf, int cap) {
    if (a && a[0])
        return a;
    buf[0] = '\0';
    if (w && w[0] && WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, cap, NULL, NULL) <= 0)
        buf[0] = '\0';
    return buf;
}

static const char *removed_reason_name(HRESULT hr) {
    switch (hr) {
    case S_OK:
        return "S_OK (not removed)";
    case DXGI_ERROR_DEVICE_HUNG:
        return "DXGI_ERROR_DEVICE_HUNG";
    case DXGI_ERROR_DEVICE_REMOVED:
        return "DXGI_ERROR_DEVICE_REMOVED";
    case DXGI_ERROR_DEVICE_RESET:
        return "DXGI_ERROR_DEVICE_RESET";
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
        return "DXGI_ERROR_DRIVER_INTERNAL_ERROR";
    case DXGI_ERROR_INVALID_CALL:
        return "DXGI_ERROR_INVALID_CALL";
    default:
        return "?";
    }
}

static const char *device_state_name(D3D12_DRED_DEVICE_STATE s) {
    switch (s) {
    case D3D12_DRED_DEVICE_STATE_HUNG:
        return "hung";
    case D3D12_DRED_DEVICE_STATE_FAULT:
        return "fault";
    case D3D12_DRED_DEVICE_STATE_PAGEFAULT:
        return "page fault";
    case D3D12_DRED_DEVICE_STATE_UNKNOWN:
    default:
        return "unknown";
    }
}

#define DRED_OPS(X)                                                                                \
    X(SETMARKER)                                                                                   \
    X(BEGINEVENT)                                                                                  \
    X(ENDEVENT)                                                                                    \
    X(DRAWINSTANCED)                                                                               \
    X(DRAWINDEXEDINSTANCED)                                                                        \
    X(EXECUTEINDIRECT)                                                                             \
    X(DISPATCH)                                                                                    \
    X(COPYBUFFERREGION)                                                                            \
    X(COPYTEXTUREREGION)                                                                           \
    X(COPYRESOURCE)                                                                                \
    X(COPYTILES)                                                                                   \
    X(RESOLVESUBRESOURCE)                                                                          \
    X(CLEARRENDERTARGETVIEW)                                                                       \
    X(CLEARUNORDEREDACCESSVIEW)                                                                    \
    X(CLEARDEPTHSTENCILVIEW)                                                                       \
    X(RESOURCEBARRIER)                                                                             \
    X(EXECUTEBUNDLE)                                                                               \
    X(PRESENT)                                                                                     \
    X(RESOLVEQUERYDATA)                                                                            \
    X(BEGINSUBMISSION)                                                                             \
    X(ENDSUBMISSION)                                                                               \
    X(ATOMICCOPYBUFFERUINT)                                                                        \
    X(ATOMICCOPYBUFFERUINT64)                                                                      \
    X(RESOLVESUBRESOURCEREGION)                                                                    \
    X(WRITEBUFFERIMMEDIATE)                                                                        \
    X(BUILDRAYTRACINGACCELERATIONSTRUCTURE)                                                        \
    X(EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO)                                            \
    X(COPYRAYTRACINGACCELERATIONSTRUCTURE)                                                         \
    X(DISPATCHRAYS)                                                                                \
    X(SETPIPELINESTATE1)                                                                           \
    X(DISPATCHMESH)                                                                                \
    X(BARRIER)                                                                                     \
    X(BEGIN_COMMAND_LIST)

static const char *op_name(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
#define DRED_OP_CASE(n)                                                                            \
    case D3D12_AUTO_BREADCRUMB_OP_##n:                                                             \
        return #n;
        DRED_OPS(DRED_OP_CASE)
#undef DRED_OP_CASE
    default:
        return "other";
    }
}

/* The context string (marker name) DRED recorded at op `index` of `n`, or NULL. */
static const wchar_t *op_context(const D3D12_AUTO_BREADCRUMB_NODE1 *n, UINT index) {
    for (UINT i = 0; n->pBreadcrumbContexts && i < n->BreadcrumbContextsCount; ++i)
        if (n->pBreadcrumbContexts[i].BreadcrumbIndex == index)
            return n->pBreadcrumbContexts[i].pContextString;
    return NULL;
}

static void report_nodes(text_buf *b, const D3D12_AUTO_BREADCRUMB_NODE1 *head) {
    u32 shown = 0, total = 0;
    for (const D3D12_AUTO_BREADCRUMB_NODE1 *n = head; n; n = n->pNext) {
        ++total;
        const UINT count = n->BreadcrumbCount;
        const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
        if (done >= count)
            continue; /* finished: not where the fault is */
        if (shown++ >= DRED_NODES)
            continue;
        char lbuf[128], qbuf[128];
        put(b, "  list \"%s\" on queue \"%s\": %u of %u ops completed\n",
            dred_name(n->pCommandListDebugNameA, n->pCommandListDebugNameW, lbuf, sizeof lbuf),
            dred_name(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW, qbuf, sizeof qbuf),
            done, count);
        const UINT from = done > DRED_OPS_BEFORE ? done - DRED_OPS_BEFORE : 0;
        const UINT to = done + DRED_OPS_AFTER < count ? done + DRED_OPS_AFTER : count;
        for (UINT i = from; n->pCommandHistory && i < to; ++i) {
            char cbuf[128];
            const wchar_t *ctx = op_context(n, i);
            put(b, "    %s op %u %s",
                i + 1u == done ? "->"
                : i == done    ? "!!"
                               : "  ",
                i, op_name(n->pCommandHistory[i]));
            if (ctx)
                put(b, " \"%s\"", dred_name(NULL, ctx, cbuf, sizeof cbuf));
            put(b, "%s\n",
                i + 1u == done ? "  (last completed)"
                : i == done    ? "  (first not completed)"
                               : "");
        }
    }
    put(b, "  command lists in the DRED history: %u, unfinished: %u%s\n", total, shown,
        shown > DRED_NODES ? " (the first ones are listed)" : "");
}

static void report_allocations(text_buf *b, const char *what,
                               const D3D12_DRED_ALLOCATION_NODE1 *head) {
    u32 n = 0;
    for (const D3D12_DRED_ALLOCATION_NODE1 *a = head; a; a = a->pNext) {
        if (n++ >= DRED_ALLOCATIONS)
            continue;
        char buf[128];
        put(b, "  %s: \"%s\" (allocation type %d)\n", what,
            dred_name(a->ObjectNameA, a->ObjectNameW, buf, sizeof buf), (int)a->AllocationType);
    }
    if (n > DRED_ALLOCATIONS)
        put(b, "  %s: %u more\n", what, n - DRED_ALLOCATIONS);
}

size_t de__d3d12_device_lost_report(de_gpu_device *d, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    d3d12_device *dd = de__d3d12(d);
    text_buf b = {out, cap, 0};
    out[0] = '\0';
    const HRESULT reason = dd->dev ? ID3D12Device_GetDeviceRemovedReason(dd->dev) : S_OK;
    put(&b, "removed reason: %s (HRESULT 0x%08lx)\n", removed_reason_name(reason),
        (unsigned long)reason);
    if (!dd->dred) {
        put(&b, "DRED: off (de_gpu_device_desc.crash_breadcrumbs, or the runtime has no DRED)\n");
        return b.len;
    }
    ID3D12DeviceRemovedExtendedData2 *dred2 = NULL;
    ID3D12DeviceRemovedExtendedData1 *dred1 = NULL;
    if (FAILED(ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12DeviceRemovedExtendedData2,
                                           (void **)&dred2)))
        dred2 = NULL;
    if (!dred2 && FAILED(ID3D12Device_QueryInterface(dd->dev, &IID_ID3D12DeviceRemovedExtendedData1,
                                                     (void **)&dred1)))
        dred1 = NULL;
    if (!dred2 && !dred1) {
        put(&b, "DRED: ID3D12DeviceRemovedExtendedData1 is not available\n");
        return b.len;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs;
    memset(&crumbs, 0, sizeof crumbs);
    HRESULT hr = dred2 ? ID3D12DeviceRemovedExtendedData2_GetAutoBreadcrumbsOutput1(dred2, &crumbs)
                       : ID3D12DeviceRemovedExtendedData1_GetAutoBreadcrumbsOutput1(dred1, &crumbs);
    if (dred2)
        put(&b, "DRED device state: %s\n",
            device_state_name(ID3D12DeviceRemovedExtendedData2_GetDeviceState(dred2)));
    put(&b, "DRED auto-breadcrumbs:\n");
    if (SUCCEEDED(hr))
        report_nodes(&b, crumbs.pHeadAutoBreadcrumbNode);
    else
        put(&b, "  not available (HRESULT 0x%08lx)\n", (unsigned long)hr);

    if (dred2) {
        D3D12_DRED_PAGE_FAULT_OUTPUT2 pf;
        memset(&pf, 0, sizeof pf);
        hr = ID3D12DeviceRemovedExtendedData2_GetPageFaultAllocationOutput2(dred2, &pf);
        if (SUCCEEDED(hr)) {
            put(&b, "DRED page fault: VA 0x%016llx\n", (unsigned long long)pf.PageFaultVA);
            report_allocations(&b, "existing allocation", pf.pHeadExistingAllocationNode);
            report_allocations(&b, "recently freed allocation", pf.pHeadRecentFreedAllocationNode);
        } else {
            put(&b, "DRED page fault: not available (HRESULT 0x%08lx)\n", (unsigned long)hr);
        }
    } else {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf;
        memset(&pf, 0, sizeof pf);
        hr = ID3D12DeviceRemovedExtendedData1_GetPageFaultAllocationOutput1(dred1, &pf);
        if (SUCCEEDED(hr)) {
            put(&b, "DRED page fault: VA 0x%016llx\n", (unsigned long long)pf.PageFaultVA);
            report_allocations(&b, "existing allocation", pf.pHeadExistingAllocationNode);
            report_allocations(&b, "recently freed allocation", pf.pHeadRecentFreedAllocationNode);
        }
    }
    if (dred2)
        ID3D12DeviceRemovedExtendedData2_Release(dred2);
    if (dred1)
        ID3D12DeviceRemovedExtendedData1_Release(dred1);
    return b.len;
}
