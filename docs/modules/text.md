# Text

The optional text module owns CPU fonts, fallback resolution, HarfBuzz shaping,
Unicode paragraph layout, runtime MTSDF glyph generation, and optional GPU rendering.
Create it with the asset system, update after assets each frame, and destroy it
before its assets. The host performs that lifecycle when the module is enabled.

## Import

Font assets accept TrueType/OpenType outlines, BMFont v3 text or binary files with
PNG pages, and PNG pixel fonts with a TOML mapping sidecar. Outline fonts cook to
DFNT v1 with a deterministic MTSDF atlas; bitmap fonts preserve source pixels and
metrics. Manifest settings include `em_px`, `px_range`, `page_size`, `charset`,
`chars` and codepoint `ranges`. Ranges accept inclusive integer pairs such as
`ranges = [[256, 591], [0x400, 0x4ff]]` or the existing string syntax
`ranges = ["0x100-0x24f"]`. Pairs require two ordered integers in `0..0x10ffff`;
surrogate codepoints are excluded. The
pixel sidecar accepts explicit numeric `cell = [8,8]` and per-character advances.

The importer validates font/BMFont inputs, character ranges and page bounds.
HarfBuzz 14.3.1 and msdfgen 1.13 are pinned under `third_party`; fonts in tests have
redistributable license files beside their data.

## Runtime, fallback and shaping

`de_text_font_metrics` returns size-scaled metrics and the current font generation.
`de_text_set_fallbacks` configures up to eight fallback fonts per primary. A bounded
codepoint cache resolves each character to its actual font and glyph. Font reloads
and fallback changes invalidate resolution and shaping cache inputs.

Shaping validates UTF8 with U+FFFD replacement, itemizes font/script runs, and uses
HarfBuzz for glyph IDs, logical byte clusters, advances and offsets. Kerning,
standard/contextual ligatures and contextual alternates can be disabled with
`DE_TEXT_DISABLE_*`; language is BCP-47. Thread-local shaping buffers and striped
bounded caches support worker callers. Inherited combining marks stay with the
preceding base font when it covers them, preserving a single shaping run. Marks
that it cannot cover retain their resolved fallback font.

An outline glyph absent from the cooked atlas queues one deduplicated background
MTSDF job. `de_text_update` integrates completed jobs in deterministic order with
per-frame and page limits, increments font generations, and publishes dirty rects.
Missing glyphs keep their advances while their quads are omitted. Bitmap fonts
never generate runtime outline glyphs. Task-record OOM on external or frame callers
keeps the glyph pending for a later background-worker retry. Dirty upload rectangles
coalesce by page when their bounded list fills.

## Paragraph layout

`de_text_layout_build` combines SheenBidi 2.7, HarfBuzz and Unicode 15.1 generated
properties. It handles paragraph direction, visual line ordering, UAX #14 line breaks,
grapheme-safe emergency wrapping, start/center/end/justify alignment, spacing,
line height, fixed-width tabs, and optional integer snapping. Invalid UTF8 becomes
U+FFFD while queries use original input byte offsets. `max_width = 0` disables wrapping.

The result is immutable and refcounted. Release it with `de_text_layout_free`.
`de_text_layout_size`, `de_text_layout_hit_test`, `de_text_layout_caret`, and
`de_text_layout_quads` expose measurement, cluster-aware editing and visible glyphs.
Every quad identifies its actual fallback font. Carets use leading affinity inside
a cluster; visual hit testing chooses the nearest cluster edge.

A striped bounded cache keys all description/style inputs and fallback configuration.
When atlas generations change, a new immutable result rebuilds quads from retained
positions; older results remain valid and runtime arrivals do not reflow the text.
Default-heap layouts use the shared cache. Explicit custom allocators, including arenas,
produce uncached layouts: release those layouts before resetting/destroying their storage.
The cache never retains caller-owned arena allocations after release (DENG-502).

All 91,707 official BidiCharacter cases, 10,274 LineBreak cases and 1,187 GraphemeBreak
cases pass with zero skips. The line breaker uses the numeric tailoring specified
by the official test file. Regenerate properties with
`python3 tools/unicode/gen_linebreak.py`; output is byte reproducible from vendored
Unicode inputs. Line wrapping records breaks after discardable trailing blanks before
testing the next visible glyph (DENG-499). Cache keys snapshot the fallback epoch after
registering the primary/fallback fonts, so the first repeat is a cache hit (DENG-500).
DENG-503 is fixed: paragraph splitting also honors UAX #9
paragraph separators U+001C..U+001E, keeping shaping within the bidi level array.
The original crash and separator/invalid-UTF8 regressions pass ASan/UBSan; a subsequent
61-second coverage-guided campaign completed 66,715 cases without a finding.

## Rendering

Call `de_text_attach(text, renderer, world)` after creating the renderer and world.
Passing a null renderer detaches rendering; CPU fonts, shaping and layout remain
available. Destroy or detach text before its renderer/world. Update text before
submitting that frame's quads.

For renderer-only upload warmup, finish those frames before attaching text. Attachment
seeds text's frame queue from the renderer's current frame. Thereafter call
`de_text_update` once before submissions for each rendered frame; extra renderer-only
frames advance the renderer without advancing the text queue. Use the uploader's
flush/acquire calls for upload progress that should not advance a render frame.

MSDF import normalizes contour orientation for both TrueType and CFF outlines,
including opposite-winding counters. Font importer version2 invalidates prior
cooked atlases so CFF assets affected by inverted coverage are rebuilt.

`de_text_draw` copies quads into the current frame list. `pos` is x,y,width,height
in pixels; `uv` is minimum/maximum atlas UV. Each quad's nonzero font selects its
fallback atlas; a zero font preserves `options.font`. Options also identify the
clip rectangle, outline, shadow, and integer snapping. Submissions retain order
within each view. One instanced draw handles mixed fonts/pages and bitmap/MSDF glyphs.

`de_text_page_texture` exposes pages after first frame use. New pages upload once;
initialized pages upload only dirty regions through renderer staging and a bounded
compute region copy. `de_text_get_render_stats` returns per-frame counters.

MSDF coverage uses median RGB distance and derivative-based pixel range clamped to
one pixel. MTSDF alpha supplies shadows. Bitmap fonts use nearest sampling. Screen
text uses final output dimensions at the UI stage after pixel upscaling; world text
uses internal dimensions and the S14 pre-tonemap HDR lighting target. HDR UI white
uses `r.ui_white_nits`; display composition still depends on S18 output.

Attach registers `DeWorldText` and an Extract system. A literal or localization ID,
font/color/effects, size in meters, alignment, billboarding, lighting and depth flags
control each entity. World text uses this same paragraph layout and camera-relative
double positions. Localization callbacks are reserved for DENG-264; a literal is
used when no callback supplies a localization ID.

Superseded atlas generations are collected; pending UI packets hold generation leases
until retirement. World text reuses visibility depth when present. Text-only views create
a cleared reverse-Z depth attachment, so no other drawable is required.
Fill and effect samples stay inside each glyph's atlas rectangle, including its padding.
Shadow coordinates outside that allocation contribute no coverage. Effect extent still
depends on the glyph quad and available padding.
Metal native pixel readbacks and four FLIP scenes live under `tests/unit/text` and
`tests/golden/text`; other backend captures remain DENG-496. Render-disabled builds
compile the CPU module independently.

## Inline formatting and binding cache prerequisites (DENG-321)

`de_loc_format_pattern(text, pattern, args, count, out, capacity)` formats an inline UTF-8 span
with the existing localization message engine and the current published locale. It allocates
nothing, returns the full needed byte count, preserves UTF-8 boundaries on truncation, and
reports invalid patterns through the same error context as `de_loc_format`. A NULL text system
uses English. This is the formatting provider for authored UI binding patterns such as
`{value, number, integer} / 100`; it does not add a string-table entry or duplicate the parser.

Binding caches can also observe `de_type_registry_revision` from `de_reflect.h`: every successful
struct/enum registration or replacement advances it, including metadata/hook-only changes;
failed registrations and lookups leave it unchanged. Re-resolve both cached paths and previously
invalid paths when it changes. Actual UI binding/widget implementation remains a separate task.
