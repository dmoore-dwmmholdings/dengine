# src/host: module `host` (target de_host), layer host.
# Stub source only, until the sprint that owns this module adds real code.
de_add_module(
    NAME host
    LAYER host
    SOURCES host_stub.c
    DEPS
        core platform task reflect ecs asset anim input text sky terrain water decal streaming
        tilemap crash mods save
    OPTIONAL_DEPS gpu gpu_d3d12 gpu_vulkan gpu_metal render physics audio net ui vfx nav video steam
)
