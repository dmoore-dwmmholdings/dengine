# Retained UI rendering diagnostics

These are engine component comparisons, not game interface proposals. They use the established
S10 harness unchanged and production layout, draw-list generation, Extract, font/texture cooking,
and the renderer UI composite. Components are spawned through public ECS APIs. Renderer-private
includes only provide the same test host-kick/offscreen-readback glue used by existing scenes.

The component inventory is deliberately small: rounded fill/border, nine-slice image, nested
axis-aligned clip, sibling z order/popup layer, and retained MSDF labels. A quiet cool palette,
4/8-pixel spacing and NotoSans from the existing test assets make edges and labels legible.
There are no fabricated application controls, interactions, statistics, or new brand assets.

- `ui_rect_border`: left half is an actual 1x render (`reference_height=720`); right half is the
  left640pixels of a separate actual2x render (`reference_height=360`). The same authored rectangles
  have 1/3pixel reference borders and8/20pixel reference radii. Both use a1280x720 physical output;
  layout stats must report the requested scale. The harness sees one side-by-side diagnostic.
- `ui_nine_slice`: the real cooked32x32 texture `panel.png` has8pixel marked corners. Three panels
  stretch only the centre/edges to160x96,320x176,480x288. Corner marks must retain identical size.
- `ui_clip_nested`: child stripes extend beyond two clipping ancestors; only their AABB intersection
  appears. Rounded clip masks are intentionally outside317scope.
- `ui_z_popup`: z10/20 siblings overlap; a popup with z-100 draws over both by its popup layer.
- `ui_text_labels`: real cooked MSDF font labels at16/24/40/64physical pixels. All scenes also use
  actual retained MSDF captions, so missing text cannot silently pass the nonblank guard.

`panel.png` is original deterministic diagnostic data generated from integer colour/corner marks.
Font source and license stay in `tests/data/fonts/`. Strict FLIP limits remain mean0.001/p990.015.
Only native Metal references may be added here; no references are fabricated for other backends.
A separate capture must be visually inspected for clipping, borders, text, ordering and corner
size before reference approval. Repeat comparison must pass unchanged thresholds afterward.

Native capture review found two retained text defects after runtime glyph work settled:
DENG-512 (`fi` ligature missing from the nine-slice caption) and DENG-513 (plain-fill glyph-edge
seams/clipped stroke, including64pixel `complete` and18pixel leading `3`). Captures intentionally
preserve these visible defects; no disabled ligatures, alternate strings, text shader edits or
relaxed FLIP limits hide them. Shared shadow-specific DENG-493 is distinct. Geometry probes also
assert identical8x8corner patterns across panel sizes (2code-value tolerance), no clip spill,
and popup-over-sibling colour ordering. Static diagnostics have no interactive state controls;
keyboard/hover/motion behaviour belongs to the widget tasks, outside these render fixtures.
