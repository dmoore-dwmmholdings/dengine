# Font atlas inspection

This Dev-only sample exposes real baked and generated MSDF atlas pages to DIDE's
font viewer. It loads the repository's pinned Noto Sans and generated CJK test
fonts from `tests/data/fonts`; it does not download fonts or require a GPU.

```sh
cmake --build build/dev --target font_atlas_demo
build/dev/examples/font_atlas/font_atlas_demo/font_atlas_demo --headless --frames 120
```

For an editor session, use that executable in an engine launch configuration with
`args = ["--headless"]`. Open the Assets panel and the `font_atlas/latin` font
viewer. The small 32×32 pages deliberately spread ASCII across multiple baked
pages. Shaping `AÄ一丁` requests Ä outside the baked charset; background generation
adds a runtime page. The CJK face is an explicit fallback. Page selection reads
the current CPU atlas, including runtime pages; it does not reconstruct a font
atlas from the source file. The five text samples use source fonts through the
editor's ordered fallback chain, separately from the engine atlas image.

The public `de_text_copy_font_atlas` API queries page metadata or copies into a
caller-owned RGBA8 buffer, without allocations or borrowed pixel lifetimes. Call
it on the main thread between schedules and asset updates. `max_edge = 0` copies
the full page; a nonzero limit fits it with nearest sampling. Single-channel
bitmap/pixel coverage becomes opaque grayscale. Four-channel MSDF values are
preserved, including the fourth distance channel. DIDE displays the RGB channels
with opaque alpha so distance data does not become image transparency.

Editor protocol 1.2 adds the optional `assets.fontAtlas` capability and
`asset.fontAtlas` request, bounded to one page with a maximum 1024-pixel edge
(4 MiB RGBA). Older compatible peers negotiate down and do not advertise the new
method. `sourceRevision` distinguishes decoded font lifetimes; `generation` is a
diagnostic counter, not a content hash (see DENG-690). Refresh after source changes.

The focused native DIDE acceptance test copies the sources into a disposable
project, verifies baked/runtime pixels, samples and locale refresh, and supports
four-theme captures. From the sibling `dide` repository, using a current harness
build:

```sh
DIDE_E2E_BIN="$PWD/target/debug/dide" \
DIDE_FONT_ATLAS_GAME="$PWD/../engine/build/dev/examples/font_atlas/font_atlas_demo/font_atlas_demo" \
DIDE_FONT_ATLAS_ENGINE="$PWD/../engine" \
cargo test -p dide-e2e --test asset-viewers real_font_atlas_pages_and_locale_refresh -- --nocapture
```

Set `DIDE_VIEWER_CAPTURE_DIR` to save actual native viewer screenshots in all four
themes. Both font test environment variables are required when either is set.
