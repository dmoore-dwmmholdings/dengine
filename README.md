# DEngine

DEngine is an open-source, C11 game engine for desktop platforms. It renders 2D and 3D in one
world with a GPU-driven, physically based pipeline, adds hardware raytracing where the GPU
supports it, and falls back to a raster pipeline where it does not.

**Status:** pre-alpha, M1 in progress. Nothing here is usable yet.

## Platforms

| Platform | Arch | Graphics | Window / input | Audio backend |
|---|---|---|---|---|
| Windows 10 22H2+ / 11 | x86-64 | D3D12 (Agility SDK) | SDL3 | WASAPI |
| Linux (glibc 2.31+), SteamOS | x86-64 | Vulkan 1.3 | SDL3 (Wayland + X11) | PipeWire, ALSA fallback |
| macOS 14+ | arm64 | Metal 3 | SDL3 | Core Audio (AudioUnit) |
| Headless server | x86-64 Linux, Windows | none | none | none |

## Documentation

- [`SPEC.md`](SPEC.md): the technical specification.
- [`docs/building.md`](docs/building.md): how to build and test.
- [`CONTRIBUTING.md`](CONTRIBUTING.md): how to contribute.
- [`docs/`](docs/README.md): all other documentation.

## License

MIT, see [`LICENSE`](LICENSE). Vendored dependencies in `third_party/` keep their own licenses.
