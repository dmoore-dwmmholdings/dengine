# shaders/engine_shaders.cmake: the renderer's own shaders (DENG-421). One line per shader:
#
#   de_engine_shader(<name> <source relative to shaders/> [PERMUTATIONS <defines> ...])
#
# <name> is what the C code asks for: de__engine_shader_load(dev, alloc, "<name>", ...)
# (src/render/shaders/engine_shaders.h). <defines> is one permutation, written with ',' between
# defines ("A=1,B" means the shader setting "A=1;B"); its index in the list is the permutation
# index (0 when there are none). Entry points are every [shader("...")] function of the source.
#
# Every build cooks these at build time (tools/engine_shaders/, de-engine-shaders) into the
# platform's default targets and embeds them in de_render, so Release and Shipping have them. Debug
# and Dev builds with the cooker cook the source in-process at first use instead (through the
# build tree's .decache), so an edit applies without a rebuild. Read by src/render/CMakeLists.txt.
# See the Decision page "Engine shaders are listed once, cooked at build time and embedded; Dev
# cooks the source through a cache".

de_engine_shader(composite      common/composite.slang)
de_engine_shader(scene_scatter  scene/scatter.slang)
de_engine_shader(vis_instance_cull  culling/instance_cull.slang)
de_engine_shader(vis_meshlet_cull  culling/meshlet_cull.slang)
de_engine_shader(vis_hzb_build      culling/hzb_build.slang)
de_engine_shader(vis_occlusion      culling/occlusion.slang)
de_engine_shader(vis_mesh           visbuffer/vis_mesh.slang)
de_engine_shader(vis_raster_expand  visbuffer/vis_expand.slang)
de_engine_shader(vis_raster_draw    visbuffer/vis_vs_ps.slang)
de_engine_shader(vis_alpha          visbuffer/vis_alpha.slang)
de_engine_shader(vis_classify       visbuffer/classify.slang)
de_engine_shader(vis_pick           visbuffer/pick.slang)
de_engine_shader(vis_resolve        visbuffer/resolve_standard.slang)
de_engine_shader(brdf_lut_gen       brdf/lut_gen.slang)
de_engine_shader(lighting_clusters  lighting/cluster_build.slang)
de_engine_shader(lighting_cull      lighting/light_cull.slang)
de_engine_shader(lighting_deferred  lighting/deferred_lighting.slang)
de_engine_shader(post_output_basic  post/output_basic.slang)
de_engine_shader(forward_transparent forward/forward_transparent.slang)
# DENG-306: screen-space separable subsurface scattering (Burley profile; burley_profile.slang).
de_engine_shader(sss_separable      sss/sss_separable.slang)

de_engine_shader(probe_prefilter probes/probe_prefilter.slang)
de_engine_shader(reflection_probe_sample probes/reflection_probe_sample.slang)
de_engine_shader(ssr_resolve ssr/ssr_resolve.slang)
de_engine_shader(ssr_trace ssr/ssr_trace.slang)
de_engine_shader(irradiance_bake probes/irradiance_bake.slang)
de_engine_shader(ssgi_trace ssgi/ssgi_trace.slang)
de_engine_shader(ssgi_filter ssgi/ssgi_filter.slang)

de_engine_shader(shadow_depth shadows/shadow_depth.slang)
de_engine_shader(shadow_sample shadows/csm_sample.slang)

de_engine_shader(gtao ao/gtao.slang)
de_engine_shader(gtao_filter ao/gtao_filter.slang)

de_engine_shader(scene_color_pyramid forward/scene_color_pyramid.slang)
de_engine_shader(ss_refraction forward/ss_refraction.slang)

de_engine_shader(shadow_atlas shadows/atlas_sample.slang)

de_engine_shader(contact_shadows shadows/contact_shadows.slang)

de_engine_shader(pcss_query shadows/pcss_query.slang)

# DENG-44: quantized position decode and GPU scene -> TLAS public instance records.
de_engine_shader(rt_scene_instances rt/scene_instances.slang)
# Vulkan/DXR use the same opacity helper in RayQuery candidate loops. Metal's explicit IFT
# batch adapter is embedded by render/CMakeLists from the generated shared opacity MSL.
de_engine_shader(rt_trace rt/trace.slang)

de_engine_shader(rt_shadows rt/rt_shadows.slang)

de_engine_shader(restir_di_initial rt/restir_di_initial.slang)
de_engine_shader(restir_di_temporal rt/restir_di_temporal.slang)
de_engine_shader(restir_di_spatial rt/restir_di_spatial.slang)
de_engine_shader(restir_di_shade rt/restir_di_shade.slang)

# DENG-47: ray generation, temporary spatial filter and portable AO publication.
de_engine_shader(rtao rt/rtao.slang)

de_engine_shader(rt_reflections rt/rt_reflections.slang)

de_engine_shader(ddgi_trace rt/ddgi_trace.slang)
de_engine_shader(ddgi_update rt/ddgi_update.slang)
de_engine_shader(ddgi_relocate rt/ddgi_relocate.slang)

de_engine_shader(rt_refraction rt/rt_refraction.slang)

# Native RT stages use DXIL/SPIR-V. Metal uses its explicit R2 callable-table wrapper.
if(NOT APPLE)
    de_engine_shader(pt_pipeline pathtracer/pt_raygen.slang)
endif()
de_engine_shader(pt_environment pathtracer/pt_env.slang)

# S17 shared deterministic denoiser blocks.
de_engine_shader(denoise_reproject denoise/reproject.slang)
de_engine_shader(denoise_temporal denoise/temporal.slang)
de_engine_shader(denoise_variance denoise/variance.slang)
de_engine_shader(denoise_atrous denoise/atrous.slang)

# Shared-block shadow/AO policies (no temporal or wavelet kernel fork).
de_engine_shader(denoise_shadow_prefilter denoise/shadow_prefilter.slang)
de_engine_shader(denoise_shadow_publish denoise/shadow_temporal.slang)

# S17 radiance policies; shared temporal and a-trous remain single implementations.
de_engine_shader(denoise_reblur_diffuse denoise/reblur_diffuse.slang)
de_engine_shader(denoise_reblur_specular denoise/reblur_specular.slang)
