# Module pages

One page per engine module, named `docs/modules/<module>.md`. Start each page from
[`_template.md`](_template.md). Any task that changes a module's public API updates its page
in the same commit (`docs/agents/CONVENTIONS.md` §4), and adds the link in the **Page** column
below.

Layers are from SPEC §4.1. **Det.** marks the modules where the SPEC §5.7 determinism rules
apply (for `anim`, only the simulation side).

| Module | Source | Header | Layer | Det. | Page |
| --- | --- | --- | --- | --- | --- |
| core | `src/core/`, `src/core/math/` | `de_base.h`, `de_memory.h`, `de_math.h` | Core | | |
| platform | `src/platform/` | `de_platform.h` | Platform | | |
| task | `src/task/` | `de_task.h` | ECS, reflection, tasks | | |
| reflect | `src/reflect/` | `de_reflect.h` | ECS, reflection, tasks | | |
| ecs | `src/ecs/` | `de_ecs.h` | ECS, reflection, tasks | yes | |
| asset | `src/asset/` | `de_asset.h` | Asset system | | |
| gpu | `src/gpu/{d3d12,vulkan,metal}/` | `de_gpu.h` | GPU RHI | | |
| render | `src/render/` | `de_render.h` | Renderer | | |
| anim | `src/anim/` | `de_anim.h` | Gameplay | yes | |
| physics | `src/physics/`, `src/physics/2d/` | `de_physics.h` | Gameplay | yes | |
| audio | `src/audio/` | `de_audio.h` | Gameplay | | |
| input | `src/input/` | `de_input.h` | Gameplay | yes | |
| net | `src/net/` | `de_net.h` | Gameplay | yes | |
| ui | `src/ui/` | `de_ui.h` | Gameplay | | |
| text | `src/text/` | `de_text.h` | Gameplay | | |
| world/vfx | `src/world/vfx/` | `de_render.h` | Gameplay | | |
| world/sky | `src/world/sky/` | `de_render.h` | Gameplay | | |
| world/terrain | `src/world/terrain/` | `de_render.h` | Gameplay | | |
| world/water | `src/world/water/` | `de_render.h` | Gameplay | | |
| world/decal | `src/world/decal/` | `de_render.h` | Gameplay | | |
| world/streaming | `src/world/streaming/` | | Gameplay | | |
| gameplay/nav | `src/gameplay/nav/` | `de_gameplay.h` | Gameplay | yes | |
| gameplay/tilemap | `src/gameplay/tilemap/` | `de_gameplay.h` | Gameplay | | |
| gameplay/video | `src/gameplay/video/` | `de_gameplay.h` | Gameplay | | |
| services/steam | `src/services/steam/` | `de_services.h` | App host | | |
| services/crash | `src/services/crash/` | `de_debug.h` | App host | | |
| services/mods | `src/services/mods/` | `de_services.h` | App host | | |
| services/save | `src/services/save/` | | App host | | |
| services/bind | `src/services/bind/` | | App host | | |
| host | `src/host/` | `de_game.h`, `de_debug.h` | App host | | |

For a nested module, use a hyphen in the page name: `world/vfx` → `world-vfx.md`.
