# Startup artwork

`generate_splash.py --font <Onest-VF-latin.woff2> --dide <DIDE checkout>` generates the native embedded PNG/header and matching browser atlas/shader. Run on macOS with Swift/CoreText, NumPy, SciPy and Pillow. These are developer tools, not runtime dependencies.

The exact Onest v11 input hash is checked against DIDE's pinned font. `splash_type.swift` uses actual ExtraBold/SemiBold/Medium glyph outlines and kerning. The upper atlas band holds a 16-bit signed distance field in RG; lower bands hold wordmark/caption coverage in B. Runtime artwork is embedded and never depends on a game font or filesystem asset. The OFL text accompanies the derived artwork.

The renderer body is canonical in `shaders/startup/brand_body.slang`; the browser GLSL is generated from its restricted shared dialect. The native target writes to sRGB; browser output applies the equivalent encoding explicitly. The browser's static fallback is a render of the same artwork, not a second logo design.
