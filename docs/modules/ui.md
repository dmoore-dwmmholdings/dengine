# UI

## Retained UI and layout

The retained surface extends the existing `de_ui.h` components. It uses ECS entities
and `ChildOf`/`Children` order. `DeUiNode` is the layout primitive; Text and Image
supply intrinsic content. Anchor and Grid compose HUD placement and track layouts;
Popup composes an anchored overlay. Style holds resolved visual values, and
InteractionState holds hover, press, focus and disabled state for later input/styles.

These components share one layout system rather than separate widget layouts.
Nodes use reference pixels, percentages or intrinsic AUTO sizing. Grid tracks may
also use fractional units. The flex subset is single-line: row/column and reverse,
grow/shrink, min/max, margins/padding, gaps, justification and alignment. Wrapping
containers are outside this version. UI rendering, widget interaction, focus and
binding are separate S29 issues; this foundation does not invent their behavior.

Authored labels use the bounded inline `DeUiText.text` string or localization ID.
The existing `literal` pointer remains an optional runtime override and is not
serialized. Computed rectangles are read-only derived physical pixels. A static UI
uses cached intrinsic measurements; changes propagate through ancestors and only
changed subtrees are arranged. Viewport/safe-inset overrides support headless tests.

Create with `de_ui_create`, then attach once to an ECS world. `ui.layout` runs in
PostUpdate; style systems can order before it, while extraction orders after
layout. The intrinsic measure registry is extensible by component. Layout scratch
uses the active frame arena, or a persistent virtual arena when running standalone;
entity records and intrinsic measurements persist across frames.

`reference_height` defaults to 1080, `max_nodes` to 65536. `ui.scale` multiplies the
reference scale. Pass `desc.window` for automatic physical output size and native
safe-area insets; renderer-only configurations obtain public output dimensions.
`de_ui_set_viewport` explicitly overrides both sources and supplies headless safe
insets. Grid indices are zero-based; unspecified items auto-place row-major.
Popup placements are below/above/right/left and clamp against viewport safe insets.

The module uses public renderer queries for loaded image dimensions and output
size. A loaded image's intrinsic dimensions include its UV rectangle. Localization
uses the text system's installed callback; translated content is hashed so locale
changes invalidate measurements. The S28 layout defects remain separately tracked.

Focused host verification covers exact flex/grid rectangles, reverse and span
placement, anchors/popups, hierarchy edits, safe areas, scaling, real-font wrapping,
loaded texture UV dimensions, and reflected serialization. Dev and ASan/UBSan each
pass 14 tests. The 5000-node layout is below the 2 ms budget; the static 2000-node,
1000-frame test observes zero heap allocations across all built-in memory tags.
Retained rendering and interaction are delivered by subsequent S29 issues.
