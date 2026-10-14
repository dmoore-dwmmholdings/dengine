/* mtl_pipeline.m — render, mesh and compute pipeline states on Metal (DENG-325, SPEC §12.1).
 *
 * Shaders: each blob is a cooked metallib (S09 importer, one entry point per blob). It becomes an
 * MTLLibrary with newLibraryWithData:, cached per device by the blob's hash, so pipelines that
 * share a shader share its library. The function is the blob's entry name; a library with one
 * function also accepts another name (or none), as Vulkan accepts any name for a one-entry module.
 * The function's type must be the stage's (vertex, fragment, object, mesh, kernel).
 *
 * Fixed state (Decision: Pipelines share fixed state on every backend):
 *   - Front faces are counter-clockwise in clip space (y up). Metal keeps clip-space y up and its
 *     viewport maps y down, so front faces are MTLWindingCounterClockwise, as the shared triangle
 *     test checks; the cull mode and winding are set at draw time (mtl_draw.m).
 *   - Depth: one MTLDepthStencilState per pipeline, GreaterEqual with `depth_test` (reverse-Z),
 *     Always without; writes with `depth_write`. No stencil, one sample, triangles.
 *   - Blend (color; alpha): OPAQUE off; ALPHA SourceAlpha, OneMinusSourceAlpha; One,
 *     OneMinusSourceAlpha; PREMUL One, OneMinusSourceAlpha; ADD One, One. Op Add, all channels.
 *   - Render pipelines support indirect command buffers (indirect-count draws, mtl_icb.m).
 *   - Mesh pipelines: object (optional) + mesh + fragment functions, payload 16 KiB (the Metal
 *     maximum: the cooked reflection does not give the payload size).
 *   - COMPUTE and MESH pipelines need their threadgroup sizes in the desc (from the cooked
 *     reflection): MSL kernels do not carry them. The total must fit the pipeline's
 *     maxTotalThreadsPer*Threadgroup.
 *
 * Push constants: setBytes at buffer index 1 (push_constants.slang). Metal's binding reflection
 * (MTLPipelineOptionBindingInfo) gives each stage's binding at index 1: the stages that read it,
 * its size (setBytes passes at least that many bytes: MSL rounds a struct up to its alignment, so
 * a 24-byte block is a 32-byte struct) and the bytes its members cover, which must fit
 * `push_constant_bytes` (the check the other backends make on their blobs).
 *
 * Binary archive (mtl_binary_archive.m): when the device has one, each creation first asks for
 * the archived binary (MTLPipelineOptionFailOnBinaryArchiveMiss); on a miss it compiles and adds
 * the pipeline's functions to the archive.
 *
 * Destruction: command buffers do not retain pipeline states (DENG-288), so pipeline_destroy runs
 * from the front-end's deferred destruction, after the GPU is done with every submission made
 * before de_gpu_pipeline_destroy.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "gpu/metal/mtl_internal.h"

#include <string.h>

#define F "de_gpu_pipeline_create"
/* The largest object-to-mesh payload Metal allows. */
#define PAYLOAD_BYTES 16384u

static void release(void **p) {
    if (*p)
        (void)CFBridgingRelease(*p);
    *p = NULL;
}

static const char *pname(const de__gpu_pipeline *p) {
    return p->name[0] ? p->name : "(unnamed)";
}

/* ---------- Libraries and functions ---------- */

static bool is_metallib(const de_gpu_shader_blob *b) {
    return b->size >= 4 && memcmp(b->code, "MTLB", 4) == 0;
}

static id<MTLLibrary> library_of(de_gpu_device *d, const de__gpu_pipeline *p, const char *stage,
                                 const de_gpu_shader_blob *b) {
    mtl_device *m = de__mtl_dev(d);
    if (!is_metallib(b)) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                     F "(\"%s\"): %s blob (%llu bytes) is not a metallib (no MTLB header)",
                     pname(p), stage, (unsigned long long)b->size);
        return nil;
    }
    const u64 key = de_hash64(b->code, (size_t)b->size, b->size);
    NSNumber *k = @(key);
    de_mutex_lock(&m->library_lock);
    if (!m->libraries)
        m->libraries = (void *)CFBridgingRetain([NSMutableDictionary new]);
    NSMutableDictionary *cache = (__bridge NSMutableDictionary *)m->libraries;
    id<MTLLibrary> lib = cache[k];
    de_mutex_unlock(&m->library_lock);
    if (lib != nil)
        return lib;

    dispatch_data_t data =
        dispatch_data_create(b->code, (size_t)b->size, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *err = nil;
    lib = [(__bridge id<MTLDevice>)m->device newLibraryWithData:data error:&err];
    if (lib == nil) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, F "(\"%s\"): %s metallib does not load: %s",
                     pname(p), stage,
                     err ? err.localizedDescription.UTF8String : "no reason given");
        return nil;
    }
    de_mutex_lock(&m->library_lock);
    id<MTLLibrary> other = cache[k]; /* another thread may have loaded it meanwhile */
    if (other != nil)
        lib = other;
    else
        cache[k] = lib;
    de_mutex_unlock(&m->library_lock);
    return lib;
}

static const char *function_type_name(MTLFunctionType t) {
    switch (t) {
    case MTLFunctionTypeVertex:
        return "vertex";
    case MTLFunctionTypeFragment:
        return "fragment";
    case MTLFunctionTypeKernel:
        return "kernel";
    case MTLFunctionTypeObject:
        return "object";
    case MTLFunctionTypeMesh:
        return "mesh";
    default:
        return "other";
    }
}

/* The function of a blob for `stage` (vs, ps, as, ms, cs), of type `want`; nil (and the error
 * set) when there is none. */
static id<MTLFunction> function_of(de_gpu_device *d, const de__gpu_pipeline *p, const char *stage,
                                   const de_gpu_shader_blob *b, MTLFunctionType want) {
    id<MTLLibrary> lib = library_of(d, p, stage, b);
    if (lib == nil)
        return nil;
    NSArray<NSString *> *names = lib.functionNames;
    NSString *name = nil;
    if (b->entry && b->entry[0])
        name = [NSString stringWithUTF8String:b->entry];
    if (name == nil || ![names containsObject:name]) {
        /* One function: it is the entry point, whatever its name in MSL. */
        if (names.count == 1) {
            name = names[0];
        } else {
            NSString *list = [names componentsJoinedByString:@"\", \""];
            de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                         F "(\"%s\"): %s metallib has no function \"%s\" (it has \"%s\")",
                         pname(p), stage, b->entry ? b->entry : "", list.UTF8String);
            return nil;
        }
    }
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (fn == nil) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, F "(\"%s\"): %s function \"%s\" did not load",
                     pname(p), stage, name.UTF8String);
        return nil;
    }
    if (fn.functionType != want) {
        de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                     F "(\"%s\"): %s \"%s\" is a %s function, not a %s function", pname(p), stage,
                     name.UTF8String, function_type_name(fn.functionType),
                     function_type_name(want));
        return nil;
    }
    return fn;
}

/* ---------- Push constants (binding reflection) ---------- */

static u32 type_bytes(MTLDataType t) {
    switch (t) {
    case MTLDataTypeChar:
    case MTLDataTypeUChar:
    case MTLDataTypeBool:
        return 1;
    case MTLDataTypeChar2:
    case MTLDataTypeUChar2:
    case MTLDataTypeBool2:
    case MTLDataTypeShort:
    case MTLDataTypeUShort:
    case MTLDataTypeHalf:
        return 2;
    case MTLDataTypeChar3:
    case MTLDataTypeUChar3:
    case MTLDataTypeBool3:
        return 3;
    case MTLDataTypeHalf3:
    case MTLDataTypeShort3:
    case MTLDataTypeUShort3:
        return 6;
    case MTLDataTypeFloat2:
    case MTLDataTypeInt2:
    case MTLDataTypeUInt2:
    case MTLDataTypeHalf4:
    case MTLDataTypeShort4:
    case MTLDataTypeUShort4:
    case MTLDataTypeLong:
    case MTLDataTypeULong:
    case MTLDataTypePointer:
        return 8;
    case MTLDataTypeFloat3:
    case MTLDataTypeInt3:
    case MTLDataTypeUInt3:
        return 12;
    case MTLDataTypeFloat4:
    case MTLDataTypeInt4:
    case MTLDataTypeUInt4:
    case MTLDataTypeFloat2x2:
    case MTLDataTypeLong2:
    case MTLDataTypeULong2:
        return 16;
    case MTLDataTypeFloat3x2:
        return 24;
    case MTLDataTypeFloat2x3:
    case MTLDataTypeFloat2x4:
    case MTLDataTypeFloat4x2:
    case MTLDataTypeLong4:
    case MTLDataTypeULong4:
        return 32;
    case MTLDataTypeFloat3x3:
    case MTLDataTypeFloat3x4:
        return 48;
    case MTLDataTypeFloat4x3:
    case MTLDataTypeFloat4x4:
        return 64;
    default:
        return 4; /* 4-byte scalars, and anything unusual counted as one word */
    }
}

static u32 struct_used(MTLStructType *st);

static u32 array_used(MTLArrayType *at) {
    if (at == nil || at.arrayLength == 0)
        return 0;
    u32 elem;
    switch (at.elementType) {
    case MTLDataTypeStruct:
        elem = struct_used(at.elementStructType);
        break;
    case MTLDataTypeArray:
        elem = array_used(at.elementArrayType);
        break;
    default:
        elem = type_bytes(at.elementType);
        break;
    }
    return (u32)((at.arrayLength - 1u) * at.stride) + elem;
}

/* The bytes a struct's members cover: the end of the last one. */
static u32 struct_used(MTLStructType *st) {
    u32 used = 0;
    for (MTLStructMember *mb in st.members) {
        u32 size;
        switch (mb.dataType) {
        case MTLDataTypeStruct:
            size = struct_used(mb.structType);
            break;
        case MTLDataTypeArray:
            size = array_used(mb.arrayType);
            break;
        default:
            size = type_bytes(mb.dataType);
            break;
        }
        const u32 end = (u32)mb.offset + size;
        if (end > used)
            used = end;
    }
    return used;
}

typedef struct push_info {
    u32 stages;   /* DE__MTL_PUSH_* with a binding at index 1 */
    u32 size;     /* largest binding size */
    u32 used;     /* largest member coverage */
    const char *stage; /* the stage with the largest coverage */
} push_info;

static void push_scan(push_info *pi, NSArray<id<MTLBinding>> *bindings, u32 stage_bit,
                      const char *stage) {
    for (id<MTLBinding> b in bindings) {
        if (b.type != MTLBindingTypeBuffer || b.index != 1)
            continue;
        id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
        const u32 size = (u32)bb.bufferDataSize;
        const u32 used = bb.bufferStructType ? struct_used(bb.bufferStructType) : size;
        pi->stages |= stage_bit;
        if (size > pi->size)
            pi->size = size;
        if (used > pi->used) {
            pi->used = used;
            pi->stage = stage;
        }
    }
}

/* Checks the shaders' push constants against the desc and fills the payload's push fields. */
static de_result push_check(de_gpu_device *d, const de__gpu_pipeline *p, mtl_pipeline *mp,
                            const push_info *pi) {
    if (pi->used > p->push_constant_bytes)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            F "(\"%s\"): the %s shader uses %u bytes of push constants, but "
                              "push_constant_bytes is %u",
                            pname(p), pi->stage, pi->used, p->push_constant_bytes);
    u32 bytes = pi->size > p->push_constant_bytes ? pi->size : p->push_constant_bytes;
    if (bytes > DE_GPU_MAX_PUSH_CONSTANT_BYTES)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            F "(\"%s\"): the shaders' push-constant block is %u bytes; the limit "
                              "is %u",
                            pname(p), bytes, DE_GPU_MAX_PUSH_CONSTANT_BYTES);
    mp->push_stages = pi->stages;
    mp->push_bytes = pi->stages ? bytes : 0u;
    return DE_OK;
}

/* ---------- Creation through the binary archive ---------- */

typedef id (^make_fn)(MTLPipelineOption opts, MTLAutoreleasedRenderPipelineReflection *rr,
                      MTLAutoreleasedComputePipelineReflection *cr, NSError **err);

/* Makes a pipeline state through `make`, from the archive when it has it (`descriptor` names the
 * archive in binaryArchives); a miss compiles the pipeline into the archive first. */
static id make_state(de_gpu_device *d, id descriptor, bool archive_ok, make_fn make,
                     MTLAutoreleasedRenderPipelineReflection *rr,
                     MTLAutoreleasedComputePipelineReflection *cr, NSError **err, bool *hit) {
    const u64 t0 = de_time_now_ns();
    /* MTLPipelineOptionBindingInfo (1 << 0): spelled as the value, because the macOS 14 SDK (CI's
     * Xcode 15.4) only has its deprecated name, MTLPipelineOptionArgumentInfo. */
    const MTLPipelineOption opts = (MTLPipelineOption)(1u << 0);
    const MTLPipelineOption from_archive = opts | MTLPipelineOptionFailOnBinaryArchiveMiss;
    const bool archived = archive_ok && de__mtl_archive_list(d) != NULL;
    id state = nil;
    *hit = false;
    if (archived) {
        NSError *miss = nil;
        state = make(from_archive, rr, cr, &miss);
        if (state != nil) {
            *hit = true;
            de__mtl_archive_hit(d);
        } else if (de__mtl_archive_add(d, (__bridge void *)descriptor)) {
            /* Compiled into the archive: take it from there rather than compile it again. */
            state = make(from_archive, rr, cr, &miss);
        }
    }
    if (state == nil)
        state = make(opts, rr, cr, err);
    mtl_device *m = de__mtl_dev(d);
    de_mutex_lock(&m->archive_lock);
    m->pipeline_ns += de_time_now_ns() - t0;
    de_mutex_unlock(&m->archive_lock);
    return state;
}

/* ---------- Fixed state ---------- */

static void set_blend(MTLRenderPipelineColorAttachmentDescriptor *a, de_gpu_blend blend) {
    a.writeMask = MTLColorWriteMaskAll;
    a.rgbBlendOperation = MTLBlendOperationAdd;
    a.alphaBlendOperation = MTLBlendOperationAdd;
    switch (blend) {
    case DE_BLEND_ALPHA:
        a.blendingEnabled = YES;
        a.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        a.sourceAlphaBlendFactor = MTLBlendFactorOne;
        a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
    case DE_BLEND_PREMUL:
        a.blendingEnabled = YES;
        a.sourceRGBBlendFactor = MTLBlendFactorOne;
        a.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        a.sourceAlphaBlendFactor = MTLBlendFactorOne;
        a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
    case DE_BLEND_ADD:
        a.blendingEnabled = YES;
        a.sourceRGBBlendFactor = MTLBlendFactorOne;
        a.destinationRGBBlendFactor = MTLBlendFactorOne;
        a.sourceAlphaBlendFactor = MTLBlendFactorOne;
        a.destinationAlphaBlendFactor = MTLBlendFactorOne;
        break;
    default:
        a.blendingEnabled = NO;
        break;
    }
}

/* Color and depth formats of a render or mesh descriptor (both have the same properties). */
#define SET_TARGETS(rd, p, desc)                                                                   \
    do {                                                                                           \
        for (u32 i_ = 0; i_ < (p)->color_count; ++i_) {                                            \
            (rd).colorAttachments[i_].pixelFormat =                                                \
                (MTLPixelFormat)de__mtl_pixel_format((p)->color_formats[i_]);                      \
            set_blend((rd).colorAttachments[i_], (desc)->blend[i_]);                               \
        }                                                                                          \
        if ((p)->depth_format != DE_FMT_UNKNOWN) {                                                 \
            (rd).depthAttachmentPixelFormat =                                                  \
                (MTLPixelFormat)de__mtl_pixel_format((p)->depth_format);                           \
            if ((p)->depth_format == DE_FMT_D32_FLOAT_S8)                                          \
                (rd).stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;           \
        }                                                                                          \
        (rd).rasterSampleCount = 1;                                                                \
    } while (0)

static MTLCullMode cull_mode(de_gpu_cull c) {
    return c == DE_CULL_BACK ? MTLCullModeBack : c == DE_CULL_FRONT ? MTLCullModeFront
                                                                    : MTLCullModeNone;
}

static id<MTLDepthStencilState> depth_state(de_gpu_device *d, const de_gpu_pipeline_desc *desc) {
    MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
    dd.depthCompareFunction = desc->depth_test ? MTLCompareFunctionGreaterEqual
                                               : MTLCompareFunctionAlways;
    dd.depthWriteEnabled = desc->depth_write ? YES : NO;
    return [(__bridge id<MTLDevice>)de__mtl_dev(d)->device newDepthStencilStateWithDescriptor:dd];
}

static NSString *label_of(const de__gpu_pipeline *p) {
    return p->name[0] ? [NSString stringWithUTF8String:p->name] : nil;
}

static u32 threads(const u32 s[3]) {
    return s[0] * s[1] * s[2];
}

static bool given(const u32 s[3]) {
    return s[0] != 0 || s[1] != 0 || s[2] != 0;
}

static de_result fail_state(de_gpu_device *d, const de__gpu_pipeline *p, const char *what,
                            NSError *err) {
    return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT, F "(\"%s\"): metal: %s: %s", pname(p), what,
                        err ? err.localizedDescription.UTF8String : "no reason given");
}

/* ---------- Kinds ---------- */

static de_result create_graphics(de_gpu_device *d, de__gpu_pipeline *p, mtl_pipeline *mp,
                                 const de_gpu_pipeline_desc *desc) {
    id<MTLFunction> vs = function_of(d, p, "vs", &desc->vs, MTLFunctionTypeVertex);
    if (vs == nil)
        return de_error_last()->result;
    id<MTLFunction> ps = nil;
    if (desc->ps.code) {
        ps = function_of(d, p, "ps", &desc->ps, MTLFunctionTypeFragment);
        if (ps == nil)
            return de_error_last()->result;
    }
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    if (p->name[0])
        rd.label = label_of(p);
    rd.vertexFunction = vs;
    rd.fragmentFunction = ps;
    rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    rd.supportIndirectCommandBuffers = YES;
    SET_TARGETS(rd, p, desc);
    id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
    rd.binaryArchives = (__bridge NSArray *)de__mtl_archive_list(d);
    MTLAutoreleasedRenderPipelineReflection refl = nil;
    NSError *err = nil;
    bool hit = false;
    id<MTLRenderPipelineState> st = make_state(
        d, rd, true,
        ^id(MTLPipelineOption o, MTLAutoreleasedRenderPipelineReflection *rr,
            MTLAutoreleasedComputePipelineReflection *cr, NSError **e) {
          (void)cr;
          return [dev newRenderPipelineStateWithDescriptor:rd options:o reflection:rr error:e];
        },
        &refl, NULL, &err, &hit);
    if (st == nil)
        return fail_state(d, p, "render pipeline", err);
    push_info pi = {0};
    push_scan(&pi, refl.vertexBindings, DE__MTL_PUSH_VERTEX, "vs");
    push_scan(&pi, refl.fragmentBindings, DE__MTL_PUSH_FRAGMENT, "ps");
    DE_TRY(push_check(d, p, mp, &pi));
    mp->state = (void *)CFBridgingRetain(st);
    mp->archive_hit = hit;
    return DE_OK;
}

static de_result create_mesh(de_gpu_device *d, de__gpu_pipeline *p, mtl_pipeline *mp,
                             const de_gpu_pipeline_desc *desc) {
    if (!given(desc->threadgroup_size))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            F "(\"%s\"): a MESH pipeline needs threadgroup_size (the ms "
                              "reflection's threadgroup): Metal reads it from the desc",
                            pname(p));
    if (desc->as.code && !given(desc->object_threadgroup_size))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            F "(\"%s\"): a MESH pipeline with an as stage needs "
                              "object_threadgroup_size (the as reflection's threadgroup)",
                            pname(p));
    if (@available(macOS 13.0, *)) {
        id<MTLFunction> os = nil, ms = nil, ps = nil;
        if (desc->as.code &&
            (os = function_of(d, p, "as", &desc->as, MTLFunctionTypeObject)) == nil)
            return de_error_last()->result;
        if ((ms = function_of(d, p, "ms", &desc->ms, MTLFunctionTypeMesh)) == nil)
            return de_error_last()->result;
        if (desc->ps.code &&
            (ps = function_of(d, p, "ps", &desc->ps, MTLFunctionTypeFragment)) == nil)
            return de_error_last()->result;
        MTLMeshRenderPipelineDescriptor *rd = [MTLMeshRenderPipelineDescriptor new];
        if (p->name[0])
            rd.label = label_of(p);
        rd.objectFunction = os;
        rd.meshFunction = ms;
        rd.fragmentFunction = ps;
        rd.maxTotalThreadsPerMeshThreadgroup = threads(desc->threadgroup_size);
        if (os != nil) {
            rd.maxTotalThreadsPerObjectThreadgroup = threads(desc->object_threadgroup_size);
            rd.payloadMemoryLength = PAYLOAD_BYTES;
        }
        SET_TARGETS(rd, p, desc);
        /* Mesh descriptors take binary archives from macOS 15 (and its SDK) on. */
        bool archive_ok = false;
#if defined(__MAC_15_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0
        if (@available(macOS 15.0, *))
            archive_ok = true;
#endif
#if defined(__MAC_15_0) && __MAC_OS_X_VERSION_MAX_ALLOWED >= __MAC_15_0
        if (@available(macOS 15.0, *))
            rd.binaryArchives = archive_ok ? (__bridge NSArray *)de__mtl_archive_list(d) : nil;
#endif
        id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
        MTLAutoreleasedRenderPipelineReflection refl = nil;
        NSError *err = nil;
        bool hit = false;
        id<MTLRenderPipelineState> st = make_state(
            d, rd, archive_ok,
            ^id(MTLPipelineOption o, MTLAutoreleasedRenderPipelineReflection *rr,
                MTLAutoreleasedComputePipelineReflection *cr, NSError **e) {
              (void)cr;
              return [dev newRenderPipelineStateWithMeshDescriptor:rd
                                                           options:o
                                                        reflection:rr
                                                             error:e];
            },
            &refl, NULL, &err, &hit);
        if (st == nil)
            return fail_state(d, p, "mesh render pipeline", err);
        if (threads(desc->threadgroup_size) > st.maxTotalThreadsPerMeshThreadgroup ||
            (os != nil &&
             threads(desc->object_threadgroup_size) > st.maxTotalThreadsPerObjectThreadgroup))
            return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                                F "(\"%s\"): threadgroup sizes of %u (ms) and %u (as) threads; "
                                  "the pipeline allows %u and %u",
                                pname(p), threads(desc->threadgroup_size),
                                threads(desc->object_threadgroup_size),
                                (u32)st.maxTotalThreadsPerMeshThreadgroup,
                                (u32)st.maxTotalThreadsPerObjectThreadgroup);
        push_info pi = {0};
        push_scan(&pi, refl.objectBindings, DE__MTL_PUSH_OBJECT, "as");
        push_scan(&pi, refl.meshBindings, DE__MTL_PUSH_MESH, "ms");
        push_scan(&pi, refl.fragmentBindings, DE__MTL_PUSH_FRAGMENT, "ps");
        DE_TRY(push_check(d, p, mp, &pi));
        mp->state = (void *)CFBridgingRetain(st);
        mp->archive_hit = hit;
        return DE_OK;
    }
    return de__gpu_fail(d, DE_ERR_UNSUPPORTED, F "(\"%s\"): mesh pipelines need macOS 13",
                        pname(p));
}

static de_result create_compute(de_gpu_device *d, de__gpu_pipeline *p, mtl_pipeline *mp,
                                const de_gpu_pipeline_desc *desc) {
    if (!given(desc->threadgroup_size))
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            F "(\"%s\"): a COMPUTE pipeline needs threadgroup_size (the cs "
                              "reflection's threadgroup): Metal reads it from the desc",
                            pname(p));
    id<MTLFunction> cs = function_of(d, p, "cs", &desc->cs, MTLFunctionTypeKernel);
    if (cs == nil)
        return de_error_last()->result;
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    if (p->name[0])
        cd.label = label_of(p);
    cd.computeFunction = cs;
    cd.binaryArchives = (__bridge NSArray *)de__mtl_archive_list(d);
    id<MTLDevice> dev = (__bridge id<MTLDevice>)de__mtl_dev(d)->device;
    MTLAutoreleasedComputePipelineReflection refl = nil;
    NSError *err = nil;
    bool hit = false;
    id<MTLComputePipelineState> st = make_state(
        d, cd, true,
        ^id(MTLPipelineOption o, MTLAutoreleasedRenderPipelineReflection *rr,
            MTLAutoreleasedComputePipelineReflection *cr, NSError **e) {
          (void)rr;
          return [dev newComputePipelineStateWithDescriptor:cd options:o reflection:cr error:e];
        },
        NULL, &refl, &err, &hit);
    if (st == nil)
        return fail_state(d, p, "compute pipeline", err);
    if (threads(desc->threadgroup_size) > st.maxTotalThreadsPerThreadgroup)
        return de__gpu_fail(d, DE_ERR_INVALID_ARGUMENT,
                            F "(\"%s\"): threadgroup size (%u, %u, %u) has %u threads; the "
                              "pipeline allows %u",
                            pname(p), desc->threadgroup_size[0], desc->threadgroup_size[1],
                            desc->threadgroup_size[2], threads(desc->threadgroup_size),
                            (u32)st.maxTotalThreadsPerThreadgroup);
    push_info pi = {0};
    push_scan(&pi, refl.bindings, DE__MTL_PUSH_COMPUTE, "cs");
    DE_TRY(push_check(d, p, mp, &pi));
    mp->state = (void *)CFBridgingRetain(st);
    mp->archive_hit = hit;
    return DE_OK;
}

/* ---------- Vtable entries ---------- */

de_result de__mtl_pipeline_create(de_gpu_device *d, de__gpu_pipeline *p,
                                  const de_gpu_pipeline_desc *desc) {
    mtl_pipeline *mp = DE__GPU_PAYLOAD(mtl_pipeline, p);
    de_result r;
    @autoreleasepool {
        switch (desc->kind) {
        case DE_PIPE_GRAPHICS:
            r = create_graphics(d, p, mp, desc);
            break;
        case DE_PIPE_MESH:
            r = create_mesh(d, p, mp, desc);
            break;
        case DE_PIPE_COMPUTE:
            r = create_compute(d, p, mp, desc);
            break;
        default:
            r = de__gpu_fail(d, DE_ERR_UNSUPPORTED, F "(\"%s\"): metal: pipeline kind %d",
                             pname(p), (int)desc->kind);
            break;
        }
        if (r == DE_OK && desc->kind != DE_PIPE_COMPUTE) {
            id<MTLDepthStencilState> ds = depth_state(d, desc);
            if (ds == nil)
                r = de__gpu_fail(d, DE_ERR_OUT_OF_MEMORY, F "(\"%s\"): metal: depth state",
                                 pname(p));
            else
                mp->depth_state = (void *)CFBridgingRetain(ds);
            mp->cull = (u32)cull_mode(desc->cull);
        }
        if (r != DE_OK) {
            release(&mp->state);
            release(&mp->depth_state);
        }
    }
    return r;
}

void de__mtl_pipeline_destroy(de_gpu_device *d, de__gpu_pipeline *p) {
    (void)d;
    mtl_pipeline *mp = DE__GPU_PAYLOAD(mtl_pipeline, p);
    @autoreleasepool {
        release(&mp->state);
        release(&mp->depth_state);
    }
}

void de__mtl_pipeline_shutdown(de_gpu_device *d) {
    mtl_device *m = de__mtl_dev(d);
    @autoreleasepool {
        release(&m->libraries);
        release(&m->icb_kernel);
    }
}
