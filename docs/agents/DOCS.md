# DEngine documentation rules (Trama space `DENG`)

The project documentation lives in the Trama space **`DENG`** ("DEngine"). The orchestrator and every worker read it for context and keep it up to date. This file says what goes there, who writes which page, and how.

Related files: `docs/agents/LOOP.md` (orchestrator), `docs/agents/WORKER.md` (worker), `docs/agents/CONVENTIONS.md` (code rules).

---

## 1. What lives where

| Kind of information | Where | Why |
|---|---|---|
| What to build | `SPEC.md`, DENG-2, the Andare issue | The plan. |
| What was built | the code in git | Git is the source of truth for done work. |
| How and why it works: architecture, module internals, design decisions, sprint reports, open verification items | **Trama space `DENG`** | Read by agents for context; linked to Andare issues. |
| API reference | Doxygen `/** */` comments in `include/dengine/*.h` | Versioned with the API. |
| Files that a checkout or a tool needs: `CLAUDE.md`, `README.md`, `CONTRIBUTING.md`, `docs/building.md`, `docs/ci.md`, `docs/agents/*`, `third_party/README.md`, tool `README.md` files | the repo | Versioned with the code they describe. |

- **Do not copy** between the repo and Trama. A Trama page links to a repo file (by path) instead of repeating its commands.
- An issue written before Trama was adopted may tell you to write `docs/modules/<mod>.md`, `docs/milestones/*.md` or another narrative page under `docs/`. Write the matching Trama page instead (§2), and say so in your report. Files in the repo row above stay in the repo.
- **Precedence.** `SPEC.md`, DENG-2 and the issue decide *what* to build. The code decides *what exists*. Trama explains it. If a page disagrees with the code, the code wins: fix the page if your issue owns it (§3), otherwise list it under **Follow-ups**.

## 2. Page tree

Titles are exact; `[[wikilinks]]` resolve against them, and Trama refuses a duplicate title.

```
DEngine                          home: one-paragraph vision, lead sprint, other open sprints and status, map of the space
├── Architecture                 layer bands, module graph, threading model, memory model, frame flow
├── Modules                      index table: module, source dir, layer, deterministic, status, page
│   └── Module: <module>         one per module, e.g. "Module: core", "Module: gpu/vulkan", "Module: world/vfx"
├── Build and Test               index
│   ├── Build System             de_add_module, options, build types, presets (links docs/building.md)
│   ├── Testing                  de_test.h, de_add_test, fuzz targets, golden + determinism harnesses
│   ├── Checks                   header_check, layering check (layers.toml), how to add an allow edge
│   ├── Vendoring                vendor.toml, vendor.py, how to add a library
│   └── CI                       workflows, matrix, what CI verifies (links docs/ci.md)
├── Tools                        index
│   └── Tool: <tool>             e.g. "Tool: de-cook", "Tool: de-pack", "Tool: caps", "Tool: editor"
├── Decisions                    how to read the log; embeds DENG-2
│   └── Decision: <topic>        one page per cross-cutting decision
├── Sprints                      index: sprint, dates, points, merge sha, report page
│   └── Sprint Sxx: <name>       the sprint report, e.g. "Sprint S01: Foundation"
├── Verification Backlog         every open needs-hw-verify item: issue, what to run, where
└── Glossary                     engine terms (handle, archetype, render world, tier, …)
```

`<module>` is the canonical module name from `tools/check_layers/layers.toml` (the source path under `src/`, or `cook` for `tools/cook/lib/`). Nested modules keep the slash.

A page that is too long (more than about 300 lines) gets child pages: `Module: render` → `render: Render Graph`, `render: GPU Scene`. A child page title is `<module>: <topic>`.

### Page header

Every `Module:`, `Tool:` and `Build and Test` child page starts with this header. Module pages then follow the sections of `docs/modules/_template.md`.

```
- **Status:** <planned | stub | in progress (Sxx) | implemented (Sxx) | partial (Sxx): <what is missing>>
- **Source:** `src/<module>/` · **Header:** `include/dengine/de_<x>.h` · **CMake target:** `de_<leaf>`
- **Last change:** <KEY> (Sxx)
```

### Decision pages

Write a `Decision: <topic>` page when a choice affects other modules or later sprints: an API shape, a file format, a naming rule, an ownership rule, or a deviation from the spec or the issue. A choice that only matters inside your files stays in your issue comment. Sections: **Context**, **Decision**, **Consequences**, **Issues** (keys). DENG-2 still holds the binding defaults; a decision page never overrides DENG-2.

## 3. Who writes what

| Page | Written by | When |
|---|---|---|
| `Module: <m>` and its child pages | the worker whose issue changes module `<m>` | in the issue, before `In Review` |
| `Tool: <t>`, `Build and Test` children | the worker whose issue changes that tool or build feature | same |
| `Decision: <topic>` (new pages) | any worker | same |
| `DEngine`, `Architecture`, `Modules`, `Build and Test`, `Tools`, `Sprints`, `Verification Backlog`, `Glossary`, `Sprint Sxx: …` | the orchestrator | sprint start and sprint close |
| A page outside your issue | nobody | Use `comment_on_page`, or list it under **Follow-ups**. |

A worker that changes the module graph, the threading model or another architecture-level fact writes it on its module page and lists it under **Follow-ups** as `Architecture: <change>`. The orchestrator folds it into `Architecture` at sprint close.

## 4. How to write a page

1. **Find it first.** `get_page_tree DENG` gives exact titles. A page you cannot find may still exist under another title: `search_pages` with the module or tool name before you create a page.
2. **Create** with `create_page` (`space: "DENG"`, `parent: "<parent title>"`).
3. **Update with read-modify-write in one step.** `update_page` replaces the whole body, and other workers may edit the same page.
   - Call `get_page` immediately before `update_page`. Do no other work between the two calls.
   - **Address the page by `id`** (from `get_page_tree`) in both calls. `get_page` by title can return the last *published* text instead of the current draft, and writing that back deletes other workers' unpublished changes.
   - Change only the sections about your work. Send back every other line unchanged.
   - Call `get_page` once more afterwards. If text from another agent is missing, put it back.
   - Every issue embed (`![[andare:KEY]]`) that was on the page before your write must still be there after it. A missing embed means you wrote over someone else's work: restore their sections from the page as you read it.
   - Write your page changes **after** your final rebase, as the last step before `In Review`, not early in the issue. A draft written hours before the merge is the one most likely to overwrite another worker's.
4. **Link the issue.** After your last update of a page, call `link_to_issue` (`ref: "<KEY>"`) on it once. `pages_referencing <KEY>` then shows that the issue is documented.
5. **Do not publish.** Workers leave their changes as drafts. The orchestrator publishes after a green merge, so the **published** version of a page always describes merged code.
6. **Style.** Short sections. Plain sentences. Link repo files by path in backticks, other pages with `[[Title]]`, issues with their key. Say what is missing or deferred, with the issue key that tracks it. Do not paste large code blocks; point at the file.

## 5. Reading the docs for context

Read before you plan; keep it to the pages that matter for the issue.

**Worker** (WORKER.md Step 1):
- `Architecture`.
- `Module: <m>` for every module your issue changes, and for each module your code calls.
- `pages_referencing` for your `KEY` and for the epic key.
- `search_pages` (space `DENG`) for the two or three main terms of the issue, and for `Decision:` pages about your modules.
- The `Sprint Sxx: …` page of the sprint that last changed your module, if your issue builds on it.

**Orchestrator** (LOOP.md):
- Sprint start: `DEngine`, `Architecture`, the previous `Sprint Sxx: …` page (its follow-ups), and `Verification Backlog`.
- Dispatching and merging: no extra reads. The worker reads the docs; the orchestrator only publishes.

## 6. Trama tool notes

- Every tool takes the space key `DENG`.
- `search_pages` matches whole words and is **not ranked**. Read several results.
- `update_page` **replaces** the body (see §4.3). `create_page` refuses a title that exists.
- **No empty table cells.** A Markdown table with an empty cell (`| |`) makes `create_page`/`update_page` fail with "Internal error". `create_page` then leaves an empty page behind. Put `–` in cells with no value. After an error, check with `get_page` whether the page exists before you retry, and retry with `update_page`.
- An empty result can mean "not visible to you". The `DENG` space is company-visible, so this should not happen; if it does, report it.
- Every write is recorded against the user whose token the connection uses. Write as a careful teammate would.
- `list_stubs DENG` lists `[[links]]` to pages nobody has written. Link a page that should exist even before it is written; the stub list is the docs to-do list.
