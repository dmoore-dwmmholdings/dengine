/* vk_debug.c — Vulkan debug markers and object names (DENG-326; SPEC §18.3).
 *
 * Both go through VK_EXT_debug_utils, which vk_device.c enables on the instance whenever the
 * loader offers it (not only with the validation layer), so RenderDoc and Nsight see them in
 * every build:
 *   - markers: vkCmdBeginDebugUtilsLabelEXT / vkCmdEndDebugUtilsLabelEXT; the 0xRRGGBB colour
 *     becomes the label colour (0 leaves it to the tool);
 *   - names: vkSetDebugUtilsObjectNameEXT on every object the front-end names (object_name):
 *     buffers, images and their views, samplers, pipelines, fences (timeline semaphores) and
 *     query pools. Command buffers are named by cmd_begin (vk_cmd.c). The validation layer
 *     quotes these names in its messages ("name = ...").
 *
 * Test hook: de__gpu_vk_debug_capture adds a second messenger that keeps the last validation
 * error with its objects' names and the command-buffer labels open at it, which is how the tests
 * see that names and labels reach the layer.
 */
#include "gpu/vulkan/vk_internal.h"

#include <stdio.h>
#include <string.h>

/* ---------- Markers ---------- */

void de__vk_label_color(u32 rgb, float out[4]) {
    if ((rgb & 0xFFFFFFu) == 0) {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    out[0] = (float)((rgb >> 16) & 0xFFu) / 255.0f;
    out[1] = (float)((rgb >> 8) & 0xFFu) / 255.0f;
    out[2] = (float)(rgb & 0xFFu) / 255.0f;
    out[3] = 1.0f;
}

void de__vk_cmd_marker_push(de_gpu_cmd *c, const char *name, u32 color) {
    de__vk_device *vk = de__vk(c->device);
    if (!vk->on.debug_utils || !vk->fn.vkCmdBeginDebugUtilsLabelEXT)
        return;
    VkDebugUtilsLabelEXT l;
    memset(&l, 0, sizeof l);
    l.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
    l.pLabelName = name; /* copied by the call */
    de__vk_label_color(color, l.color);
    vk->fn.vkCmdBeginDebugUtilsLabelEXT(de__vk_cmd_of(c)->cb, &l);
}

void de__vk_cmd_marker_pop(de_gpu_cmd *c) {
    de__vk_device *vk = de__vk(c->device);
    if (!vk->on.debug_utils || !vk->fn.vkCmdEndDebugUtilsLabelEXT)
        return;
    vk->fn.vkCmdEndDebugUtilsLabelEXT(de__vk_cmd_of(c)->cb);
}

/* ---------- Names ---------- */

void de__vk_set_name(de__vk_device *vk, VkObjectType type, u64 handle, const char *name) {
    if (!handle || !name || !name[0] || !vk->on.debug_utils || !vk->fn.vkSetDebugUtilsObjectNameEXT)
        return;
    VkDebugUtilsObjectNameInfoEXT ni;
    memset(&ni, 0, sizeof ni);
    ni.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vk->fn.vkSetDebugUtilsObjectNameEXT(vk->device, &ni);
}

#define H(x) ((u64)(uintptr_t)(x))

void de__vk_object_name(de_gpu_device *d, de__gpu_kind kind, void *rec, const char *name) {
    de__vk_device *vk = de__vk(d);
    switch (kind) {
    case DE__GPU_BUFFER:
        de__vk_set_name(vk, VK_OBJECT_TYPE_BUFFER,
                        H(DE__GPU_PAYLOAD(de__vk_buffer, (de__gpu_buffer *)rec)->buffer), name);
        break;
    case DE__GPU_TEXTURE: {
        de__vk_texture *t = DE__GPU_PAYLOAD(de__vk_texture, (de__gpu_texture *)rec);
        de__vk_set_name(vk, VK_OBJECT_TYPE_IMAGE, H(t->image), name);
        de__vk_set_name(vk, VK_OBJECT_TYPE_IMAGE_VIEW, H(t->sampled_view), name);
        for (u32 m = 0; m < DE_GPU_MAX_MIPS; ++m)
            de__vk_set_name(vk, VK_OBJECT_TYPE_IMAGE_VIEW, H(t->storage_views[m]), name);
        break;
    }
    case DE__GPU_SAMPLER:
        de__vk_set_name(vk, VK_OBJECT_TYPE_SAMPLER,
                        H(DE__GPU_PAYLOAD(de__vk_sampler, (de__gpu_sampler *)rec)->sampler), name);
        break;
    case DE__GPU_PIPELINE:
        de__vk_set_name(vk, VK_OBJECT_TYPE_PIPELINE,
                        H(DE__GPU_PAYLOAD(de__vk_pipeline, (de__gpu_pipeline *)rec)->pipeline),
                        name);
        break;
    case DE__GPU_FENCE:
        de__vk_set_name(vk, VK_OBJECT_TYPE_SEMAPHORE,
                        H(DE__GPU_PAYLOAD(de__vk_fence, (de__gpu_fence *)rec)->semaphore), name);
        break;
    case DE__GPU_QUERY_HEAP:
        de__vk_set_name(vk, VK_OBJECT_TYPE_QUERY_POOL,
                        H(DE__GPU_PAYLOAD(de__vk_query_heap, (de__gpu_query_heap *)rec)->pool),
                        name);
        break;
    default:
        break;
    }
}

/* ---------- Test hook: a capturing messenger ---------- */

typedef struct de__vk_debug_capture {
    VkDebugUtilsMessengerEXT messenger;
    de_mutex lock;
    u32 errors;
    char message[2048];
    char objects[512];
    char labels[512];
} de__vk_debug_capture;

/* Appends `s` to `buf` (cap bytes, always terminated), after `sep` when buf is not empty. */
static void append(char *buf, size_t cap, const char *sep, const char *s) {
    size_t used = strlen(buf);
    if (used + 1 >= cap)
        return;
    snprintf(buf + used, cap - used, "%s%s", used ? sep : "", s ? s : "");
}

static VKAPI_ATTR VkBool32 VKAPI_CALL capture_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    de__vk_debug_capture *cap = (de__vk_debug_capture *)user;
    if (!(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) || !data)
        return VK_FALSE;
    de_mutex_lock(&cap->lock);
    ++cap->errors;
    snprintf(cap->message, sizeof cap->message, "%s", data->pMessage ? data->pMessage : "");
    cap->objects[0] = '\0';
    for (u32 i = 0; i < data->objectCount; ++i)
        if (data->pObjects[i].pObjectName)
            append(cap->objects, sizeof cap->objects, ";", data->pObjects[i].pObjectName);
    cap->labels[0] = '\0';
    /* The layer lists the innermost label first: outermost first reads as a path. */
    for (u32 i = data->cmdBufLabelCount; i-- > 0;)
        append(cap->labels, sizeof cap->labels, "/", data->pCmdBufLabels[i].pLabelName);
    de_mutex_unlock(&cap->lock);
    return VK_FALSE;
}

static bool is_vulkan(de_gpu_device *d) {
    return d && d->vt == &de__gpu_vulkan_vtable;
}

bool de__gpu_vk_debug_capture(de_gpu_device *d, bool on) {
    if (!is_vulkan(d))
        return false;
    de__vk_device *vk = de__vk(d);
    if (!on) {
        de__vk_debug_capture *cap = vk->capture;
        if (cap) {
            vk->fn.vkDestroyDebugUtilsMessengerEXT(vk->instance, cap->messenger, NULL);
            de_mutex_destroy(&cap->lock);
            DE_FREE(d->alloc, cap, sizeof *cap);
            vk->capture = NULL;
        }
        return true;
    }
    if (vk->capture)
        return true;
    if (!vk->on.validation || !vk->on.debug_utils || !vk->fn.vkCreateDebugUtilsMessengerEXT)
        return false;
    de__vk_debug_capture *cap = (de__vk_debug_capture *)DE_ALLOC(
        d->alloc, sizeof *cap, DE_ALIGNOF(de__vk_debug_capture), DE_MEM_RENDER);
    if (!cap)
        return false;
    memset(cap, 0, sizeof *cap);
    de_mutex_init(&cap->lock);
    VkDebugUtilsMessengerCreateInfoEXT ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    ci.pfnUserCallback = capture_callback;
    ci.pUserData = cap;
    if (vk->fn.vkCreateDebugUtilsMessengerEXT(vk->instance, &ci, NULL, &cap->messenger) !=
        VK_SUCCESS) {
        de_mutex_destroy(&cap->lock);
        DE_FREE(d->alloc, cap, sizeof *cap);
        return false;
    }
    vk->capture = cap;
    return true;
}

u32 de__gpu_vk_debug_captured(de_gpu_device *d, char *message, size_t message_cap, char *objects,
                              size_t objects_cap, char *labels, size_t labels_cap) {
    if (message && message_cap)
        message[0] = '\0';
    if (objects && objects_cap)
        objects[0] = '\0';
    if (labels && labels_cap)
        labels[0] = '\0';
    if (!is_vulkan(d) || !de__vk(d)->capture)
        return 0;
    de__vk_debug_capture *cap = de__vk(d)->capture;
    de_mutex_lock(&cap->lock);
    const u32 n = cap->errors;
    if (message && message_cap)
        snprintf(message, message_cap, "%s", cap->message);
    if (objects && objects_cap)
        snprintf(objects, objects_cap, "%s", cap->objects);
    if (labels && labels_cap)
        snprintf(labels, labels_cap, "%s", cap->labels);
    de_mutex_unlock(&cap->lock);
    return n;
}

u64 de__gpu_vk_cmd_buffer(de_gpu_cmd *c) {
    if (!c || !is_vulkan(c->device))
        return 0;
    return H(de__vk_cmd_of(c)->cb);
}
