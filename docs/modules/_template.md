# `<module>` module

<!--
The section layout of the Trama page "Module: <module>" (space DENG; see docs/agents/DOCS.md).
Start the page with the DOCS.md §2 page header, then these sections.
Any task that changes this module updates its page before In Review; the orchestrator publishes it
after the merge. Keep each section short. Write "None." when a section does not apply.
-->

- **Source:** `src/<module>/`
- **Header:** `include/dengine/de_<module>.h`
- **Layer:** <SPEC §4.1 layer, as listed on the Trama `Modules` page> · **CMake band:**
  <foundation | runtime | content | gameplay | host> (`de_add_module(... LAYER)`)
- **CMake target:** `de_<module>` · **Option:** <`DE_WITH_<MOD>` | always built>
- **Deterministic:** <yes (`de_add_module(... DETERMINISTIC)`) | no>
- **Spec:** §N (the sections this module implements)

## Purpose

<!-- Two to four sentences: what the module does, and what it does not do. -->

## Public API

<!--
The header and the functions a caller needs first. The Doxygen comments in the header are the
reference (DENG-2 Q7); this section is a map, not a copy.
-->

| Function or type | What it does |
| --- | --- |
| `de_<module>_create(const de_<module>_desc *, de_allocator *, ...)` | |
| `de_<module>_destroy(...)` | |

## Internal structure

<!-- The main files in src/<module>/ and how the parts fit together. Name third-party code
here, and the C interface that wraps it (DENG-2 Q1). -->

## Threading

<!-- Which functions are thread-safe. Which thread owns what. Task-graph usage. -->

## Memory and allocators

<!-- The allocator the create function takes, the arenas and pools used inside, and which paths
run each frame (these must not call malloc/free; SPEC §5.3). -->

## Determinism

<!-- Only for deterministic modules (SPEC §5.7): de_dmath usage, stable iteration order, the
checksum function and its test under tests/determinism/. Otherwise: "Not deterministic." -->

## Cvars

<!-- Every de_cvar the module registers: name, type, default, flags (SPEC §5.8). -->

| Name | Type | Default | Flags | Meaning |
| --- | --- | --- | --- | --- |

## Tests

<!-- Unit tests (tests/unit/<module>/), fuzz targets (tests/fuzz/<module>/), golden scenes,
determinism scenarios, and how to run only this module's tests. -->

## Known gaps

<!-- What is missing, stubbed or deferred, with the Andare key that tracks it. -->
