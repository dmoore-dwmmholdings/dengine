# Live Aseprite UI panels

This Dev example connects the Aseprite importer to two retained `DeUiImage` panels.
They use the same `aseprite_ui/panel` clip and generated `#atlas` at different aspect
ratios. The source has nine colored regions and a named `panel` slice whose authored
center is `[4, 4, 12, 12]` in a 20 x 20 image. No external artwork or Aseprite install is
needed: `generate.py` reproduces the CC0 diagnostic source byte for byte.

From an engine checkout:

```sh
cmake --preset macos-dev
cmake --build --preset macos-dev --target aseprite_ui
build/macos-dev/examples/aseprite_ui/aseprite_ui/aseprite_ui
```

On Windows use the `windows-clang-cl-dev` configure/build preset and the `.exe`
under that build directory. The example uses Metal on macOS and Vulkan elsewhere;
the corresponding backend and Dev cooker must be enabled. `DE_BUILD_EXAMPLES=ON`
is required. It is not a packed/Shipping sample.

Open `AsepriteUI.dproj` in a DIDE source build containing DIDE-117 shared sprite edits.
In the asset browser, open `aseprite_ui/panel` in its sprite viewer. Open a **Game**
view to see the retained panels. Set the 9-slice center to `[1, 1, 18, 18]`, then Apply:
the stretched bands change in both panels. **Back to the default** removes the local
override; Undo restores it. All changes belong to `assets.toml`; `panel.aseprite` is
never edited. Saving the source from an installed external Aseprite also uses the
engine's ordinary asset watcher.

`refresh_panels` runs in PostUpdate before `ui.layout`, which continues in editor
Edit mode. It copies current frame/slice metadata each frame, maps source coordinates
through the trimmed atlas rectangle, and updates UVs and source-pixel border widths.
No borrowed asset pointers are retained across reload. The opaque slice fits inside
the trim; handling transparent margins/outside slices requires clipping or an untrimmed
import. Atlas handles stay stable through import setting reloads.

The engine normally mounts this sample read-only. `--dev-dir <copied sample directory>`
selects writable copied assets, which the integration test uses. DIDE writes through
its normal scoped file/document service independently of the engine's mount flags.

## GPU evidence

The sample adds an explicit console command:

```text
aseprite_ui.capture dev:/before.rgba16f
```

It waits four completed render frames, copies the **actual main-view UI composite**
with public render-graph APIs, drains the hook before mapping its buffer, and writes
an atomic diagnostic file. It is independent of WebKit screenshots, which omit native
viewport layers. One capture may be pending; width and height are capped at 2048.
Only a requested capture allocates readback/copy storage (at most two 32 MiB buffers).
The command requires a writable virtual path such as `dev:/` from `--dev-dir`.

The file begins with three little-endian u32 values (width, height, row pitch), then
padded RGBA16F rows. Convert it to an SDR PNG for viewing:

```sh
python3 examples/aseprite_ui/view_capture.py before.rgba16f before.png
python3 examples/aseprite_ui/generate.py --check
```

The DIDE opt-in native test copies the source/manifest into a disposable project,
starts this real executable through the editor service, and exercises Apply, Reset
and Undo Reset. It compares RGB16F panel pixels exactly, ignores row padding/alpha,
asserts that no pixels outside the panels change, and verifies source-byte identity.
From the sibling DIDE checkout, after building its native test harness:

```sh
DIDE_ASEPRITE_UI_GAME=/absolute/path/to/engine/build/macos-dev/examples/aseprite_ui/aseprite_ui/aseprite_ui \
DIDE_ASEPRITE_UI_EXAMPLE=/absolute/path/to/engine/examples/aseprite_ui \
DIDE_E2E_BIN="$PWD/target/debug/dide" \
cargo test -p dide-e2e --test asset-viewers real_aseprite_apply_updates_retained_panel -- --nocapture --test-threads=1
```

Set `DIDE_VIEWER_CAPTURE_DIR` to retain the four raw captures. Both example environment
variables are required once either is set; leaving both unset skips this opt-in case.
The ordinary mock-backed model/sprite/font test remains available separately.

Verified on macOS Metal: Apply changed 48,834 panel pixels and zero outside pixels;
Reset and Undo Reset exactly restored their respective outputs. Both C sources pass
strict Windows C11 compile checks; Windows GPU execution is not yet qualified. This
qualifies DIDE-R164.AC1, not all model/font viewer or external-tool acceptance work.
ASan/UBSan-instrumented sample sources also complete the pixel workload, but the
sanitizer process stalls after normal host shutdown with leak detection enabled
(DENG-688). A complete process-exit leak check is not qualified; no leak suppression
or disabled leak detection was used. Transitive engine libraries were native in that run.

All source code and generated artwork in this example use the engine's license; the
diagnostic artwork in `panel.aseprite` is additionally dedicated to the public domain
under CC0-1.0.
