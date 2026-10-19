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
