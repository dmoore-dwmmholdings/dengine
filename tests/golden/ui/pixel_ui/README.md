# DENG-322 pixel UI diagnostic

`ui_pixel_four` renders retained UI at480×270 into a1920×1080 view target,
using the normal pixel-UI hook before integer4× upscale. It loads the actual
bitmap-font importer asset the local diagnostic.fnt/PNG (real8×8A/B glyphs with transparent interiors) and
shows ABBA at8/16/24 internal pixels alongside one-pixel borders and stripes.

The capture checks every RGBA sample of every4×4output block for exact equality,
so fractional filtering or a misplaced post-upscale UI pass cannot silently pass.
The target is the actual explicitly sized view texture, rather than the host's
1280×720default offscreen output. No palette or dither changes are enabled.
Only authentic Metal/Base references are adopted; thresholds are unchanged.

The old shared unit font atlas is solid white, so it is deliberately unsuitable
for this visual check. The local deterministic bitmap contains actual A/B shapes
and holes; it uses the same production BMFont importer and retained glyph path.

Final actual capture passes all4×4RGBA block equality checks (zero mismatches),
shows readable alpha-masked letters at allthree sizes, and strict FLIP0. Initial
blank capture exposed a missing render-target usage flag on output.basic; the
renderer owner fixed that actual pixel path before any reference was accepted.
