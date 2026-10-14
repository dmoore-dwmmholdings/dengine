/* mtl_resources.m — buffers, textures and samplers on Metal (DENG-287).
 *
 * Buffers and textures take memory from the front-end's heap manager (de__gpu_mem_alloc) and are
 * placed in its MTLHeaps at the offset it picked; placed (transient) resources use the region's
 * heap and offset instead. Resources inherit the heap's MTLResourceOptions, so hazard tracking is
 * off: the caller orders their use with barriers (DENG-288).
 *
 *   - Buffers: UPLOAD/READBACK buffers are shared storage, `contents` is the persistent mapping.
 *     `gpuAddress` goes into the bindless table.
 *   - Textures: private storage. The sampled view is the texture itself. A STORAGE texture has a
 *     read-write view per mip (all layers; a cube is viewed as a 2D array), or uses the texture
 *     itself when it is a single-mip 2D, 2D-array or 3D texture.
 *   - Samplers: supportArgumentBuffers (they live in the bindless table); compare_reverse_z is
 *     GREATER_EQUAL (reverse-Z depth).
 * Destroy puts the null descriptor back at the resource's bindless slots before it releases the
 * object, so a stale index reads the null resource rather than freed memory.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

/* ---------- Formats and descriptors ---------- */

static const MTLPixelFormat k_formats[DE_FMT_COUNT_] = {
    [DE_FMT_UNKNOWN] = MTLPixelFormatInvalid,
    [DE_FMT_R8_UNORM] = MTLPixelFormatR8Unorm,
    [DE_FMT_RG8_UNORM] = MTLPixelFormatRG8Unorm,
    [DE_FMT_RGBA8_UNORM] = MTLPixelFormatRGBA8Unorm,
    [DE_FMT_RGBA8_SRGB] = MTLPixelFormatRGBA8Unorm_sRGB,
    [DE_FMT_BGRA8_UNORM] = MTLPixelFormatBGRA8Unorm,
    [DE_FMT_BGRA8_SRGB] = MTLPixelFormatBGRA8Unorm_sRGB,
    [DE_FMT_R16_FLOAT] = MTLPixelFormatR16Float,
    [DE_FMT_RG16_FLOAT] = MTLPixelFormatRG16Float,
    [DE_FMT_RGBA16_FLOAT] = MTLPixelFormatRGBA16Float,
    [DE_FMT_R32_UINT] = MTLPixelFormatR32Uint,
    [DE_FMT_RG32_UINT] = MTLPixelFormatRG32Uint,
    [DE_FMT_R32_FLOAT] = MTLPixelFormatR32Float,
    [DE_FMT_RG32_FLOAT] = MTLPixelFormatRG32Float,
    [DE_FMT_RGBA32_FLOAT] = MTLPixelFormatRGBA32Float,
    [DE_FMT_R11G11B10_FLOAT] = MTLPixelFormatRG11B10Float,
    [DE_FMT_RGB10A2_UNORM] = MTLPixelFormatRGB10A2Unorm,
    [DE_FMT_D32_FLOAT] = MTLPixelFormatDepth32Float,
    [DE_FMT_D32_FLOAT_S8] = MTLPixelFormatDepth32Float_Stencil8,
    [DE_FMT_BC1_SRGB] = MTLPixelFormatBC1_RGBA_sRGB,
    [DE_FMT_BC3_SRGB] = MTLPixelFormatBC3_RGBA_sRGB,
    [DE_FMT_BC4_UNORM] = MTLPixelFormatBC4_RUnorm,
    [DE_FMT_BC5_UNORM] = MTLPixelFormatBC5_RGUnorm,
    [DE_FMT_BC6H_UFLOAT] = MTLPixelFormatBC6H_RGBUfloat,
    [DE_FMT_BC7_UNORM] = MTLPixelFormatBC7_RGBAUnorm,
    [DE_FMT_BC7_SRGB] = MTLPixelFormatBC7_RGBAUnorm_sRGB,
};

u32 de__mtl_pixel_format(de_gpu_format f) {
    return (u32)f < (u32)DE_FMT_COUNT_ ? (u32)k_formats[f] : (u32)MTLPixelFormatInvalid;
}

static MTLTextureType texture_type(const de_gpu_texture_desc *desc) {
    switch (desc->dim) {
    case DE_TEX_2D_ARRAY:
        return MTLTextureType2DArray;
    case DE_TEX_3D:
        return MTLTextureType3D;
    case DE_TEX_CUBE:
        return desc->depth_or_layers > 6 ? MTLTextureTypeCubeArray : MTLTextureTypeCube;
    case DE_TEX_2D:
    default:
        return desc->sample_count > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    }
}

/* The descriptor of a texture (desc with the front-end defaults applied), or nil with the error
 * context set and its code in *err. */
static MTLTextureDescriptor *texture_descriptor(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                                MTLResourceOptions options, de_result *err) {
    id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
    const MTLPixelFormat fmt = (MTLPixelFormat)de__mtl_pixel_format(desc->format);
    const char *name = desc->name ? desc->name : "";
    if (fmt == MTLPixelFormatInvalid) {
        *err = de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "metal: texture \"%s\": format %s has no Metal format", name,
                            de__gpu_format_name(desc->format));
        return nil;
    }
    if (de__gpu_format_is_compressed(desc->format) && !dev.supportsBCTextureCompression) {
        *err = de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "metal: texture \"%s\": the device has no BC texture compression (%s)",
                            name, de__gpu_format_name(desc->format));
        return nil;
    }
    if (desc->sample_count > 1 && ![dev supportsTextureSampleCount:desc->sample_count]) {
        *err = de__gpu_fail(d, DE_ERR_UNSUPPORTED,
                            "metal: texture \"%s\": %u samples are not supported", name,
                            desc->sample_count);
        return nil;
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = texture_type(desc);
    td.pixelFormat = fmt;
    td.width = desc->width;
    td.height = desc->height;
    td.depth = desc->dim == DE_TEX_3D ? desc->depth_or_layers : 1;
    td.arrayLength = desc->dim == DE_TEX_2D_ARRAY ? desc->depth_or_layers
                     : desc->dim == DE_TEX_CUBE   ? desc->depth_or_layers / 6
                                                  : 1;
    td.mipmapLevelCount = desc->mip_count;
    td.sampleCount = desc->sample_count;
    MTLTextureUsage usage = 0;
    if (desc->usage & DE_USAGE_SAMPLED)
        usage |= MTLTextureUsageShaderRead;
    if (desc->usage & DE_USAGE_STORAGE)
        usage |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    if (desc->usage & (DE_USAGE_RENDER_TARGET | DE_USAGE_DEPTH))
        usage |= MTLTextureUsageRenderTarget;
    if (usage == 0) /* copy-only textures */
        usage = MTLTextureUsageShaderRead;
    td.usage = usage;
    td.resourceOptions = options;
    return td;
}

/* ---------- Memory requirements ---------- */

de_result de__mtl_buffer_requirements(de_gpu_device *d, const de_gpu_buffer_desc *desc,
                                      de__gpu_mem_req *out) {
    mtl_device *m = de__mtl_dev(d);
    id<MTLDevice> dev = (__bridge id<MTLDevice>)m->device;
    const MTLSizeAndAlign sa = [dev
        heapBufferSizeAndAlignWithLength:(NSUInteger)desc->size
                                 options:(MTLResourceOptions)m->resource_options[desc->memory]];
    out->size = sa.size;
    out->align = sa.align;
    out->class_mask = 1u << (u32)desc->memory; /* base class ids equal de_gpu_memory values */
    return DE_OK;
}

de_result de__mtl_texture_requirements(de_gpu_device *d, const de_gpu_texture_desc *desc,
                                       de__gpu_mem_req *out) {
    @autoreleasepool {
        mtl_device *m = de__mtl_dev(d);
        de_result err = DE_OK;
        MTLTextureDescriptor *td = texture_descriptor(
            d, desc, (MTLResourceOptions)m->resource_options[DE__GPU_MEM_CLASS_GPU_ONLY], &err);
        if (td == nil)
            return err;
        /* Serialised: GitHub's paravirtual Metal driver crashed (SEGV in objc_msgSend inside
         * AppleParavirtGPUMetalIOGPUFamily) with this query on 8 threads at once (DENG-288,
         * gpu_metal.threads_create_destroy under ASan). Textures are made at load time. */
        de_mutex_lock(&m->size_lock);
        const MTLSizeAndAlign sa =
            [(__bridge id<MTLDevice>)m->device heapTextureSizeAndAlignWithDescriptor:td];
        de_mutex_unlock(&m->size_lock);
        out->size = sa.size;
        out->align = sa.align;
        out->class_mask = 1u << DE__GPU_MEM_CLASS_GPU_ONLY;
        return DE_OK;
    }
}

/* ---------- Buffers ---------- */

static id<MTLHeap> heap_object(de__gpu_heap *heap) {
    return (__bridge id<MTLHeap>)DE__GPU_PAYLOAD(mtl_heap, heap)->heap;
}

static de_result place_buffer(de_gpu_device *d, de__gpu_buffer *b, const de_gpu_buffer_desc *desc,
                              de__gpu_heap *heap, u64 offset) {
    @autoreleasepool {
        id<MTLHeap> h = heap_object(heap);
        id<MTLBuffer> buf = [h newBufferWithLength:(NSUInteger)desc->size
                                           options:h.resourceOptions
                                            offset:(NSUInteger)offset];
        if (buf == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                                "metal: buffer \"%s\" (%llu bytes) at offset %llu of heap \"%s\"",
                                b->name, (unsigned long long)desc->size, (unsigned long long)offset,
                                h.label.UTF8String);
        if (b->name[0])
            buf.label = [NSString stringWithUTF8String:b->name];
        mtl_buffer *mb = DE__GPU_PAYLOAD(mtl_buffer, b);
        mb->gpu_address = buf.gpuAddress;
        if (b->memory != DE_MEM_GPU_ONLY)
            b->mapped = buf.contents;
        mb->buffer = (void *)CFBridgingRetain(buf);
        return DE_OK;
    }
}

/* A shared buffer outside the heaps, for devices without shared-storage heaps. */
static de_result standalone_buffer(de_gpu_device *d, de__gpu_buffer *b,
                                   const de_gpu_buffer_desc *desc) {
    @autoreleasepool {
        mtl_device *m = de__mtl_dev(d);
        id<MTLBuffer> buf = [(__bridge id<MTLDevice>)m->device
            newBufferWithLength:(NSUInteger)desc->size
                        options:(MTLResourceOptions)m->resource_options[desc->memory]];
        if (buf == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: buffer \"%s\" (%llu bytes)",
                                b->name, (unsigned long long)desc->size);
        if (b->name[0])
            buf.label = [NSString stringWithUTF8String:b->name];
        mtl_buffer *mb = DE__GPU_PAYLOAD(mtl_buffer, b);
        mb->gpu_address = buf.gpuAddress;
        b->mapped = buf.contents;
        mb->buffer = (void *)CFBridgingRetain(buf);
        return DE_OK;
    }
}

de_result de__mtl_buffer_create(de_gpu_device *d, de__gpu_buffer *b,
                                const de_gpu_buffer_desc *desc) {
    mtl_buffer *mb = DE__GPU_PAYLOAD(mtl_buffer, b);
    if (desc->memory != DE_MEM_GPU_ONLY && !de__mtl_dev(d)->shared_heaps)
        return standalone_buffer(d, b, desc);
    de__gpu_mem_req req;
    de_result r = de__mtl_buffer_requirements(d, desc, &req);
    if (r != DE_OK)
        return r;
    r = de__gpu_mem_alloc(d, (u32)desc->memory, req.size, req.align, DE_MEM_RENDER, &mb->alloc);
    if (r != DE_OK)
        return r;
    r = place_buffer(d, b, desc, mb->alloc.heap, mb->alloc.offset);
    if (r != DE_OK)
        de__gpu_mem_free(d, &mb->alloc);
    return r;
}

de_result de__mtl_buffer_create_placed(de_gpu_device *d, de__gpu_buffer *b,
                                       const de_gpu_buffer_desc *desc, de__gpu_heap *heap,
                                       u64 offset) {
    return place_buffer(d, b, desc, heap, offset);
}

void de__mtl_buffer_destroy(de_gpu_device *d, de__gpu_buffer *b) {
    mtl_buffer *mb = DE__GPU_PAYLOAD(mtl_buffer, b);
    if (b->bindless)
        de__mtl_bindless_clear(d, DE__GPU_BINDLESS_BUFFER, b->bindless);
    if (mb->buffer) {
        @autoreleasepool {
            (void)CFBridgingRelease(mb->buffer);
        }
    }
    mb->buffer = NULL;
    mb->gpu_address = 0;
    b->mapped = NULL;
    de__gpu_mem_free(d, &mb->alloc); /* zero for placed buffers: ignored */
}

/* ---------- Textures ---------- */

/* Creates the per-mip storage views a STORAGE texture needs. */
static de_result storage_views(de_gpu_device *d, de__gpu_texture *t, id<MTLTexture> tex) {
    if (!(t->usage & DE_USAGE_STORAGE))
        return DE_OK;
    const bool cube = t->dim == DE_TEX_CUBE;
    if (t->mip_count == 1 && !cube)
        return DE_OK; /* the texture itself is the storage view of mip 0 */
    mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
    const MTLTextureType type = cube || t->dim == DE_TEX_2D_ARRAY ? MTLTextureType2DArray
                                : t->dim == DE_TEX_3D             ? MTLTextureType3D
                                                                  : MTLTextureType2D;
    const NSUInteger slices = t->dim == DE_TEX_3D ? 1 : t->depth_or_layers;
    for (u32 m = 0; m < t->mip_count; ++m) {
        id<MTLTexture> v = [tex newTextureViewWithPixelFormat:tex.pixelFormat
                                                  textureType:type
                                                       levels:NSMakeRange(m, 1)
                                                       slices:NSMakeRange(0, slices)];
        if (v == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                                "metal: storage view of mip %u of texture \"%s\"", m, t->name);
        if (t->name[0])
            v.label = [NSString stringWithFormat:@"%s mip %u", t->name, m];
        mt->views[m] = (void *)CFBridgingRetain(v);
    }
    return DE_OK;
}

static void release_texture_objects(mtl_texture *mt) {
    @autoreleasepool {
        for (u32 m = 0; m < DE_GPU_MAX_MIPS; ++m) {
            if (mt->views[m])
                (void)CFBridgingRelease(mt->views[m]);
            mt->views[m] = NULL;
        }
        if (mt->texture)
            (void)CFBridgingRelease(mt->texture);
        mt->texture = NULL;
    }
}

static de_result place_texture(de_gpu_device *d, de__gpu_texture *t,
                               const de_gpu_texture_desc *desc, de__gpu_heap *heap, u64 offset) {
    @autoreleasepool {
        id<MTLHeap> h = heap_object(heap);
        de_result err = DE_OK;
        MTLTextureDescriptor *td = texture_descriptor(d, desc, h.resourceOptions, &err);
        if (td == nil)
            return err;
        id<MTLTexture> tex = [h newTextureWithDescriptor:td offset:(NSUInteger)offset];
        if (tex == nil)
            return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY,
                                "metal: texture \"%s\" (%ux%u %s) at offset %llu of heap \"%s\"",
                                t->name, t->width, t->height, de__gpu_format_name(t->format),
                                (unsigned long long)offset, h.label.UTF8String);
        if (t->name[0])
            tex.label = [NSString stringWithUTF8String:t->name];
        mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
        mt->texture = (void *)CFBridgingRetain(tex);
        de_result r = storage_views(d, t, tex);
        if (r != DE_OK)
            release_texture_objects(mt);
        return r;
    }
}

de_result de__mtl_texture_create(de_gpu_device *d, de__gpu_texture *t,
                                 const de_gpu_texture_desc *desc) {
    mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
    de__gpu_mem_req req;
    de_result r = de__mtl_texture_requirements(d, desc, &req);
    if (r != DE_OK)
        return r;
    r = de__gpu_mem_alloc(d, DE__GPU_MEM_CLASS_GPU_ONLY, req.size, req.align, DE_MEM_RENDER,
                          &mt->alloc);
    if (r != DE_OK)
        return r;
    r = place_texture(d, t, desc, mt->alloc.heap, mt->alloc.offset);
    if (r != DE_OK)
        de__gpu_mem_free(d, &mt->alloc);
    return r;
}

de_result de__mtl_texture_create_placed(de_gpu_device *d, de__gpu_texture *t,
                                        const de_gpu_texture_desc *desc, de__gpu_heap *heap,
                                        u64 offset) {
    return place_texture(d, t, desc, heap, offset);
}

void de__mtl_texture_destroy(de_gpu_device *d, de__gpu_texture *t) {
    mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
    if (t->sampled)
        de__mtl_bindless_clear(d, DE__GPU_BINDLESS_SAMPLED_TEXTURE, t->sampled);
    for (u32 m = 0; m < DE_GPU_MAX_MIPS; ++m)
        if (t->storage[m])
            de__mtl_bindless_clear(d, DE__GPU_BINDLESS_STORAGE_TEXTURE, t->storage[m]);
    release_texture_objects(mt);
    de__gpu_mem_free(d, &mt->alloc); /* zero for placed and external textures: ignored */
}

/* ---------- Samplers ---------- */

static MTLSamplerAddressMode address_mode(de_gpu_address a, bool border_color) {
    switch (a) {
    case DE_ADDR_CLAMP:
        return MTLSamplerAddressModeClampToEdge;
    case DE_ADDR_MIRROR:
        return MTLSamplerAddressModeMirrorRepeat;
    case DE_ADDR_BORDER:
        return border_color ? MTLSamplerAddressModeClampToBorderColor /* transparent black */
                            : MTLSamplerAddressModeClampToZero;
    case DE_ADDR_WRAP:
    default:
        return MTLSamplerAddressModeRepeat;
    }
}

/* Sampler states are created and released under the device's sampler lock: the Metal debug
 * layer (MTL_DEBUG_LAYER=1) keeps argument-buffer samplers in a hash map that is not
 * thread-safe, and concurrent create/release crashes inside it (seen on macOS 26, MetalTools
 * MTLSamplerDescriptorHashMap::remove). Samplers are few and made at load time. */
static void *make_sampler(de_gpu_device *d, const de_gpu_sampler_desc *desc) {
    @autoreleasepool {
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = desc->min == DE_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear
                                                     : MTLSamplerMinMagFilterNearest;
        sd.magFilter = desc->mag == DE_FILTER_LINEAR ? MTLSamplerMinMagFilterLinear
                                                     : MTLSamplerMinMagFilterNearest;
        sd.mipFilter =
            desc->mip == DE_FILTER_LINEAR ? MTLSamplerMipFilterLinear : MTLSamplerMipFilterNearest;
        const bool border = de__mtl_dev(d)->border_color;
        sd.sAddressMode = address_mode(desc->u, border);
        sd.tAddressMode = address_mode(desc->v, border);
        sd.rAddressMode = address_mode(desc->w, border);
        if (border &&
            (desc->u == DE_ADDR_BORDER || desc->v == DE_ADDR_BORDER || desc->w == DE_ADDR_BORDER))
            sd.borderColor = MTLSamplerBorderColorTransparentBlack;
        NSUInteger aniso = desc->max_aniso >= 1.0f ? (NSUInteger)desc->max_aniso : 1u;
        sd.maxAnisotropy = aniso > 16u ? 16u : aniso;
        if (desc->compare_reverse_z)
            sd.compareFunction = MTLCompareFunctionGreaterEqual; /* reverse-Z: nearer is larger */
        sd.supportArgumentBuffers = YES;
        id<MTLSamplerState> st = [dev newSamplerStateWithDescriptor:sd];
        return st != nil ? (void *)CFBridgingRetain(st) : NULL;
    }
}

de_result de__mtl_sampler_create(de_gpu_device *d, de__gpu_sampler *s,
                                 const de_gpu_sampler_desc *desc) {
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->sampler_lock);
    void *st = make_sampler(d, desc);
    de_mutex_unlock(&m->sampler_lock);
    if (!st)
        return de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, "metal: sampler state");
    DE__GPU_PAYLOAD(mtl_sampler, s)->sampler = st;
    return DE_OK;
}

void de__mtl_sampler_destroy(de_gpu_device *d, de__gpu_sampler *s) {
    mtl_device *m = de__mtl_dev(d);
    mtl_sampler *ms = DE__GPU_PAYLOAD(mtl_sampler, s);
    if (s->bindless)
        de__mtl_bindless_clear(d, DE__GPU_BINDLESS_SAMPLER, s->bindless);
    if (ms->sampler) {
        de_mutex_lock(&m->sampler_lock);
        @autoreleasepool {
            (void)CFBridgingRelease(ms->sampler);
        }
        de_mutex_unlock(&m->sampler_lock);
    }
    ms->sampler = NULL;
}

/* ---------- Accessors (DENG-288, tests) ---------- */

void *de__mtl_buffer_object(de_gpu_device *d, de_gpu_buffer h) {
    de__gpu_buffer *b = de__mtl_device_object(d) ? de__gpu_get_buffer(d, h) : NULL;
    return b ? DE__GPU_PAYLOAD(mtl_buffer, b)->buffer : NULL;
}

void *de__mtl_texture_object(de_gpu_device *d, de_gpu_texture h, i32 mip) {
    de__gpu_texture *t = de__mtl_device_object(d) ? de__gpu_get_texture(d, h) : NULL;
    if (!t)
        return NULL;
    mtl_texture *mt = DE__GPU_PAYLOAD(mtl_texture, t);
    if (mip < 0)
        return mt->texture;
    if ((u32)mip >= t->mip_count)
        return NULL;
    return mt->views[mip] ? mt->views[mip] : mt->texture;
}

void *de__mtl_sampler_object(de_gpu_device *d, de_gpu_sampler h) {
    de__gpu_sampler *s = de__mtl_device_object(d) ? de__gpu_get_sampler(d, h) : NULL;
    return s ? DE__GPU_PAYLOAD(mtl_sampler, s)->sampler : NULL;
}

static bool resource_heap(void *res, void **heap, u64 *offset) {
    if (!res)
        return false;
    @autoreleasepool {
        id<MTLResource> r = (__bridge id<MTLResource>)res;
        id<MTLHeap> h = r.heap; /* kept alive by its de__gpu_heap payload */
        if (h == nil)
            return false;
        if (heap)
            *heap = (__bridge void *)h;
        if (offset)
            *offset = r.heapOffset;
        return true;
    }
}

bool de__mtl_buffer_placement(de_gpu_device *d, de_gpu_buffer b, void **heap, u64 *offset) {
    return resource_heap(de__mtl_buffer_object(d, b), heap, offset);
}

bool de__mtl_texture_placement(de_gpu_device *d, de_gpu_texture t, void **heap, u64 *offset) {
    return resource_heap(de__mtl_texture_object(d, t, -1), heap, offset);
}
