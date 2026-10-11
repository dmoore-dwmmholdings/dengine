# DEngine worker instructions (one issue per worker)

The orchestrator (`docs/agents/LOOP.md`) gives you **one** Andare issue, a **worktree** and a **feature branch**. You take the issue from `In Progress` to `In Review`: you build it, test it, commit it and report on it. The orchestrator merges your branch and sets the issue to Done.

Your prompt gives: `KEY`, `Mode`, `Worktree`, `Feature branch`, `Sprint branch`, `Sprint epic`, and sometimes `Reason`.

**Work only inside your worktree.** Use absolute paths. Do not `cd` into the main repo or into another worktree. Do not check out other branches.

---

## Step 1: Read and claim
1. `get_issue <KEY>`. If it reports an unfinished blocker, stop and return `RESULT: failed` with the reason `blocked by <keys>`. Do not start.
2. Read the issue description in full. Then read:
   - the `## SPRINT PLAN (v2)` section of the epic description (`get_issue <EPIC>`),
   - **DENG-2** (decisions),
   - `docs/agents/CONVENTIONS.md` and `docs/agents/DOCS.md`,
   - the Trama pages that DOCS.md §5 lists for workers: `Architecture`, the `Module:` pages of the modules you change and call, the pages that reference your key or the epic, and the `Decision:` pages about your modules. They explain the code that earlier sprints merged into your base. If a page disagrees with the code, the code wins.
   - the SPEC.md sections and the `include/dengine/*.h` headers that the issue cites,
   - the existing code in the module directories the issue touches. Earlier sprints are merged into your base.
3. Set the status to **In Progress**: `set_issue_status <KEY> "In Progress"`. Use `update_issue` with `assignee: "this agent"` if the issue has no assignee.
4. **Comment where you are building.** Post this comment before you write any code:

```
🚧 **Started** (<mode>)
- Branch: `feature/<KEY>-<slug>` (from `sprint/<…>` @ <short sha of base>)
- Worktree: `.worktrees/<KEY>`
- Plan:
  - <3–8 bullets: files to create or change, approach, tests>
```

In **resume / fix / rebase / retry** mode, first read your earlier comments and `git log` on the branch. Then post a shorter start comment that says what you will do in this pass.

## Step 2: Build it
- Stay inside the issue's **Owns paths**. Change **Shared paths** only by appending (add lines; do not reorder or rewrite). If you must touch a file outside both lists, keep the change small and give the reason in your report.
- Follow `CONVENTIONS.md`: the C11 rules, explicit allocators, `de_` naming, handles, `de_result`, the determinism rules for modules marked deterministic, and no SDL types in public headers.
- If the header sketch is wrong or incomplete, amend it. Record each change under **API changes** in your report.
- Write the tests that the issue's **Test plan** lists. Put unit tests in `tests/unit/<module>/`, parser fuzz targets in `tests/fuzz/<module>/`, golden scenes in `tests/golden/`, and determinism scenarios in `tests/determinism/`.
- When the spec and the issue leave a choice open, make the choice that fits SPEC.md and DENG-2 best. Record it under **Decisions** in your report. Do not stop to ask, unless the work cannot be done at all.
- **Document as you build** (DOCS.md §3–§4). Update the `Module:` page of each module you change, the `Tool:` or `Build and Test` page of each tool or build feature you change, and create a `Decision:` page for each cross-cutting decision. Set the page header's `Status` and `Last change: <KEY> (Sxx)`. Write the pages from what your final code does, after Step 3 passes. Do not publish them; the orchestrator does after the merge. If an issue names a `docs/modules/…` or other narrative `docs/` file, write the Trama page instead.
- You may use subagents for research or reviews. Do not run two agents that edit files in this worktree at the same time.
- Do not commit while the work is half done, unless you need a checkpoint. If you make checkpoint commits, the **final** commit must still leave the branch green.

## Step 3: Verify
Run these in your worktree. The preset names come from `docs/building.md` once S01 exists. Before then, use `cmake -S . -B build/dev`.
1. Configure and build the host preset, with no warnings (`-Werror`).
2. `ctest` on the host preset. Every test must pass, not only yours.
3. The header-compile check and the layering check (S01 onward).
4. The cross-compile presets that apply to your code, if the host has their prerequisites (see the **Verification matrix** in CONVENTIONS.md).
5. The issue's acceptance criteria: check every box. If a criterion cannot be checked on this host, mark it `⏳ needs-hw` and say what has to run, and where.

Then rebase onto the latest sprint branch and verify again:
```
git fetch . sprint/<…>:sprint/<…> 2>/dev/null || true   # sprint branch is local; usually a no-op
git rebase sprint/<…>
<rebuild + ctest>
```
If the rebase has conflicts: resolve the trivial ones (append-only lists). Otherwise follow the fail path in Step 5.

## Step 4: Commit and report
1. **Commit at the end of the issue.** Use one final commit, or squash your checkpoints into a small number of meaningful commits:
```
<KEY>: <issue summary without the [Sxx] prefix>

<what was built, 2–6 lines>
- API changes: <none | list>
- Tests: <n added>, all <N> passing on <preset>

Refs: <KEY>, epic <EPIC>
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
```
2. Do not push. Do not merge. The orchestrator does both.
3. **Attach the completion comment** to the issue:

```
✅ **Ready for merge**: `feature/<KEY>-<slug>` @ <short sha>

**Summary**
<what exists now, 3–8 lines>

**Files**
- added: …
- changed: …

**API changes** (headers)
- <none | `de_x.h`: added/changed …>

**Tests**
- <list of test names> → <passed>/<total> on `<preset>`
- cross-compile: <preset → ok | skipped (missing prerequisite)>

**Acceptance criteria**
- [x] … 
- [x] …
- ⏳ needs-hw: … (what to run, where)

**Decisions**
- <choices made where spec/issue were open, with the reason; link `Decision:` pages by title>

**Docs** (Trama space `DENG`, drafts until merged)
- <page title> — <created | updated>: <what changed>

**Follow-ups**
- <work found that belongs elsewhere; the orchestrator will file it>
```
4. If any criterion is `⏳ needs-hw`, add the label `needs-hw-verify` with `update_issue`, keeping the existing labels.
5. Set the status to **In Review**: `set_issue_status <KEY> "In Review"`.

## Step 5: Return
End your final message with exactly one block:

```
RESULT: ready-for-merge
KEY: <KEY>
BRANCH: feature/<KEY>-<slug>
COMMIT: <full sha>
TESTS: <passed>/<total>
NEEDS_HW: <yes|no>
DOCS: <Trama page titles you created or updated, separated by " | ", or "none">
NOTES: <one line, or "none". If DOCS is none but you changed code, say why here.>
```

**Fail path.** Use it when you cannot finish: the work is impossible on this host and there is nothing useful to commit, a blocker is missing, or you hit an unresolvable conflict. Then:
1. Commit any useful partial work as `<KEY>: WIP: <what>`. It is not merged; it only keeps the work.
2. Comment `❌ **Could not complete**`, with the reason, what you tried, what is committed, and what a human or the next attempt needs.
3. Set the status to **To Do**.
4. Return:
```
RESULT: failed
KEY: <KEY>
BRANCH: feature/<KEY>-<slug>
REASON: <one line>
```

## Never
- Change other issues' statuses. You may comment on related issues.
- Edit files outside your worktree, or check out or modify `main` or the sprint branch.
- Push, force-push, or rewrite history that you did not create in this issue.
- Install licensed SDKs (Windows SDK, Steamworks), or accept licenses for the user.
- Publish a Trama page, or edit a page your issue does not own (DOCS.md §3). Comment on it instead.
- Weaken a test, or skip a check, to get to green. If a check is wrong, fix the check and say so in the report.
