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

## Styles

Styles are TOML `ui_style` assets, or ordinary loaded blobs passed to
`de_ui_load_stylesheet`. Set `de_ui_desc.assets` at creation; obtain a registered
sheet handle with `de_ui_stylesheet(ui, asset_id)` and load/reference it through the
asset system before adding it. The custom loader validates before publication and
loads referenced fonts/background images, including theme variants, as dependencies.
Parsed objects are immutable and independent of UI lifetime. Each UI retains its
sheet; blob sheets parse their bytes when added. Reloads replace valid sheets and
preserve the prior sheet on errors, with a source line in the error message.

```toml
[vars]
primary = "#3a7bff"
[theme.dark.vars]
primary = "#7aa2ff"
[style.button]
background = "$primary"
radius = 6
padding = [8, 16]
transition = 0.12
[style."button:hover"]
background = "#5a95ff"
[style.".danger"]
text_color = "#ff4040"
```

This example is the issue-defined token source: primary color has base/dark
variants, spacing has 8/16 reference pixels, radius is 6, and motion lasts 0.12 s.
There is no implicit engine-wide color or typography theme. Applications define
those values in their own sheets. State appearance supports hover/focus/pressed/
disabled independently; keyboard/focus behavior is supplied by the input issues.

Selectors are widget types, `.class`, or `type.class`, optionally followed by one
state. Type specificity is 1, class 10, state 5; later rules/sheets win ties.
`DeUiWidgetType` supplies the type tag, and `DeUiNode.style_class` supplies the class.
`ui.style` runs before layout. It resolves changed class/type/state or sheet/theme
revisions; an unchanged frame reports zero restyled entities. Unset properties
retain authored component values. `de_ui_get_style_stats` exposes restyle/tween counts.

Properties are `background`, `background_image`, `nine_slice`, `border_color`,
`border_width`, `radius`, `opacity`, `text_color`, `font`, `font_size`, `padding`,
`margin`, `width`, `height`, `min_width`, `min_height`, `max_width`, `max_height`,
`gap`, `transition`, `easing`. Colors accept `#rgb`, `#rrggbb`, `#rrggbbaa` and convert
RGB from sRGB to linear while retaining alpha. Font/image values are asset IDs.
Nine-slice borders are `[left, top, right, bottom]` source pixels; all zero selects a
plain background image. Lengths accept numbers/`"Npx"`, `"N%"`, or `"auto"`.
Spacing accepts one/two/four pixel values in top/right/bottom/left order (two means
vertical/horizontal). Variables may reference variables, with bounded cycle checks.

`de_ui_set_theme(ui, "dark")` selects variable overrides; empty names select base
variables. Numeric/color state changes use `transition` and `easing`. Easing names
are `linear`, `quad-in/out/in-out`, `cubic-in/out/in-out`, `back-out`, `elastic-out`.
The public `de_ui_tween` preserves its original void/u32 API and accepts those
`de_ui_easing` enum constants. Paths such as `DeUiStyle.opacity` or
`DeUiNode.width.value` resolve through reflection and must identify f32 fields;
`opacity` and `radius` are style shorthands. Replacing a tween starts from its current
value. The fixed 4096-field pool allocates no per-frame tween storage.

Limits: 32 sheets, 512 rules per sheet, 4 MiB source per sheet. DENG-511 tracks the
retained failing regression for an interrupted state transition whose base rule
leaves the animated property unset. Explicit tween replacement and fully specified
state color transitions pass. Drawing consumes these resolved values in DENG-317.

Stylesheet verification: Dev CTest passes 22/23 cases, with DENG-511 retained as
a failing regression. ASan/UBSan passes 21/23: the same regression plus the 1 ms
theme benchmark (8.524 ms under instrumentation; Dev is 0.539 ms). Public C11/C++17
headers, layering, and all 11 unique UI C sources compile with render/GPU disabled.

## Retained rendering (DENG-317)

PostUpdate `ui.draw_list`, after layout, builds per-root commands in stable
(root order, popup layer, z order, tree order). Descendant opacity multiplies;
`clip` intersects an axis-aligned scissor with its parent. Rounded clip masks
remain unsupported. Commands include SDF rounded fill/border, tinted image,
CPU-expanded 9-slice image, and cached MSDF/bitmap text quads. Resolved stylesheet
background images/nine-slice values use the same image path. `text_common.slang`
is shared with ordinary text rendering. Per-root metadata distinguishes screen
and future world-panel routing without a renderer reading the UI hierarchy.

`ui.extract` is the only handoff from simulation commands to immutable render
packets. Four packets are keyed by the actual renderer Extract token, so a
future published UI update cannot replace the world frame being rendered. Slot
pins keep ordinary image/font generations alive after load release or entity
despawn; retirement waits for the packet's actual submitted GPU fence. Frames
that never invoke a UI hook retire against the CPU-completed Extract token.
When no packet is free, the newest UI update is dropped rather than overwriting
pending data. Existing same-slot hot reload can still retire a GPU generation
from an unsent packet; that separate resource-lease defect is DENG-514.

The public `DE_HOOK_UI` consumer uses one bindless pipeline and instanced quads
batched by contiguous scissor runs, on the tonemapped full-resolution target.
Each renderer upload-ring slot is written only after its reuse fence completes.
UI code includes public renderer APIs only: device and builtin pipeline access,
hook removal/drain, hook frame/format information, typed asset pins and texture
lookup. UI must be destroyed before its text system, renderer and asset system.
The internal widget emitter runs after node backgrounds/images and before text;
it automatically preserves the node's opacity, clip and ordering context.

Set `ui.render.max_quads` before UI creation to size CPU/GPU storage (default
65536). Runtime lowering also truncates; increasing beyond allocated storage
requires recreating the UI. Overflow logs once and records truncation.
`de_ui_get_render_stats` exposes quads, draw calls, roots, truncation and build +
Extract CPU time. Static warmed UI has no per-frame heap allocations; changed
text/layout cache entries can allocate and are not included in that claim.

`de_renderer_set_ui_output` validates finite positive UI-white and paper-white
nits. SDR/zero settings preserve scale 1. HDR linear output multiplies UI RGB by
UI-white/paper-white while preserving premultiplied alpha. Calibration is copied
with renderer frame settings. Actual floating-target tests at 80/203/400 nits,
including half-alpha composition, validate this linear brightness contract.
This is **not** qualification of the absent S18 PQ/scRGB swapchain encoders,
HDR metadata, or a physical display.

Host evidence: 10/10 draw tests; five Metal goldens repeat with FLIP mean/p99/max
zero at unchanged .001/.015 limits; the deliberate wrong-reference control
fails. Actual Metal 2000-widget HUD: 2001 quads, one draw, 0.1141 ms build +
Extract. Static CPU 2000-widget HUD: 1000 frames with zero allocations, mean
0.0764 ms. M1-specific timing and Vulkan/D3D12 remain needs-hw-verify. Known
stylesheet transition regression DENG-511 remains separate; visible font
ligature loss DENG-512 and plain-fill seams DENG-513 are retained in references.
Metal shader-instrumented renderer setup has known timeout DENG-508; these
native comparisons do not claim that instrumented suite passed.

## Basic widget provider (DENG-318)

Widgets remain plain reflected components on retained nodes: Text, Image, Button,
Toggle, Slider and Progress. `ui.widgets` runs before styles/layout, installs basic
widget tags and synchronizes button disabled state. Advanced tags (input and later
values) remain owned by advanced widget registration even when they carry text or
image content. `ui.pointer` runs in PreUpdate after the host updates input. Cursor
logical points convert to physical pixels using the actual window's two sizes.
`de_ui_pointer_consumed` tells gameplay when UI owns the hit/capture.

The internal contract in `widgets/widgets_internal.h` is shared by later S29 work:

- `de_ui__widget_table(ui)` returns the fixed per-UI entry table. Each type has
  optional `submit`, `increment`, `decrement`, `cancel` functions taking `(ui, entity)`.
  Basic widgets install button/toggle submit and slider increment/decrement. DENG-319
  owns advanced entry registration. DENG-320 calls entries without linking widget
  implementation symbols. The optional `scroll_into_view(ui, container, child)` is
  initially NULL; DENG-319 installs it and callers check it before use.
- `de_ui__hit_test(ui, root, root_local_pixels)` consumes the renderer's exact
  retained order, inherited axis-aligned clips and popup layers in reverse order.
  A zero root targets screen roots; a named root takes root-local physical pixels.
  Text inside an actionable ancestor targets that ancestor. Hidden/disabled
  ancestors reject their entire subtree.
- `de_ui__pointer_route` exposes press/release/down routing for root-local sources
  such as world-panel rays. Capture lasts until release; a click requires the same
  enabled target on press and release. Slider capture clamps and snaps its value.
- `de_ui__widget_event` sends the public `de_ui_event`. Buttons send CLICK with
  their authored action, toggles send CHANGED with0/1, sliders with the actual value.
  These are the same payloads used by later navigation and data binding.
- The renderer's optional CPU node emitter is registered after render attach and
  re-registered on reattach. Widget fills/knobs inherit the node's clip, opacity and
  order and enter the immutable Extract packet before text. No GPU draw reads ECS.

The default TOML sheet embeds at build time and loads at UI create. It uses the
existing blue tokens,8px spacing and6px corners; no font asset/size is imposed.
Later sheets override the defaults. Text resolves through public `de_loc`, and
locale generation marks localized nodes dirty after ready tables publish.

This is a coherent implementation provider; final DENG-318 native state-sheet
verification and full acceptance handoff are still pending. Known strict regressions
remain tracked as DENG-515 (first-repeat/full-tree layout cost), DENG-516 (explicit
basic selector tag without its corresponding behavior component), and DENG-511
(interrupted state transition from the preceding issue).
