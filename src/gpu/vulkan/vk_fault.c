/* vk_fault.c — GPU crash diagnostics on Vulkan (DENG-328; SPEC §18.5).
 *
 * Breadcrumbs: an execution barrier from ALL_COMMANDS to the transfer stage, then vkCmdFillBuffer
 * of the 4-byte value into the ring slot, so the value lands once every earlier command of the
 * list has completed. vkCmdFillBuffer works on every queue family (transfer included) and needs
 * no extension; VK_AMD_buffer_marker is not used (one code path for every driver).
 *
 * Device lost: VK_ERROR_DEVICE_LOST marks the device lost (vk_sync.c, vk_device.c), which the
 * front-end polls through device_lost. The report section asks VK_EXT_device_fault
 * (vkGetDeviceFaultInfoEXT; the backend enables the extension whenever the device has it,
 * DENG-283) for the fault description, the faulting addresses and the vendor fault codes. The
 * vendor binary dump is not fetched (it needs a heap buffer of the driver's size).
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** Address and vendor infos fetched from VK_EXT_device_fault (more are counted). */
#define FAULT_INFOS 16u

/* ---------- Breadcrumbs ---------- */

void de__vk_cmd_breadcrumb(de_gpu_cmd *c, de__gpu_buffer *ring, u64 offset, u32 value) {
    de__vk_device *vk = de__vk(c->device);
    de__vk_cmd *vc = de__vk_cmd_of(c);
    VkMemoryBarrier2 mb;
    memset(&mb, 0, sizeof mb);
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vk->fn.vkCmdPipelineBarrier2(vc->cb, &di);
    vk->fn.vkCmdFillBuffer(vc->cb, DE__GPU_PAYLOAD(de__vk_buffer, ring)->buffer,
                           (VkDeviceSize)offset, 4, value);
}

/* ---------- Device lost ---------- */

bool de__vk_device_lost(de_gpu_device *d) {
    return de__vk_lost(de__vk(d));
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

static const char *address_type_name(VkDeviceFaultAddressTypeEXT t) {
    switch (t) {
    case VK_DEVICE_FAULT_ADDRESS_TYPE_NONE_EXT:
        return "none";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_READ_INVALID_EXT:
        return "invalid read";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_WRITE_INVALID_EXT:
        return "invalid write";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_EXECUTE_INVALID_EXT:
        return "invalid execute";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_UNKNOWN_EXT:
        return "instruction pointer (unknown)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_INVALID_EXT:
        return "instruction pointer (invalid)";
    case VK_DEVICE_FAULT_ADDRESS_TYPE_INSTRUCTION_POINTER_FAULT_EXT:
        return "instruction pointer (fault)";
    default:
        return "unknown";
    }
}

size_t de__vk_device_lost_report(de_gpu_device *d, char *out, size_t cap) {
    if (!out || cap == 0)
        return 0;
    de__vk_device *vk = de__vk(d);
    text_buf b = {out, cap, 0};
    out[0] = '\0';
    put(&b, "device lost flag: %s\n", de__vk_lost(vk) ? "set" : "not set");
    if (!vk->on.device_fault || !vk->fn.vkGetDeviceFaultInfoEXT) {
        put(&b, "VK_EXT_device_fault: not available on this device\n");
        return b.len;
    }
    VkDeviceFaultCountsEXT counts;
    memset(&counts, 0, sizeof counts);
    counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
    VkResult vr = vk->fn.vkGetDeviceFaultInfoEXT(vk->device, &counts, NULL);
    if (vr != VK_SUCCESS) {
        put(&b, "vkGetDeviceFaultInfoEXT (counts): %s\n", de__vk_result_name(vr));
        return b.len;
    }
    const u32 addresses = counts.addressInfoCount, vendors = counts.vendorInfoCount;
    VkDeviceFaultAddressInfoEXT ai[FAULT_INFOS];
    VkDeviceFaultVendorInfoEXT vi[FAULT_INFOS];
    memset(ai, 0, sizeof ai);
    memset(vi, 0, sizeof vi);
    counts.addressInfoCount = addresses < FAULT_INFOS ? addresses : FAULT_INFOS;
    counts.vendorInfoCount = vendors < FAULT_INFOS ? vendors : FAULT_INFOS;
    counts.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info;
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
    info.pAddressInfos = counts.addressInfoCount ? ai : NULL;
    info.pVendorInfos = counts.vendorInfoCount ? vi : NULL;
    vr = vk->fn.vkGetDeviceFaultInfoEXT(vk->device, &counts, &info);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) {
        put(&b, "vkGetDeviceFaultInfoEXT: %s\n", de__vk_result_name(vr));
        return b.len;
    }
    put(&b, "VK_EXT_device_fault: %s\n", info.description[0] ? info.description : "(no text)");
    put(&b, "fault addresses: %u\n", addresses);
    for (u32 i = 0; i < counts.addressInfoCount; ++i) {
        /* The address is exact to within addressPrecision (a power of two). */
        const VkDeviceSize prec = ai[i].addressPrecision ? ai[i].addressPrecision : 1u;
        const VkDeviceSize lo = ai[i].reportedAddress & ~(prec - 1u);
        put(&b, "  %s at 0x%016llx (range 0x%016llx .. 0x%016llx)\n",
            address_type_name(ai[i].addressType), (unsigned long long)ai[i].reportedAddress,
            (unsigned long long)lo, (unsigned long long)(lo + prec - 1u));
    }
    put(&b, "vendor fault infos: %u\n", vendors);
    for (u32 i = 0; i < counts.vendorInfoCount; ++i)
        put(&b, "  %s: code 0x%016llx, data 0x%016llx\n", vi[i].description,
            (unsigned long long)vi[i].vendorFaultCode, (unsigned long long)vi[i].vendorFaultData);
    if (counts.vendorBinarySize || vr == VK_INCOMPLETE)
        put(&b, "(more fault data is available than shown)\n");
    return b.len;
}
