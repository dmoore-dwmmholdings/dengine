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

DENG-318 final verification: all8 new acceptance cases pass in Dev: synthetic
capture/click actions; clips/z-order/popups/root-local hit; disabled state;37% slider
snapping/clamping/change events; table callbacks; actual progress fills/reattach;
500widgets×1000frames with zero steady-state heap allocations; and an asynchronous
real locale READY switch that relayouts only the localized nodes and ancestors.
The full Dev suite passes28/31, retaining DENG-511/515/516 as strict failures.
Seven new cases pass the initial ASan/UBSan aggregate; the locale case passes its
isolated rerun after correcting that new test's cleanup to match host text-before-
assets shutdown. DENG-517 tracks the unchanged legacy fixture's inverse order.
ASan aggregate timing remains separately qualified (theme1ms budget under Dev).
Public headers/layering and all21 unique UI C sources compile with render/GPU off.
The actual Metal widget state sheet passes capture and strict comparison with FLIP0,
and the native visual review is accepted. No global suite or CI repair loop was run.

### Navigation (DENG-320)

Add `DeUiFocusable` to each navigable node. `tab_index` sorts Tab/shoulder traversal, then
hierarchy insertion order; negative values opt out of Tab but remain explicitly focusable.
Directional links override geometric selection. Otherwise navigation compares rect edge gaps
in the requested half-plane (primary gap + twice the perpendicular gap), with stable tree
order for ties and no implicit spatial wrap. Hidden or disabled ancestors exclude descendants.
A visible modal `DeUiPopup` traps navigation in its subtree; hiding/despawning it restores the
previous live, eligible focus. Nested popup cancellation unwinds one scope at a time.

`de_ui_set_focus` changes logical focus and emits FOCUS/BLUR. `DeUiInteractionState.focused`
also remains logical, including in mouse mode, so text editors keep their active selection.
`de_ui_focus_visible` separately reports keyboard/gamepad ring visibility. Mouse movement hides
only the stylesheet's `:focus` selector; the next navigation/activation restores it. Default
button/toggle/slider rings use a 2-pixel foreground border (3.27:1 against the primary blue,
above 3:1 against the surface). Existing widget hover/pressed/disabled transitions remain.

At attach, UI creates a rebindable normal action map named `ui` only when absent. A preloaded
map is preserved. Its navigate/submit/cancel/tab_next/tab_prev/scroll actions bind arrows,
Enter/Space/Escape/Tab/Shift+Tab and gamepad d-pad/left stick/south/east/shoulders/right stick;
pointer_motion is an additional recordable mouse-delta action. `de_ui_set_input_mode` selects
NONE, NON_MODAL (default passthrough) or MODAL (consumes bound input). Modal popups temporarily
force consumption. UI pops only a context it pushed and restores preexisting map flags at
detach. The input object must outlive UI; UI attaches it if the world has no input resource
and detaches only that owned attachment.

`ui.actions` reads public resolved actions in main-thread FixedUpdate after `input.fixed_tick`.
`ui.focus` applies the staged samples in PostUpdate after widget reconciliation and before
style/layout. This uses S21's actual tick replay view: a frame with zero fixed ticks retains
pending input for the next tick; multiple fixed ticks do not duplicate a press. Repeat time
is accumulated from fixed deltas (400 ms initial delay, then 80 ms), never the wall clock.
The preallocated queue holds at least 256 ticks (or max_nodes when larger); drain PostUpdate
between larger catch-up batches. Overflow reports an error rather than allocating.

Activation and slider adjustments call the stable widget entry table. Its nullable
scroll_into_view hook receives the nearest scroll/list ancestor on focus changes; advanced
controls register their own actions and scrolling implementation. No scroll stub is linked.
`test_menu_replay.c` is an executable sample menu using real button/toggle/slider components:
a gamepad walks Play, Music, Volume and Quit, cancels a modal confirmation, then confirms exit.
The test saves `menu_replay/sample_menu.deir`, creates a fresh input/UI/world with no devices,
and checks every replay tick's focus, event trace, resolved-action checksum and widget values.
Later advanced-widget walkthrough extensions use the same table and recording schema.

### Advanced controls (DENG-319)

`DeUiTextInput` owns at most255 UTF-8 bytes plus its terminator. The runtime derives
length from the bounded buffer, snaps caret/selection to the public S28 extended-grapheme
boundaries, and keeps preedit in a separate display component. A committed input event emits
CHANGED; preedit never changes the authored buffer. `DeUiText.literal` is the display view
and may contain password bullets or composition, so bindings must read the reflected
`DeUiTextInput.buffer` rather than the display. Clipboard copying is disabled for passwords.
Public layout hit-test/caret APIs supply bidi-aware mouse positioning and caret geometry;
single-line controls scroll their physical text origin while preserving the inherited clip.
The public grapheme wrapper uses stack scratch through255bytes, temporary heap for larger
inputs, returns required boundary count, and documents invalid UTF-8/capacity/error behavior.

The keyboard-only blocking `ui.text_edit` action context is created once at attach. It
consumes editor navigation/Enter/Space/Escape while leaving gamepad bindings available.
Push/pop follow the existing S21 next-frame context semantics. Escape suspends editing even
when a modal restores the same logical focus; explicit activation resumes it. IME candidate
areas use the public input-to-platform bridge in logical window points. Headless platforms
retain editing state while the OS keyboard/candidate APIs report their normal unsupported result.

Dropdown options are localization IDs. A real modal popup flips upward when needed; outside
click/cancel closes it and selection emits one CHANGED. Scroll controls clip children, clamp
physical offsets, integrate decaying wheel momentum, draw draggable bars, and expose
`de_ui_scroll_offset` and child-based `de_ui_scroll_to`, plus the navigation scroll-into-view hook. A list materializes at most
ceil(viewport/item-height)+2 rows, independently of100000-item data size. Configured prefabs
are instantiated through the real asset API. Rows rotate their index mapping and emit
BIND_ITEM only when their represented index changes; applications populate those row entities.
Tabs create header buttons and hide inactive child pages. Tooltips wait for hover/logical
focus, anchor near the pointer or target, clamp via popup safe-area layout, and hide on press/leave.

`ui.edit` runs in PreUpdate after pointer routing. `ui.advanced` reconciles structures and
refreshes borrowed display views in PostUpdate before basic widgets, focus, style and layout.
The nullable `refresh_text` widget-table hook lets bindings refresh display after committed
buffer writes without depending on private advanced-control structures. Runtime components
own no external heap pointers, and the fixed entity scratch buffer is allocated at UI creation.

DENG-521 is fixed: virtual-list pooling waits for a finite positive pixel scale and bounds
the visible row count before converting to an integer. The existing first-frame and advanced
gamepad replay regressions pass ASan/UBSan. Both actual Metal advanced scenes pass strict FLIP
comparison with mean/p99/max0 and visual review. DENG-508 limits shader-validation startup;
captures retain the debug layer with shader validation disabled. DENG-512 is fixed: public
atlas metadata includes runtime-generated pages as well as cooked pages, making cold password
bullets and generated ligatures visible. The original page-metadata regression passes and the
unchanged nine-slice scene renders its previously missing `fi` ligature. Existing font seams
remain tracked by DENG-513.

### Data binding (DENG-321)

Binding initialization safely unwinds after any state/cache/entity allocation failure;
detaching a partially initialized binding system is safe (DENG-519).

Attach `DeUiBinding` to a widget, with its source entity, a path such as
`BindData.current` or `BindInventory.slots.2.count`, and a property. Dotted numeric
indices adapt to the reflection API's bracket form (`slots[2].count`); all actual
schema lookup, bounds, nested fields and flags use `de_reflect_resolve_path`.
Properties are `value` (slider/progress), `on` (toggle), `text`, `visible`
(NONE/FLEX), `disabled`, `opacity`, and `tint` (image tint, text color or panel fill).
Scalar properties convert bool, signed/unsigned integers and f32/f64. Tint accepts
vec4/color. Text copies inline strings into UI-owned storage, resolves names through
`de_loc`, and formats numeric values with optional `format[32]`, for example
`{value, number, integer} / 100`. Numeric inline formats reuse S28's message engine;
no expression language or reflection parser is embedded in UI. Default integer
text preserves the complete signed/unsigned64 range. S28's optional numeric format
uses a floating argument for unsigned values above INT64_MAX, with f64 precision.

Bindings default to ONE_WAY. TWO_WAY also reads normal widget CHANGED events in
main-thread PostUpdate and writes the reflected source with `de_entity_get` and
`de_entity_mark_changed`; READONLY fields are always protected. The next gameplay
Update's changed query observes the write. Editor integration is generic: committed
`DeUiTextInput.buffer/length` fields resolve through reflection, independently of
password/preedit display text, and a nullable widget-table `refresh_text` callback
updates display after an external buffer change. No advanced-widget code is linked
from the binding module; the actual text-input integration case follows DENG-319.

`ui.binding` runs after widget/navigation actions and before style/layout. Each
binding caches its descriptor, resolved component/field/offset and a byte copy of
the source leaf. Unchanged source bytes avoid mutable component access and layout
invalidation. Locale publication refreshes localized/formatted text even if source
bytes did not change. Any successful type registry publication invalidates both
valid and errored caches, including moved offsets and metadata-only changes.
Invalid paths/kinds/formats log once per binding/configuration/revision and disable
that binding until a descriptor or registry change. A dead/missing source simply
idles and preserves the last visible value. Text and byte spans live in a per-UI
arena, reused when capacity suffices; they remain valid through detach and end at
UI destruction. The warmed idle frame path performs no heap allocations.

Focused binding tests cover all numeric kinds, extreme64-bit text, every property,
nested/array paths, `42 / 100`, actual slider/toggle writeback plus changed queries,
readonly fields, schema/metadata replacement, warning-once errors, source despawn,
real cooked locale publication, and1000 idle bindings within0.2ms in Dev.

### World panels and pixel UI (DENG-322)

Attach `DeUiWorldPanel` and a normal `DeTransform` to a UI root. `size_m` defines
its physical XY plane and `pixels_per_meter` defines its independent layout
extent. A 2×1 metre panel at100 pixels/metre lays out as200×100 pixels, without
changing screen roots or other panels. Panel-local origin is its upper-left;
the transform positions its centre. `lit` selects the renderer's existing
clustered direct/indirect lighting and `depth_test` selects reverse-Z scene
occlusion. With depth disabled, the panel joins the final overlay group.

World panels emit direct rounded/image/text quads, using the same MSDF and bitmap
sampling as screen UI. They participate in the renderer's ordinary transparent
sort, including interleaving with translucent meshes. Sorting is per root, so
intersecting surfaces retain the renderer's object-sort limitation. Geometry and
asset references come from the immutable Extract packet; render callbacks read
neither ECS nor asset state. Double-precision panel positions are reduced by the
matching camera origin before conversion to GPU floats.

Pointer routing unprojects the selected view's physical pointer position, tests
each transformed panel plane, and passes panel-local pixels to the normal widget
hit test. Existing screen widgets take precedence; a captured world widget keeps
its panel routing while dragged. Focus and activation use the existing widget
and navigation paths. Set `de_ui_desc.view` to select a view; zero uses the main
view for world pointer projection.

For pixel UI, set `de_ui_desc.pixel_art` and enable integer pixel rendering on
the selected view. Layout uses the view's internal pixel grid and an integer
`ui.scale`; rectangle, clip and glyph edges snap to that grid. Bitmap font pages
use nearest sampling; MSDF fonts are not emitted in this mode. The pixel hook
composites after internal tonemapping and before palette conversion/upscale, so
UI shares scene palette, scale and letterboxing. Pointer mapping uses the actual
fitted content rectangle. A resize/settings mismatch between PostUpdate and
Extract safely skips that packet until layout catches up. Ordinary screen UI
with no explicit view retains its existing output-resolution behavior.

Focused CPU tests cover per-root extent isolation, rotated/scaled ray-plane
mapping, a projected button at100 km, immutable panel poses, integer snapping,
letterbox routing and advisory-resize rejection. A Metal readback of the actual
world UI consumer returns RGB0/212/0 at the panel centre; translating both camera
and panel by100 km changes zero image codes beyond one step. The static2000-node
HUD test measures1000 complete UI frames plus Extract with zero heap allocations:
0.321ms mean in Dev on the tested Apple M5 Pro host; this is not an M1 hardware
qualification. DENG-523 now constrains world-root popups to their panel-local
extent at scale 1, independently of screen DPI and safe insets. The terminal demo
checks autocomplete/menu placement at the CRT edge and after panel resizing.

The seven Metal/Base world/pixel references use the original mean0.001/p990.015
limits. Pixel output additionally checks exact equality within every4×4 block;
the100km MSDF image matches the origin image with mean/p99/max zero after
camera translation is cancelled before local glyph geometry is projected.
Five older screen-UI references currently fail identically on the pre-322
baseline because of a text-style tint difference; their references are preserved.
The existing eleven draw regressions pass, including real native brightness,
Extract-token selection and image-pin retirement. Headless syntax and public
header/layering checks pass. Vulkan/D3D12 native captures remain hardware checks.

S29 exit verification now also runs a110-tick real gamepad-only advanced menu in
`test_menu_replay.c`, preserving the original40-tick basic menu. The game authors
public components/tab order and populates real virtual rows through BIND_ITEM;
all activation uses actual resolved S21 actions and the production widget table.
It visits13 main/generated controls and all6 pooled list buttons, activates the
editor, selects a real dropdown option, exercises tabs and both generated headers,
reveals a fully visible offscreen focused button through scrolling, then cancels
and confirms exit. The saved `menu_replay/advanced_menu.deir` replays in a fresh
UI/world/input with no device or event injection; per-tick focus, event/value hash,
action checksum, editor/buffer hash, selection, tab and scroll state agree.


### Script documents and terminal integration

`DeUiTextInput.buffer` remains the legacy 255-byte inline field. Real widgets promote
into UI-owned growable storage when editing exceeds it; use
`de_ui_text_input_set(ui, entity, text)` to load a document and
`de_ui_text_input_get(ui, entity)` to read the full committed UTF-8 document.
`length` and `caret` remain byte offsets; do not read/write `buffer` after promotion.
Set copies the source, clears history, resets selection/scroll, and preserves the
previous document on allocation failure. Documents have no fixed widget byte cap
(the text segmenter requires length below INT32_MAX); memory remains the limit.
Runtime document/history data is not an ECS snapshot or serialized component: a
save system must explicitly persist the committed text and restore it with set.
Memory is reclaimed when the component/entity disappears (at the next UI frame)
or immediately when UI detaches/destroys. Borrowed get/display pointers expire on
edit, set, removal or detach.

Each input retains 128 committed edit transactions with deleted/inserted bytes
and original selection. Ctrl/Cmd+Z undoes; Ctrl/Cmd+Shift+Z or Ctrl+Y redoes. A new
edit discards the redo branch. Unicode grapheme boundaries apply to selection,
movement and deletion. Multiline Up/Down uses shaped visual lines and keeps the
preferred horizontal position across short lines; Shift extends selection.
Home/End moves within the logical line; Ctrl/Cmd+Home/End moves to document ends.
The editor scrolls vertically to keep the caret visible. Clipboard operations use
full document-sized buffers. IME preedit is displayed separately from committed
text, including for document-backed and password inputs.

`de_ui_text_input_apply` exposes SELECT/REPLACE/UNDO/REDO commands. An optional
`de_ui_set_text_edit_observer` receives successful commands synchronously; copy
borrowed payloads before returning and do not reenter editing from the callback.
This supports deterministic game journals without recording OS devices. Transient
IME preedit is not recorded. The existing `.deir` format still records resolved
fixed-tick game actions, not text or raw mouse selections; the terminal example
uses an explicit companion `.tuij` journal for committed edits and mode changes.

See `examples/terminal_demo/README.md` for the runnable world-panel example and
its walking/editing focus and capture handoff, paused aircraft marker, CRT-bounded
completion menu, and save/replay controls. Pausing the aircraft does not pause UI
PreUpdate/PostUpdate; integration coverage also omits FixedUpdate while editing.
The example demonstrates editor integration, not a scripting-language runtime or
the game's full aircraft simulation. A new candidate pin does not change the
game project's accepted submodule revision automatically.
