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
