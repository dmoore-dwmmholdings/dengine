# hello_triangle

The first pixels of DEngine. The example draws one of two scenes on each GPU backend. It uses the
public `de_gpu` API (`include/dengine/de_gpu.h`), the platform layer for the window and the asset
system for the shaders and the texture.

- **`triangle`** (the default): the vertex shader reads the triangle's vertices from a storage
  buffer through its bindless index. The buffer is GPU-only and is filled by a copy from an UPLOAD
  buffer: shaders read bindless buffers as read-write structured buffers, which on D3D12 is a UAV,
  and UPLOAD memory cannot have one. The pixel shader takes its colour from the push constants.
- **`quad`**: the example decodes a 64x64 checker texture (`textures/checker.png`) and copies it
  to the GPU through an UPLOAD buffer, `de_gpu_cmd_copy_buffer_to_texture` and barriers. A quad
  then samples it through bindless texture and sampler indices that come from the push constants.
  The top-left cell is red, so the screenshot shows which way up the texture is.

The window has a swapchain and keeps 2 frames in flight on one timeline fence. It handles resizes,
and the example quits when you close the window or after `--frames N` frames.

## Build and run

`hello_triangle` builds with the examples (`DE_BUILD_EXAMPLES`, on by default except in Shipping).
See `docs/building.md` for the presets.

```sh
cmake --preset dev && cmake --build --preset dev --target hello_triangle
build/dev/examples/hello_triangle/hello_triangle                      # platform default backend
build/dev/examples/hello_triangle/hello_triangle --backend metal --scene quad
build/dev/examples/hello_triangle/hello_triangle --backend vulkan --frames 120
build/dev/examples/hello_triangle/hello_triangle --backend d3d12 --offscreen --screenshot out.png
```

| Option | Meaning |
|---|---|
| `--backend metal\|vulkan\|d3d12\|default` | The RHI backend. `default` is D3D12 on Windows, Metal on macOS and Vulkan on Linux. |
| `--scene triangle\|quad` | The scene to draw. |
| `--frames N` | Quit after N frames. The default is to run until the window closes, or 3 frames with `--screenshot`. |
| `--adapter N` | The N-th adapter (`de_gpu_device_desc.adapter`). The default is the best adapter that meets the Base tier. |
| `--offscreen` | Draw into a 256x256 RGBA8 texture, with no window or swapchain. Use it on machines with no display, such as CI runners. |
| `--screenshot out.png` | Read back the image of the last frame and write it as a PNG (`stb_image_write`). |
| `--check` | With `--offscreen --screenshot`: compare 4 pixels with the expected colours. Exit 1 if one differs. |
| `--pack file.dpak` | Load the assets from a pack (PACKED mode), not from the sources. |
| `--assets dir` | The directory that has `assets.toml`. The default is this source directory. |
| `--cache-dir dir` | Put the cook cache and the pipeline cache in `dir`. The default is the app's `cache:/` directory. |

Exit status: 0 when the run succeeds. 1 when something fails: an error was logged (validation
errors are logged as errors) or a `--check` pixel differs. 2 for a bad argument. 77 when this
machine cannot run the request: no device for the backend, a device below the Base tier (the
example refuses it, as the renderer does), no display, or shaders that were not cooked for the
backend.

## Expected output

- `triangle`: a yellow triangle (1.0, 0.8, 0.2) on a blue-grey background (0.2, 0.4, 0.6).
- `quad`: an 8x8 orange and dark-blue checkerboard with a red top-left cell. It covers the middle
  three quarters of the window.

On the console, the example prints the scene, the backend and the device, then
`hello_triangle: done`. With `--check`, it also prints the 4 pixels it compares.

## Shaders and assets

`assets.toml` lists `shaders/triangle.slang` and `shaders/quad.slang` as `shader` assets, and the
checker PNG as a `blob` asset. There is no texture importer until S11, so the example decodes the
PNG with `stb_image`. In Debug and Dev builds the asset system cooks the shaders in-process when
they first load (SOURCE mode), for the targets the host can build: metallib and SPIR-V on macOS,
DXIL and SPIR-V on Windows, SPIR-V on Linux. The cook cache keeps the results. Release builds have
no cooker, so they need `--pack`.

`shader_glue.c` is the only file that uses engine internals. The engine has no public API yet that
registers the `shader` asset type or gives the code of a cooked shader to `de_gpu_pipeline_create`
(see DENG-409). `main.c` includes only public headers. `tools/check_layers` checks this
(`layering_check`).

## Tests

CTest runs each scene on each backend that the build has: `example_hello_triangle_<backend>` and
`example_hello_quad_<backend>`. Each test runs offscreen for 3 frames, with `--screenshot` and
`--check`. A test is skipped (exit 77) when the machine has no Base-tier device for the backend
(the macOS CI runners have a paravirtual Metal device without Metal 3). On Windows,
the D3D12 tests use a copy of the example that has the vendored WARP next to it
(`third_party/d3d12_warp`, test-only), so they also run on CI machines that have no GPU.

```sh
ctest --preset dev -R example_hello
```
