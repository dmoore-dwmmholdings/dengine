#include "de_test.h"
#include "gpu/metal/mtl_accel_internal.h"
#include <stdio.h>
#include <dengine/de_debug.h>
#include "metal/mtl_test_helpers.h"
#include "pipeline_shaders.h"
#include <string.h>
void *de_test_mtl_accel_helper(de_gpu_device *d);
void de_test_mtl_accel_release(de_gpu_device *d, void *helper);
bool de_test_mtl_accel_trace(de_gpu_cmd *, void *, u32, de_gpu_buffer, de_gpu_buffer);
bool de_test_mtl_accel_generate(de_gpu_cmd *, void *, de_gpu_buffer, const de_gpu_tlas_instance *);
typedef struct fixture {
    de_gpu_device *d;
    de_tracking_allocator tracker;
    de_allocator alloc;
    de_gpu_buffer vertices, indices, instances, output, calls, readback, zeros;
    de_gpu_accel blas, tlas;
    de_gpu_fence fence;
    void *helper;
    u64 value;
    de_gpu_tlas_instance instance;
} fixture;
static void setup(fixture *f, bool opaque) {
    f->alloc=de_tracking_allocator_init(&f->tracker,NULL,DE_MEM_RENDER);
    de_result r=de_gpu_device_create(&(de_gpu_device_desc){.backend=DE_GPU_METAL,
        .allocator=&f->alloc,.debug_layer=true,.gpu_validation=true,.pipeline_cache_dir=""},&f->d);
    if(r==DE_ERR_UNSUPPORTED) SKIP("Metal runtime unavailable");
    ASSERT_EQ(DE_OK,r);
    if(!de_gpu_get_caps(f->d)->ray_query) SKIP("Metal ray-query API unavailable");
    printf("Metal AS: %s, raytracing=%d, ray_query=%d\n",de_gpu_get_caps(f->d)->device_name,
        de_gpu_get_caps(f->d)->raytracing,de_gpu_get_caps(f->d)->ray_query);
    f->helper=de_test_mtl_accel_helper(f->d);
    ASSERT_TRUE(f->helper!=NULL);
    f->vertices=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=36,
        .usage=DE_USAGE_ACCEL_INPUT,.memory=DE_MEM_UPLOAD});
    f->indices=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=12,
        .usage=DE_USAGE_ACCEL_INPUT,.memory=DE_MEM_UPLOAD});
    f->instances=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=60,
        .usage=DE_USAGE_ACCEL_INPUT|DE_USAGE_STORAGE});
    f->output=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=8,
        .usage=DE_USAGE_STORAGE|DE_USAGE_COPY_SRC|DE_USAGE_COPY_DST});
    f->calls=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=4,
        .usage=DE_USAGE_STORAGE|DE_USAGE_COPY_SRC|DE_USAGE_COPY_DST});
    f->readback=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=16,
        .usage=DE_USAGE_COPY_DST,.memory=DE_MEM_READBACK});
    f->zeros=de_gpu_buffer_create(f->d,&(de_gpu_buffer_desc){.size=4,
        .usage=DE_USAGE_COPY_SRC,.memory=DE_MEM_UPLOAD});
    ASSERT_TRUE(f->vertices.id && f->indices.id && f->instances.id && f->output.id && f->calls.id);
    const f32 v[]={-1,-1,2,1,-1,2,0,1,2};
    const u32 ix[]={0,1,2};
    memcpy(de_gpu_buffer_map(f->d,f->vertices),v,sizeof v);
    memcpy(de_gpu_buffer_map(f->d,f->indices),ix,sizeof ix);
    de_gpu_blas_geometry g={.vertices=f->vertices,.vertex_stride=12,.vertex_count=3,
        .vertex_format=DE_FMT_RGB32_FLOAT,.indices=f->indices,.index_count=3,.opaque=opaque};
    de_gpu_accel_desc x={.geometries=&g,.geometry_count=1,.allow_update=true,
        .allow_compaction=true,.prefer_fast_trace=true};
    de_gpu_accel_sizes sizes=de_gpu_accel_query_sizes(f->d,&x);
    printf("Metal AS sizes: %llu/%llu/%llu\n",(unsigned long long)sizes.accel_bytes,
        (unsigned long long)sizes.build_scratch_bytes,(unsigned long long)sizes.update_scratch_bytes);
    ASSERT_TRUE(sizes.accel_bytes>0);
    f->blas=de_gpu_accel_create(f->d,&x);
    f->tlas=de_gpu_accel_create(f->d,&(de_gpu_accel_desc){.is_tlas=true,.max_instances=1,
        .allow_update=true});
    f->fence=de_gpu_fence_create(f->d,0);
    ASSERT_TRUE(f->blas.id && f->tlas.id && f->fence.id);
    /* 90-degree Z rotation + translation (3,0,1). Ray at x=3 catches transpose errors. */
    f->instance=(de_gpu_tlas_instance){.transform={0,-1,0,3,1,0,0,0,0,0,1,1},
        .mask=255,.custom_index=37,.blas=f->blas};
}
static void fixture_setup(fixture *f) { setup(f, true); }
static void fixture_teardown(fixture *f) {
    if(!f->d)return;
    de_gpu_wait_idle(f->d);
    de_test_mtl_accel_release(f->d,f->helper);
    de_gpu_accel_destroy(f->d,f->blas);
    de_gpu_accel_destroy(f->d,f->tlas);
    de_gpu_buffer buffers[]={f->vertices,f->indices,f->instances,f->output,f->calls,f->readback,f->zeros};
    for (u32 i=0;i<sizeof buffers / sizeof buffers[0];++i) de_gpu_buffer_destroy(f->d,buffers[i]);
    de_gpu_fence_destroy(f->d,f->fence);
    de_gpu_device_destroy(f->d);
    EXPECT_EQ(0u,de_tracking_allocator_stats(&f->tracker).blocks_live);
}
static void submit(fixture *f,de_gpu_cmd *c) {
    ++f->value;
    ASSERT_EQ(DE_OK,de_gpu_queue_submit(f->d,DE_QUEUE_COMPUTE,
        &(de_gpu_submit){.cmds=&c,.cmd_count=1,.signal=f->fence,.signal_value=f->value}));
    ASSERT_EQ(DE_OK,de_gpu_fence_wait(f->d,f->fence,f->value,60000000000ull));
    ASSERT_FALSE(de_gpu_device_is_lost(f->d));
}
static f32 finish_trace(fixture *f, de_gpu_cmd *c) {
    ASSERT_TRUE(de_test_mtl_accel_trace(c,f->helper,de_gpu_accel_bindless(f->d,f->tlas),
        f->output,f->calls));
    de_gpu_barrier b[2]={{.buffer=f->output,.before=DE_ACCESS_SHADER_WRITE,.after=DE_ACCESS_COPY_SRC},
        {.buffer=f->calls,.before=DE_ACCESS_SHADER_WRITE,.after=DE_ACCESS_COPY_SRC}};
    de_gpu_cmd_barriers(c,b,2);
    de_gpu_cmd_copy_buffer(c,f->readback,0,f->output,0,8);
    de_gpu_cmd_copy_buffer(c,f->readback,8,f->calls,0,4);
    submit(f,c);
    return *(f32*)de_gpu_buffer_map(f->d,f->readback);
}
static f32 trace(fixture *f, bool build_blas,bool refit) {
    *(u32*)de_gpu_buffer_map(f->d,f->zeros)=0;
    de_gpu_cmd *c=de_gpu_cmd_begin(f->d,DE_QUEUE_COMPUTE,"Metal AS native acceptance");
    ASSERT_TRUE(c!=NULL);
    de_gpu_cmd_copy_buffer(c,f->calls,0,f->zeros,0,4);
    if(build_blas)de_gpu_cmd_build_blas(c,f->blas,refit);
    ASSERT_TRUE(de_test_mtl_accel_generate(c,f->helper,f->instances,&f->instance));
    de_gpu_cmd_build_tlas(c,f->tlas,f->instances,1,refit);
    return finish_trace(f,c);
}
TEST_F(accel_metal, build_trace, fixture) {
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    ASSERT_EQ(0,((u32*)de_gpu_buffer_map(f->d,f->readback))[2]);
    ASSERT_FLOAT_EQ(37.0f,((f32*)de_gpu_buffer_map(f->d,f->readback))[1],0.0f);
}
TEST_F(accel_metal, refit, fixture) {
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    f32 *v=de_gpu_buffer_map(f->d,f->vertices);
    v[2]=v[5]=v[8]=5;
    ASSERT_FLOAT_EQ(6.0f,trace(f,true,true),0.001f);
}
TEST_F(accel_metal, compact, fixture) {
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    de_gpu_cmd *c=de_gpu_cmd_begin(f->d,DE_QUEUE_COMPUTE,"AS compact size");
    de_gpu_cmd_accel_emit_compacted_size(c,f->blas);
    u64 size=0;
    ASSERT_FALSE(de_gpu_accel_compacted_size(f->d,f->blas,&size));
    submit(f,c);
    de_gpu_wait_idle(f->d); /* Metal user event can signal before completion handlers publish the queue timeline. */
    ASSERT_TRUE(de_gpu_accel_compacted_size(f->d,f->blas,&size));
    de__gpu_accel *a=de__gpu_get_accel(f->d,f->blas);
    ASSERT_TRUE(size>0 && size<=a->sizes.accel_bytes);
    c=de_gpu_cmd_begin(f->d,DE_QUEUE_COMPUTE,"AS compact copy");
    de_gpu_accel compact=de_gpu_cmd_accel_compact(c,f->blas);
    ASSERT_TRUE(compact.id!=0);
    submit(f,c);
    de_gpu_accel_destroy(f->d,f->blas);
    f->blas=compact;
    f->instance.blas=compact;
    ASSERT_FLOAT_EQ(3.0f,trace(f,false,false),0.001f);
}
TEST_F(accel_metal, intersection_fn, fixture) {
    de_gpu_accel_destroy(f->d,f->blas);
    de_gpu_blas_geometry g={.vertices=f->vertices,.vertex_stride=12,.vertex_count=3,
        .vertex_format=DE_FMT_RGB32_FLOAT,.indices=f->indices,.index_count=3,.opaque=false};
    f->blas=de_gpu_accel_create(f->d,&(de_gpu_accel_desc){.geometries=&g,.geometry_count=1});
    ASSERT_TRUE(f->blas.id!=0);
    f->instance.blas=f->blas;
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    ASSERT_TRUE(((u32*)de_gpu_buffer_map(f->d,f->readback))[2]>0);
}
TEST_F(accel_metal, generation_and_invalid_references, fixture) {
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    de_gpu_accel stale=f->blas;
    de_gpu_accel_destroy(f->d,f->blas);
    de_gpu_wait_idle(f->d);
    de_gpu_blas_geometry g={.vertices=f->vertices,.vertex_stride=12,.vertex_count=3,
        .vertex_format=DE_FMT_RGB32_FLOAT,.opaque=true};
    f->blas=de_gpu_accel_create(f->d,&(de_gpu_accel_desc){.geometries=&g,.geometry_count=1});
    ASSERT_TRUE(f->blas.id!=0 && f->blas.id!=stale.id);
    ASSERT_EQ(de__gpu_handle_index(stale.id),de__gpu_handle_index(f->blas.id));
    f->instance.blas=stale;
    ASSERT_FLOAT_EQ(-1.0f,trace(f,false,false),0.001f);
    f->instance.blas=f->blas; /* valid but unbuilt BLAS */
    ASSERT_FLOAT_EQ(-1.0f,trace(f,false,false),0.001f);
    f->instance.blas=f->tlas;
    ASSERT_FLOAT_EQ(-1.0f,trace(f,false,false),0.001f);
    f->instance.blas.id=0xffffffffu;
    ASSERT_FLOAT_EQ(-1.0f,trace(f,false,false),0.001f);
}
TEST_F(accel_metal, caps_tier, fixture) {
    const de_gpu_caps *c=de_gpu_get_caps(f->d);
    de_test_mtl_info info;
    ASSERT_TRUE(de_test_mtl_device_info(0,&info));
    ASSERT_EQ(c->raytracing,info.apple9 ? DE_RT_HARDWARE : DE_RT_API_ONLY);
    ASSERT_TRUE(c->max_bindless_accels>0 && c->ray_query);
    ASSERT_FALSE(c->opacity_micromaps);
}

TEST_F(accel_metal, retained_blas_residency, fixture) {
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    de_gpu_accel_destroy(f->d,f->blas);
    f->blas.id=0;
    de_gpu_wait_idle(f->d); /* retire original public BLAS; TLAS must keep native dependency */
    de_gpu_cmd *c=de_gpu_cmd_begin(f->d,DE_QUEUE_COMPUTE,"retired BLAS remains resident");
    de_gpu_cmd_copy_buffer(c,f->calls,0,f->zeros,0,4);
    ASSERT_FLOAT_EQ(3.0f,finish_trace(f,c),0.001f);
}
TEST_F(accel_metal, warmed_scratch_reuse, fixture) {
    for(u32 i=0;i<3;++i) {
        ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
        de_gpu_wait_idle(f->d);
    }
    const u64 before=de_tracking_allocator_stats(&f->tracker).alloc_count;
    for(u32 i=0;i<8;++i) {
        ASSERT_FLOAT_EQ(3.0f,trace(f,true,true),0.001f);
        de_gpu_wait_idle(f->d);
    }
    EXPECT_EQ(before,de_tracking_allocator_stats(&f->tracker).alloc_count);
}

TEST_F(accel_metal, slang_ray_query, fixture) {
#if DE_PIPE_TEST_COOK
    char why[512]={0};
    ASSERT_TRUE(de_pipe_test_cook(DE__SHADER_TARGET_METALLIB,why,sizeof why));
    de_pipe_test_shader sh;
    ASSERT_TRUE(de_pipe_test_get(DE__SHADER_TARGET_METALLIB,"accel_query.slang","cs_main",&sh));
    de_gpu_pipeline p=de_gpu_pipeline_create(f->d,&(de_gpu_pipeline_desc){.kind=DE_PIPE_COMPUTE,
        .cs=sh.blob,.push_constant_bytes=8,.threadgroup_size={1,1,1}});
    ASSERT_TRUE(p.id!=0);
    /* Shared Slang test ray starts at x=0; keep a nonidentity z translation. */
    f->instance=(de_gpu_tlas_instance){.transform={1,0,0,0,0,1,0,0,0,0,1,1},
        .mask=255,.blas=f->blas};
    de_gpu_cmd *c=de_gpu_cmd_begin(f->d,DE_QUEUE_COMPUTE,"Slang Metal RayQuery");
    de_gpu_cmd_build_blas(c,f->blas,false);
    ASSERT_TRUE(de_test_mtl_accel_generate(c,f->helper,f->instances,&f->instance));
    de_gpu_cmd_build_tlas(c,f->tlas,f->instances,1,false);
    de_gpu_cmd_set_pipeline(c,p);
    const u32 pc[2]={de_gpu_accel_bindless(f->d,f->tlas),de_gpu_buffer_bindless(f->d,f->output)};
    de_gpu_cmd_push_constants(c,pc,sizeof pc);
    de_gpu_cmd_dispatch(c,1,1,1);
    de_gpu_barrier b={.buffer=f->output,.before=DE_ACCESS_SHADER_WRITE,.after=DE_ACCESS_COPY_SRC};
    de_gpu_cmd_barriers(c,&b,1);
    de_gpu_cmd_copy_buffer(c,f->readback,0,f->output,0,4);
    submit(f,c);
    ASSERT_FLOAT_EQ(3.0f,*(f32*)de_gpu_buffer_map(f->d,f->readback),0.001f);
    de_gpu_pipeline_destroy(f->d,p);
#else
    SKIP("Slang shader importer not built");
#endif
}

typedef fixture forced_fixture;
static void forced_fixture_setup(forced_fixture *f) {
    const char force[]="gpu.metal.force_use_heap = true\n";
    ASSERT_EQ(DE_OK,de_cvar_load_from_memory(force,sizeof force-1));
    setup(f,true);
    de_cvar_set(de_cvar_find("gpu.metal.force_use_heap"),"0");
    ASSERT_TRUE(de__mtl_residency_uses_heaps(f->d));
}
static void forced_fixture_teardown(forced_fixture *f) { fixture_teardown(f); }
TEST_F(accel_metal, forced_use_heap, forced_fixture) {
    ASSERT_FLOAT_EQ(3.0f,trace(f,true,false),0.001f);
    de_gpu_accel_destroy(f->d,f->blas);
    f->blas.id=0;
    de_gpu_wait_idle(f->d);
    de_gpu_cmd *c=de_gpu_cmd_begin(f->d,DE_QUEUE_COMPUTE,"useHeap retired BLAS residency");
    de_gpu_cmd_copy_buffer(c,f->calls,0,f->zeros,0,4);
    ASSERT_FLOAT_EQ(3.0f,finish_trace(f,c),0.001f);
}
