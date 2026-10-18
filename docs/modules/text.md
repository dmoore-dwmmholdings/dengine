# Text

## Rendering

Call `de_text_attach(text, renderer, world)` after creating the renderer and world.
Passing a null renderer detaches rendering; shaping and CPU font queries remain usable.
Destroy or detach text before destroying its renderer or world. Call `de_text_update`
after asset updates and before submitting the frame's text.

`de_text_draw` copies glyph quads into the current frame's submission list. `pos` is
x, y, width, height in pixels; `uv` is minimum/maximum atlas UV. The options identify
the font, clip rectangle (x, y, width, height; nonpositive extent disables clipping),
outline, shadow, and integer pixel snapping. Submissions retain their order within a
view. A single instanced draw handles different atlas pages and bitmap/MSDF fonts.
Atlas textures are available through `de_text_page_texture` after a submitted frame
has first used that page. GPU work uploads complete new pages and only dirty regions
of initialized pages; `de_text_get_render_stats` exposes frame counters.

MSDF coverage uses the median RGB distance and derivative-based pixel range, clamped
to one pixel at minification. MTSDF alpha supplies shadow coverage. Bitmap glyphs
use nearest sampling. Screen text runs in the UI output stage; `r.ui_white_nits`
controls white brightness when HDR output is active. Actual HDR display composition
still depends on the renderer's output implementation.

Attach registers the `DeWorldText` ECS component and its Extract system. Add it beside
a transform, set a literal or localization ID, font/color/effects, size in meters,
alignment, billboarding, lighting, and depth testing. World text uses camera-relative
double positions and the forward lighting path. Its current single-line glyph layout
is isolated for replacement by the paragraph layout implementation.

The renderer must remain alive until text detaches. GPU atlas generations currently
remain allocated until detach (DENG-490); repeated hot reloads can exhaust the page
cache. Render-disabled builds compile text without GPU rendering code. Metal native
readback and golden scenes live under `tests/unit/text` and `tests/golden/text`.
