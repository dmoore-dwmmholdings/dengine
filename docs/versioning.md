# Versioning and ABI policy

DEngine uses Semantic Versioning. This page says what a version number promises, what counts as a
breaking change, and how the release-branch ABI check enforces it.

Sources: DENG-2 Q8 (the owner's answer: "SemVer; ABI frozen within a major version starting at
1.0; 0.x may break"), SPEC §5.1 (API conventions), SPEC §19.3 (ABI check on release branches) and
DENG-128 (this tool). Each rule below names its source. Rules marked **Proposed** are not yet
approved by the owner (decision on DENG-128); until they are approved, they are the working
default.

## Version numbers

`include/dengine/de_base.h` holds the version: `DE_VERSION_MAJOR`, `DE_VERSION_MINOR`,
`DE_VERSION_PATCH`. `de_version()` returns the version the library was built with, packed with
`DE_MAKE_VERSION`. Release tags are `vMAJOR.MINOR.PATCH`. (DENG-2 Q8)

| Change | Bump | Source |
| :- | :- | :- |
| Breaking API or ABI change (list below) | MAJOR | DENG-2 Q8 |
| New API, compatible with every earlier release of the same major | MINOR | DENG-2 Q8 |
| Bug fix with no API or ABI change | PATCH | DENG-2 Q8 |

## API and ABI

- **API** (source compatibility): a game that compiled against version X.Y still compiles
  against X.Z, for every Z > Y, with no source changes. The API is the declarations in the public
  headers, `include/dengine/*.h`. (DENG-2 Q8)
- **ABI** (binary compatibility): a game binary or a mod built against X.Y runs with the engine
  library X.Z, for every Z >= Y, with no rebuild. The ABI is plain C: exported symbols
  (`DE_API`), their calling signatures, the layout of every struct and union in a public header,
  enum values, and handle types. (SPEC §5.1)
- Within a major version from 1.0, both are frozen except for compatible additions.
  (DENG-2 Q8, SPEC §5.1)

## What counts as breaking

`tools/abi/abi_check.py` classifies every difference with these rules. (DENG-128)

Breaking:

- A `DE_API` function or exported variable is removed or renamed. A rename is a removal.
- A function's parameter types, return type, parameter count or variadic status changes.
- A public struct or union changes its size or alignment, or a field is removed, added, moved
  (offset), reordered or given a different type. This includes handle structs
  (`typedef struct { u32 id; } de_texture;`). (SPEC §5.1: "no struct passed by value across the
  ABI changes layout within a major version")
- An enum constant is removed, or its value changes. An enum's size changes.
- A public typedef is removed or its underlying type changes (handles, callback signatures).
- An opaque type is removed.
- `DE_GAME_API_VERSION` changes (see below).

Compatible:

- A new function, struct, union, enum, typedef or header.
- A new enum constant at the end of an enum. A constant inserted in the middle shifts the values
  after it, so the check reports that as breaking.
- An opaque type that gets a public definition.

Not checked by the tool, but still breaking: a change of behaviour that a correct caller can
observe (for example, a function that now returns an error for input it accepted before), and a
change to the meaning of a field or constant. Reviewers check these.

## 0.x releases

Major version 0 makes no API or ABI promise. Any 0.x release may break. (DENG-2 Q8)

**Proposed:** a breaking change in 0.x bumps the MINOR version, so `0.Y.Z -> 0.Y.(Z+1)` stays
compatible. The ABI check accepts breaking changes in 0.x only when the minor version increased,
and reports them with a notice.

## Desc structs

Every create function takes a `*_desc` struct, and `(de_x_desc){0}` is always valid: zero means
"use the default". (SPEC §5.1)

- The layout of a desc struct is frozen within a major version, the same as every other public
  struct. (SPEC §5.1, DENG-2 Q8)
- A new field goes into the next major version, or into a new struct `de_x_desc2` with a new
  function (`de_x_create2`) in a minor version. The old struct and function stay. (DENG-128)
- A new field must have a zero default that keeps the old behaviour. (SPEC §5.1)
- Do not add `reserved` padding fields to prepare for growth. Use `*_desc2`.

## Deprecation

1. Mark the declaration as deprecated in the header with a doc comment:
   `/** Deprecated since 1.4: use de_x_create2(). Removed in 2.0. */`. Also add the `DE_DEPRECATED(msg)`
   macro from `de_base.h`, before the declaration and after `DE_API`, so callers get a compiler
   warning that carries the message: `DE_API DE_DEPRECATED("use de_x_create2()") de_x de_x_create(void);`.
   The macro maps to the deprecation attribute on Clang, GCC and MSVC and expands to nothing on other
   compilers. (DENG-767)
2. Add the replacement in the same minor release. Write the change in the release notes.
3. The deprecated item stays, and keeps working, for the rest of the major version. It is removed
   only in the next major version. (DENG-2 Q8: the ABI is frozen within a major version)

## Support window

**Proposed:** when a new major version is released, the previous major version gets bug-fix and
security PATCH releases for 12 months on its `release/N.x` branch. After that it gets nothing.
Only the latest minor version of each supported major gets patches.

## DE_GAME_API_VERSION

`DE_GAME_API_VERSION` (`include/dengine/de_game.h`) is the version of the game API: the
`de_game_api` struct that a game module gives to the host. The host refuses a game whose
`api_version` is not `DE_GAME_API_VERSION`. (de_game.h)

- Bump it when the layout of `de_game_api` changes, when a callback's signature changes, or when
  the host calls a callback with different rules (order, threads, how often).
- A bump makes every game built before it fail to load, so it is a breaking change. Bump it only
  together with a new major version. The ABI check reports every change to it as breaking.
  (DENG-128)
- Do not bump it for engine changes that a game cannot see through `de_game_api`.

## The ABI check

`tools/abi/abi_dump.py` describes the public headers as JSON. It parses them with clang's JSON
AST. A generated probe (`tools/abi/layout_probe.c.in`) is compiled and run on the target, so the
struct sizes, alignments and field offsets are the real values for that platform.
`tools/abi/abi_check.py` compares two dumps. It exits with 1 on a breaking change, unless the
major version increased. (SPEC §19.3, DENG-128)

- Baselines: `abi/baseline/<platform>.json`, for `macos-arm64`, `linux-x86-64` and
  `windows-x86-64`. DENG-131 records the 1.0.0 baselines. Record a new baseline at each new major
  version.
- CI: `.github/workflows/abi-check.yml` runs on `release/**` branches and `v*` tags on all three
  platforms. If a baseline is missing, it shows a warning and uploads the dump.
- Every build compiles the probe (target `de_abi_probe`), including the `windows-xwin-dev` cross
  build. The tool tests are in `tests/unit/abi` (ctest labels `unit` and `check`).

### Running the check

```sh
python3 tools/abi/abi_dump.py --include-dir include --header dengine/dengine.h --out build/abi.json
python3 tools/abi/abi_check.py abi/baseline/macos-arm64.json build/abi.json
```

Python 3.10 or later, stdlib only, and clang on `PATH` (or `--clang`). You can only make a dump
for the platform that you run on, because the probe must run on that platform.
