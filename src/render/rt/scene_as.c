/* DENG-44: main-thread owner of published mesh BLAS and asynchronous compaction. */
#include "render/rt/rt_internal.h"
#include "render/loaders/upload_ring.h"
#include "render/shaders/engine_shaders.h"
#include "asset/budget_internal.h"
#include <string.h>

static de_gpu_buffer buffer(de__rt_scene *s,u64 size,u32 stride,u32 usage,de_gpu_memory memory) {
    return de_gpu_buffer_create(s->renderer->dev,&(de_gpu_buffer_desc){.size=size?size:16,
        .stride=stride,.usage=usage,.memory=memory,.name="RT scene"});
}
static void charge(de__rt_scene *s,de__rt_mesh *m) {
    de__budget_resize_mesh(s->renderer->assets,m->asset_id,m->source,m->base_bytes+m->bytes);
}
static de_result submit(de__rt_scene *s,de_gpu_cmd *c) {
    return de_gpu_queue_submit(s->renderer->dev,DE_QUEUE_GRAPHICS,
        &(de_gpu_submit){.cmds=&c,.cmd_count=1,.signal=s->fence,.signal_value=++s->value});
}
static void release_mesh(de__rt_scene *s,de__rt_mesh *m) {
    de_gpu_device *d=s->renderer->dev;
    for(u32 l=0;l<8;++l) {
        if(m->compact_value[l]==UINT64_MAX && m->blas[l].id)--s->compacted_count;
        de_gpu_accel_destroy(d,m->blas[l]);
        de_gpu_accel_destroy(d,m->compact[l]);
    }
    de_gpu_buffer_destroy(d,m->positions);
    de_gpu_buffer_destroy(d,m->description);
    de_gpu_buffer_destroy(d,m->ranges);
    s->bytes-=m->bytes;
    s->blas_count-=m->live_lods;
    m->bytes=0; charge(s,m);
    memset(m,0,sizeof *m);
}
de_result de__rt_scene_create(de_renderer *r,de__rt_scene **out) {
    *out=NULL;
    de__rt_scene *s=DE_ALLOC(r->alloc,sizeof *s,16,DE_MEM_RENDER);
    if(!s)return DE_FAIL(DE_ERR_OUT_OF_MEMORY,"RT scene owner");
    memset(s,0,sizeof *s); s->renderer=r;
    s->fence=de_gpu_fence_create(r->dev,0);
    const char *entries[]={"cs_decode","cs_instances"};
    de__engine_shader_code code;
    char why[512]={0};
    de_result result=de__engine_shader_load(r->dev,r->alloc,"rt_scene_instances",0,entries,2,
                                          &code,why,sizeof why);
    if(result==DE_OK) {
        for(u32 i=0;i<2;++i) {
            de_gpu_pipeline p=de_gpu_pipeline_create(r->dev,&(de_gpu_pipeline_desc){
                .kind=DE_PIPE_COMPUTE,.cs=code.entries[i].blob,.push_constant_bytes=32,
                .threadgroup_size={64,1,1},.name=entries[i]});
            if(i==0)s->decode=p; else s->generate=p;
            if(!p.id)result=DE_ERR_UNSUPPORTED;
        }
        de__engine_shader_code_free(&code);
    }
    if(result!=DE_OK || !s->fence.id) {
        de_gpu_pipeline_destroy(r->dev,s->trace);de_gpu_sampler_destroy(r->dev,s->sampler);
    de_gpu_pipeline_destroy(r->dev,s->decode); de_gpu_pipeline_destroy(r->dev,s->generate);
        de_gpu_fence_destroy(r->dev,s->fence); DE_FREE(r->alloc,s,sizeof *s);
        return DE_FAIL(result==DE_OK?DE_ERR_OUT_OF_MEMORY:result,"RT scene shaders: %s",why);
    }
    s->sampler=de_gpu_sampler_create(r->dev,&(de_gpu_sampler_desc){.min=DE_FILTER_LINEAR,
        .mag=DE_FILTER_LINEAR,.mip=DE_FILTER_LINEAR});
    de_gpu_shader_blob trace_blob={0};
#if DE_HAS_GPU_METAL
    extern const unsigned char de__rt_metal_library[];
    extern const unsigned long long de__rt_metal_library_bytes;
    if(r->caps.backend==DE_GPU_METAL)
        trace_blob=(de_gpu_shader_blob){de__rt_metal_library,de__rt_metal_library_bytes,"cs_trace"};
#endif
    de__engine_shader_code trace_code={0};
    if(!trace_blob.code) {
        const char *trace_entries[]={"cs_trace"};
        result=de__engine_shader_load(r->dev,r->alloc,"rt_trace",0,trace_entries,1,
                                      &trace_code,why,sizeof why);
        if(result==DE_OK)trace_blob=trace_code.entries[0].blob;
    }
    if(trace_blob.code)s->trace=de_gpu_pipeline_create(r->dev,&(de_gpu_pipeline_desc){
        .kind=DE_PIPE_COMPUTE,.cs=trace_blob,.push_constant_bytes=20,
        .threadgroup_size={64,1,1},.name="RT alpha batch query"});
    de__engine_shader_code_free(&trace_code);
    if(!s->trace.id||!s->sampler.id) {
        r->rt_scene=s;de__rt_scene_destroy(r);
        return DE_FAIL(DE_ERR_UNSUPPORTED,"RT alpha query pipeline: %s",why);
    }
    *out=s;return DE_OK;
}
de_result de__rt_mesh_publish(de__rt_scene *s,const de_render_mesh *source) {
    for(u32 i=0;i<s->mesh_count;++i)if(s->meshes[i].source==source) {
        s->meshes[i].seen=true;return DE_OK;
    }
    if(!source || !source->vertex_count || !source->lod_count)return DE_OK;
    if(s->mesh_count==s->mesh_capacity) {
        u32 cap=s->mesh_capacity?s->mesh_capacity*2:16;
        de__rt_mesh *next=DE_ALLOC(s->renderer->alloc,(size_t)cap*sizeof *next,16,DE_MEM_RENDER);
        if(!next)return DE_FAIL(DE_ERR_OUT_OF_MEMORY,"RT mesh capacity");
        memset(next,0,(size_t)cap*sizeof *next);
        if(s->meshes) {
            memcpy(next,s->meshes,(size_t)s->mesh_count*sizeof *next);
            DE_FREE(s->renderer->alloc,s->meshes,(size_t)s->mesh_capacity*sizeof *next);
        }
        s->meshes=next;s->mesh_capacity=cap;
    }
    de__rt_mesh *m=&s->meshes[s->mesh_count];
    m->source=source;m->asset_id=source->asset_id_;m->base_bytes=source->gpu_bytes;m->seen=true;
    de__gpu_scene_pack_mesh_record(source,&m->packed);
    m->positions=buffer(s,(u64)source->vertex_count*12,0,DE_USAGE_STORAGE|DE_USAGE_ACCEL_INPUT,
                        DE_MEM_GPU_ONLY);
    m->description=buffer(s,sizeof m->packed,sizeof m->packed,DE_USAGE_STORAGE,DE_MEM_UPLOAD);
    const u64 range_bytes=(u64)source->lod_count*source->submesh_count*8;
    m->ranges=buffer(s,range_bytes,0,DE_USAGE_STORAGE,DE_MEM_UPLOAD);
    if(!m->positions.id||!m->description.id||!m->ranges.id)goto fail;
    memcpy(de_gpu_buffer_map(s->renderer->dev,m->description),&m->packed,sizeof m->packed);
    u32 *ranges=de_gpu_buffer_map(s->renderer->dev,m->ranges);
    for(u32 l=0;l<source->lod_count;++l)for(u32 sub=0;sub<source->submesh_count;++sub) {
        const de_cooked_mesh_range *range=&source->lod_submeshes[(size_t)l*source->submesh_count+sub];
        size_t at=((size_t)l*source->submesh_count+sub)*2;
        ranges[at]=range->index_first;ranges[at+1]=range->index_count;
    }
    m->bytes=(u64)source->vertex_count*12+sizeof m->packed+(range_bytes?range_bytes:16);
    s->bytes+=m->bytes;
    for(u32 l=0;l<source->lod_count && l<8;++l) {
        const de_cooked_mesh_lod *lod=&source->lods[l];
        if(!lod->index_count)continue;
        de_gpu_blas_geometry g={.vertices=m->positions,.vertex_stride=12,
            .vertex_count=source->vertex_count,.vertex_format=DE_FMT_RGB32_FLOAT,
            .indices=source->buffers[DE_RENDER_MESH_INDICES],.index_offset=(u64)lod->index_first*4,
            .index_count=lod->index_count,.opaque=false};
        de_gpu_accel_desc desc={.geometries=&g,.geometry_count=1,.allow_compaction=true,
                               .prefer_fast_trace=true,.name="RT mesh LOD"};
        m->sizes[l]=de_gpu_accel_query_sizes(s->renderer->dev,&desc).accel_bytes;
        m->blas[l]=de_gpu_accel_create(s->renderer->dev,&desc);
        if(!m->blas[l].id)goto fail;
        m->bytes+=m->sizes[l];s->bytes+=m->sizes[l];++m->live_lods;++s->blas_count;
    }
    de_gpu_cmd *c=de_gpu_cmd_begin(s->renderer->dev,DE_QUEUE_GRAPHICS,"RT.PublishBLAS");
    if(!c)goto fail;
    de_gpu_cmd_set_pipeline(c,s->decode);
    u32 pc[8]={de_gpu_buffer_bindless(s->renderer->dev,m->description),
               de_gpu_buffer_bindless(s->renderer->dev,m->positions)};
    de_gpu_cmd_push_constants(c,pc,sizeof pc);
    de_gpu_cmd_dispatch(c,(source->vertex_count+63)/64,1,1);
    de_gpu_barrier b={.buffer=m->positions,.before=DE_ACCESS_SHADER_WRITE,.after=DE_ACCESS_ACCEL_READ};
    de_gpu_cmd_barriers(c,&b,1);
    for(u32 l=0;l<8;++l)if(m->blas[l].id) {
        de_gpu_cmd_build_blas(c,m->blas[l],false);
        de_gpu_cmd_accel_emit_compacted_size(c,m->blas[l]);
    }
    if(submit(s,c)!=DE_OK)goto fail;
    ++s->mesh_count;charge(s,m);return DE_OK;
fail:
    release_mesh(s,m);
    return DE_FAIL(DE_ERR_OUT_OF_MEMORY,"RT mesh BLAS creation/build");
}
void de__rt_mesh_evict(de__rt_scene *s,const de_render_mesh *mesh) {
    for(u32 i=0;i<s->mesh_count;++i)if(s->meshes[i].source==mesh) {
        release_mesh(s,&s->meshes[i]);
        s->meshes[i]=s->meshes[--s->mesh_count];
        memset(&s->meshes[s->mesh_count],0,sizeof *s->meshes);return;
    }
}
static de_result compact(de__rt_scene *s) {
    de_gpu_cmd *c=NULL;
    u64 done=de_gpu_fence_value(s->renderer->dev,s->fence);
    for(u32 i=0;i<s->mesh_count;++i) {
        de__rt_mesh *m=&s->meshes[i];
        for(u32 l=0;l<8;++l) {
            if(m->compact[l].id && done>=m->compact_value[l]) {
                de_gpu_accel_destroy(s->renderer->dev,m->blas[l]);
                m->blas[l]=m->compact[l];m->compact[l].id=0;
                m->bytes-=m->sizes[l];s->bytes-=m->sizes[l];
                m->sizes[l]=m->compact_sizes[l];m->compact_value[l]=UINT64_MAX;
                ++s->compacted_count;charge(s,m);
            }
            u64 bytes=0;
            if(!m->blas[l].id || m->compact[l].id || m->compact_value[l]==UINT64_MAX ||
               !de_gpu_accel_compacted_size(s->renderer->dev,m->blas[l],&bytes))continue;
            if(!c)c=de_gpu_cmd_begin(s->renderer->dev,DE_QUEUE_GRAPHICS,"RT.CompactBLAS");
            if(!c)return DE_FAIL(DE_ERR_OUT_OF_MEMORY,"RT compaction command");
            m->compact[l]=de_gpu_cmd_accel_compact(c,m->blas[l]);
            if(!m->compact[l].id)return DE_FAIL(DE_ERR_OUT_OF_MEMORY,"RT compacted BLAS");
            m->compact_sizes[l]=bytes;m->compact_value[l]=s->value+1;
            m->bytes+=bytes;s->bytes+=bytes;charge(s,m);
        }
    }
    return c?submit(s,c):DE_OK;
}
void de__rt_scene_destroy(de_renderer *r) {
    de__rt_scene *s=r?r->rt_scene:NULL;if(!s)return;
    for(u32 i=0;i<s->mesh_count;++i)release_mesh(s,&s->meshes[i]);
    for(u32 i=0;i<32;++i) {
        de__rt_view *v=&s->views[i];
        de_gpu_accel_destroy(r->dev,v->context.tlas);
        de_gpu_buffer_destroy(r->dev,v->context.instances);
        de_gpu_buffer_destroy(r->dev,v->context.selected_lods);
        de_gpu_buffer_destroy(r->dev,v->dependency);
        for(u32 k=0;k<DE__RT_RING;++k) {
            de_gpu_buffer_destroy(r->dev,v->cull[k]);de_gpu_buffer_destroy(r->dev,v->query[k]);
        }
    }
    for(u32 i=0;i<DE__RT_RING;++i)de_gpu_buffer_destroy(r->dev,s->metadata[i]);
    de_gpu_pipeline_destroy(r->dev,s->trace);de_gpu_sampler_destroy(r->dev,s->sampler);
    de_gpu_pipeline_destroy(r->dev,s->decode);de_gpu_pipeline_destroy(r->dev,s->generate);
    de_gpu_fence_destroy(r->dev,s->fence);
    if(s->meshes)DE_FREE(r->alloc,s->meshes,(size_t)s->mesh_capacity*sizeof *s->meshes);
    DE_FREE(r->alloc,s,sizeof *s);r->rt_scene=NULL;
}
de_result de__rt_prepare(de_renderer *r) {
    de_render_settings settings=de_renderer_get_settings(r);
    bool enabled=settings.tier==DE_TIER_RT && r->caps.ray_query && r->caps.max_bindless_accels;
    if(!enabled){de__rt_scene_destroy(r);r->stats.rt_blas_bytes=0;
        r->stats.rt_meshes=r->stats.rt_blas_count=r->stats.rt_compacted_blas=0;return DE_OK;}
    if(!r->rt_scene)DE_TRY(de__rt_scene_create(r,&r->rt_scene));
    de__rt_scene *s=r->rt_scene;
    if(r->uploader) {
        for(u32 i=0;i<s->mesh_count;++i)s->meshes[i].seen=false;
        /* Kick runs with the render thread idle. The uploader lock protects ready/unload hooks. */
        de_mutex_lock(&r->uploader->lock);
        for(de_render_mesh *m=r->uploader->published_meshes;m;m=m->published_next_) {
            de_result result=de__rt_mesh_publish(s,m);
            if(result!=DE_OK){de_mutex_unlock(&r->uploader->lock);return result;}
        }
        de_mutex_unlock(&r->uploader->lock);
        for(u32 i=0;i<s->mesh_count;) {
            if(!s->meshes[i].seen)de__rt_mesh_evict(s,s->meshes[i].source);else ++i;
        }
    }
    DE_TRY(compact(s));
    de__gpu_scene_table_info table=de__gpu_scene_table(r->gpu_scene,DE__GS_MESHES);
    u32 capacity=table.capacity?table.capacity:1;
    if(capacity>s->metadata_capacity) {
        for(u32 k=0;k<DE__RT_RING;++k) {
            de_gpu_buffer next=buffer(s,(u64)capacity*sizeof(de__rt_mesh_gpu),sizeof(de__rt_mesh_gpu),
                                     DE_USAGE_STORAGE,DE_MEM_UPLOAD);
            if(!next.id)return DE_FAIL(DE_ERR_OUT_OF_MEMORY,"RT metadata capacity");
            de_gpu_buffer_destroy(r->dev,s->metadata[k]);s->metadata[k]=next;
        }
        s->metadata_capacity=capacity;
    }
    s->metadata_index=(u32)(r->frame_index%DE__RT_RING);
    de__rt_mesh_gpu *gpu=de_gpu_buffer_map(r->dev,s->metadata[s->metadata_index]);
    memset(gpu,0,(size_t)s->metadata_capacity*sizeof *gpu);
    for(u32 slot=0;slot<table.count;++slot) {
        const de__gs_mesh *mesh=de__gpu_scene_entry(r->gpu_scene,DE__GS_MESHES,slot);
        if(!mesh)continue;
        for(u32 i=0;i<s->mesh_count;++i)if(s->meshes[i].packed.vertices==mesh->vertices) {
            de__rt_mesh *m=&s->meshes[i];
            gpu[slot].ranges=de_gpu_buffer_bindless(r->dev,m->ranges);
            gpu[slot].submeshes=m->packed.submesh_count;
            for(u32 l=0;l<8;++l) {
                gpu[slot].blas[l]=m->blas[l].id;
                gpu[slot].index_first[l]=m->packed.lods[l].index_first;
                if(m->blas[l].id)gpu[slot].resident_mask|=1u<<l;
            }
            break;
        }
    }
    r->stats.rt_blas_bytes=s->bytes;r->stats.rt_meshes=s->mesh_count;
    r->stats.rt_blas_count=s->blas_count;r->stats.rt_compacted_blas=s->compacted_count;
    return DE_OK;
}
