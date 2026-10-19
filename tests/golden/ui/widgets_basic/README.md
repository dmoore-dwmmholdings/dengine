# Default widget state sheet

`ui_widgets_basic` renders the actual embedded default TOML through retained ECS
layout, style resolution, draw generation, immutable Extract, and the Metal UI
composite. Rows show button, toggle, slider, progress and nine-slice image content;
columns show normal, hover, pressed and disabled state. NotoSans MSDF labels are
real cooked font assets. The passive progress samples also demonstrate four values.

The native surface uses existing S29 blue tokens, an8px spacing rhythm and6px
corners. Toggle position, slider fill/knob and progress length communicate value
without relying only on color. Disabled controls have visibly reduced emphasis.
Actual keyboard/gamepad focus is DENG-320; this issue does not invent focus behavior.

Sequential native review checked clear labels, aligned columns, unclipped controls,
state differentiation and meaningful fill/image content. The root reviewer accepted
the captured image. Existing MSDF thin seams remain DENG-513; no font repair here.
The strict Metal comparison reports FLIP mean/p99/max0. Backend-specific Vulkan and
D3D12 references still require their target hardware (existing DENG-496).

Test-only renderer-private headers provide the unchanged S10 host-kick/offscreen
capture convention. Production UI pointer and drawing paths use public APIs and
the CPU ordering/emitter contract, with no renderer-private dependency.
