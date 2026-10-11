# Contributing to DEngine

DEngine is built by one developer and a set of AI agents (DENG-2 Q6). The work is planned in
the Andare project **DENG**. Each change belongs to one issue, which has a key such as
`DENG-42`. This file is the short version. The full rules are in
[`docs/agents/CONVENTIONS.md`](docs/agents/CONVENTIONS.md), and [`SPEC.md`](SPEC.md) wins
when the two disagree.

## Branches

- `main` holds only completed sprints.
- Each sprint has a branch `sprint/<sxx>-<slug>` (for example `sprint/s01-foundation`), cut
  from `main`.
- Each issue has a branch `feature/<KEY>-<slug>` (for example
  `feature/DENG-67-agent-docs-polish`), cut from its sprint branch. One issue per branch.
- Never force-push, and never rewrite history that other people use.

## Commits

- Subject: `<KEY>: <summary>`, for example `DENG-67: CLAUDE.md, CONTRIBUTING.md and per-module
  docs template`.
- Body: what changed, API changes, and the test count. End with `Refs: <KEY>, epic <EPIC>`.
- Agent commits also end with a `Co-Authored-By:` trailer.
- Do not commit build output (`build/`, `out/`) or SDKs that must not be vendored (see
  [Vendoring a library](#vendoring-a-library)).

## Building and testing

[`docs/building.md`](docs/building.md) has the full guide, with every preset and the cross
toolchains. The common commands:

```sh
cmake --preset dev                  # host preset
cmake --build --preset dev
ctest --preset dev                  # all tests
ctest --preset dev -L unit          # unit tests only
ctest --preset dev -L check         # header_check and layering_check
cmake --preset windows-xwin-dev && cmake --build --preset windows-xwin-dev  # Windows (xwin)
```

To add code to a module, add the sources in `src/<mod>/CMakeLists.txt`, which calls
`de_add_module()`. Add tests with `de_add_test()`: unit tests in `tests/unit/<mod>/`, fuzz
targets in `tests/fuzz/<mod>/`. Do not edit the root `CMakeLists.txt`; S01 registers every
module there in advance.

## Vendoring a library

Vendor only the libraries in SPEC §22, or ones added on DENG-2.

1. Add an entry to `third_party/vendor.toml`: `name`, `version`, `url`, `sha256`, `license`,
   and the `files` to keep.
2. Run `python3 tools/vendor/vendor.py fetch <name>`. The library lands in
   `third_party/<name>/`, with its `LICENSE`.
3. Run `python3 tools/vendor/vendor.py verify`. Commit the entry and the vendored files
   together.

Never vendor or download the Steamworks SDK, the Windows SDK or CRT, or any other proprietary
SDK. CMake finds those through a path that you set.

## Documentation

- Public header comments (`include/dengine/de_*.h`) are Doxygen-style `/** */`. They are the
  source of the API reference (DENG-2 Q7), so a public function without one is not done.
- A change to a module's public API also updates its page, `docs/modules/<mod>.md`. Start a new
  page from [`docs/modules/_template.md`](docs/modules/_template.md) and add it to the
  [module index](docs/modules/README.md).

## Review checklist

Check every item before you ask for review.

- [ ] C11 only. No VLAs, no `<threads.h>`, no `-ffast-math` (SPEC §3.2).
- [ ] Builds with no warnings: `-Wall -Wextra -Wpedantic -Werror`, or `/W4 /WX` on MSVC.
- [ ] Every subsystem takes an explicit `de_allocator *`. No hidden globals. No `malloc` or
      `free` in per-frame paths (SPEC §4.2, §5.3).
- [ ] Naming follows SPEC §5.1: `de_` for functions and types, `DE_` for macros and enum
      values, `De` + PascalCase for ECS components. Internal symbols are `static` or `de__`.
- [ ] Functions that can fail return `de_result` and give outputs through pointer parameters.
- [ ] Every create function takes a `*_desc` struct, and `(de_x_desc){0}` is a valid default.
- [ ] Public headers contain no SDL type or other third-party type, and compile as C11 and as
      C++17.
- [ ] In deterministic modules: `de_dmath` only, no `<math.h>` transcendentals, no iteration
      over hash maps, and a checksum test (SPEC §5.7).
- [ ] A module includes only modules below it (SPEC §4.1, `layering_check`).
- [ ] You changed only the paths that your issue owns. Shared paths are append-only.
- [ ] New code has tests, and the full `ctest --preset dev` passes. No test is disabled or
      weakened to get to green.

## License

DEngine is MIT-licensed (see [`LICENSE`](LICENSE)). By contributing, you agree that your
contribution is licensed under the same MIT license. Vendored code in `third_party/` keeps its
own license.
