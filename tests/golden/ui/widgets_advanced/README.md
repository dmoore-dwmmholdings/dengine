# Advanced retained controls

These two1280×720 native scenes use the actual S29 ECS/style/layout/Extract/UI pass.
`ui_widgets_advanced_text` shows selection, injected Japanese-kanji preedit and its
underline, nine password grapheme bullets, and multiline content. Committed text
remains separate from preedit. The font manifest explicitly cooks U+2022; this is a
real font glyph, not a painted stand-in. `ui_widgets_advanced_controls` shows localized
Easy/Normal/Hard options with selected state, a100000-item virtual list scrolled to
item100, real scrollbar geometry, generated tab headers, and a localized tooltip.

The existing default TOML supplies primary#3a7bff, foreground#e7edf6, surface#233044,
border#52647e,6px corners and0.12s transitions. Labels use the same real cooked NotoSans
as the other UI references, with real CJK fallback for preedit. Sequential review
checked readable hierarchy, alignment, selection/focus distinction, popup layering,
meaningful data rows, clipping and interaction feedback. Both corrected native images
were visually approved. Strict Metal comparison reports FLIP mean/p99/max0 for both.

Known qualifications: DENG-512 rejects metadata for runtime atlas pages, so uncooked
ligatures can disappear and cold password bullets need the explicit cooked glyph.
The failing public page-metadata regression remains in test_text_input.c. DENG-513
tracks existing thin MSDF seams. DENG-521 retains the first-layout virtual-list
zero-scale UBSan failure. No unrelated renderer/text repair was included.

Metal debug-layer capture/comparison passed with MTL_SHADER_VALIDATION=0. The original
validation1 capture was stopped at five minutes in the sampled Apple RT pipeline
compiler, before UI execution, matching DENG-508. Thresholds were unchanged. Vulkan
and D3D12 image references still require their target hardware (existing DENG-496).
Test-only renderer-private headers provide the unchanged S10 host-kick/readback path;
production controls use public text/input APIs and the existing CPU draw emitter.
