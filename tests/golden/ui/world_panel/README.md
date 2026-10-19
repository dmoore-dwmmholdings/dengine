# DENG-322 world-space UI diagnostics

These scenes use real cooked NotoSans MSDF glyphs, reflected retained components,
normal PostUpdate/Extract, and the renderer's forward participant pass. Panel
layout is640×360pixels on a1.6×0.9metre local-XY plane at400pixels/metre.

- `ui_world_1m`: camera1metre from the panel.
- `ui_world_20m`: the identical physical panel at20metres, with telephoto framing
  to retain the same inspectable screen footprint. Text is evaluated by MSDF
  per screen pixel; no panel render-to-texture or font-size enlargement. A second
  actual20m readback keeps the fixed1.2-radian base FOV and compares panel-visible
  vs panel-hidden pixels, asserting a small100..5000pixel footprint.
- `ui_world_occluded`: real opaque XY sprite wall in front of depth-tested UI.
- `ui_world_overlay`: the same wall with panel depth testing disabled.
- `ui_world_lit`: actual colored clustered point light, with the panel lit flag.
- `ui_world_origin_100km`: root at(100000,100000,0), camera translated equally;
  compare this actual capture with ui_world_1m to check camera-relative stability.

References are Metal/Base only; other backends require genuine supported hardware
captures. Original S10 mean0.001/p99 0.015 thresholds remain unchanged. Known
runtime font artifacts remain separately tracked; these fixtures do not modify
font generation, shaping or atlas lifetime.

Native Metal verification (Apple M5 Pro, validation instrumentation disabled to avoid
known508): all7 new references compare with mean/p99/max FLIP0 at unchanged
0.001/0.015 thresholds. A real wrong depth reference fails (mean0.203216,
p990.982618, exit1). The bounded relative-vertex projection fix makes the fresh
origin and100km captures raw-pixel identical and cross-reference FLIP0. The
fixed-FOV20m supplemental capture covers1008pixels; optional environment
DE_UI_FAR_CAPTURE writes that actual readback PNG for inspection.

The same13-scene regression batch passes the7new scenes plus widgets_basic,
retaining five preexisting317screen-reference failures, independently reproduced
on public gameplay0a63fb4 with identical metrics (DENG-524). Existing references
were not regenerated. Only Metal was captured; no other backend claims.
