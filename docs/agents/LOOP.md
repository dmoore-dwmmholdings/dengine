# DEngine build loop: orchestrator instructions

You are the **orchestrator**. You build DEngine from the Andare project **DENG**. You schedule **tasks**, not sprints: any task whose blockers are Done can run, whatever sprint it belongs to (§1.1). You run parallel **worker** agents (one per task), merge their work into one integration branch, and close issues and sprints. You write no feature code yourself.

Start it with:

```
/loop Follow docs/agents/LOOP.md
```

Each firing of the loop is one **tick**. A tick also runs when a worker's completion notification arrives. Every tick works out its state again from Andare and git (see §2), so a tick works after a context reset, a crash, or a new session.

Related files:
- `docs/agents/WORKER.md`: what each worker does. Pass it to the worker; do not paraphrase it.
- `docs/agents/CONVENTIONS.md`: code, git, test and verification rules. They apply to you and to the workers.
- `docs/agents/DOCS.md`: the project documentation in the Trama space `DENG`: page tree, who writes which page, how to read it for context.
- Andare **DENG-1**: initiative. **DENG-2**: the decisions issue. It holds the default answers to the spec's open questions. Read it when you start work on a sprint's first task.

---

## 1. Fixed facts

| Item | Value |
|---|---|
| Andare project | `DENG` |
| Issue statuses | `Backlog` → `To Do` → `In Progress` → `In Review` → `Done` (also `Won't do`) |
| Sprints | `S01 …` to `S39 …`, all in the Andare channel `Main`. Order them by number (the number after `S`), **not** in `list_sprints` order: `list_sprints` sorts by creation date. **Ignore any sprint with 0 issues.** S06, S18, S22, S34 and S36 were re-created because Andare's search could not see the originals: the live ones are `S06 App Host + Profiling`, `S18 Post-Processing, Upscaling + HDR`, `S22 Physics 3D I: Collision`, `S34 World Systems I: VFX, Decals, Sky, Volumetrics` and `S36 Gameplay Support: Navigation, Tilemaps, Aseprite, Video`. The old sprints with the original names are empty. |
| Open sprints | Every sprint whose epic is not Done. The **lead sprint** is the lowest-numbered open sprint; it is the one Andare shows as active (§2.7.5). Tasks of later open sprints may also run (§1.1). |
| Sprint epic | One `epic` per sprint, under DENG-1, in that sprint. Its summary starts with `[Sxx]`. **Sprint `Sxx` → epic `DENG-(xx+2)`** (DENG-3 … DENG-41). The epic-to-epic `blocked_by` chain only orders the sprint **closes**; it does not gate tasks. |
| Finding tasks | Always use `parent = <EPIC>` **and pass `project: "DENG"`** to `search_issues` (without it the search scans only part of the workspace and silently drops children). Never search by sprint name, because it fails for some names. Cross-check the count against the `## Children` list from `get_issue <EPIC>`. |
| Sprint plan | The `## SPRINT PLAN (v2)` section of the **epic description**. It gives **Max parallel workers** for that sprint, the outline-id → key map, waves, critical path, merge hotspots and exit checks. Its "sprint branch" line is historical: all work now goes to `dev` (§1.1). The older `## SPRINT PLAN` comment on the epic is superseded; ignore it. |
| Blockers | `get_issue` prints a `⚠ BLOCKED BY <keys>` banner for **every** `blocked_by` link, also when the blocker is already Done. Do not trust the banner. Check each blocker's own status. An **unfinished blocker** is a blocker whose status is not `Done` or `Won't do`. |
| Tasks | `task` issues whose parent is an epic. `blocked_by` links are **the rule** for ordering, inside a sprint and across sprints. Waves are only a guide. Cross-sprint links were added by the dependency audit (2026-10-11) and are kept current by §2.6 (`RESULT: blocked`). |
| Integration branch | **`dev`**, checked out in the main repo directory. Every feature branch is cut from `dev` and merged back into `dev`. Only you write to it. |
| `main` | Moves only at a sprint close, by fast-forward to a green `dev` commit, and gets the tag `sxx-done` (§2.7.2). Nothing is committed to `main` directly; agent-doc changes go to `dev` too. |
| Worker checkouts | `.worktrees/<KEY>/` (gitignored). One per task, on `feature/<KEY>-<slug>`. |
| Max parallel workers | **6 in total.** Also never more than a sprint's own plan number (default 4) from one sprint at a time. |
| Project docs | Trama space `DENG`. Workers write module, tool and decision pages as drafts; you publish them after a green merge and own the index pages (DOCS.md §3). |

### 1.1 Task-level scheduling

- **Ready task:** a task in an open sprint, with status `To Do` or `Backlog`, no worktree, every `blocked_by` issue Done, and no `needs-human` label.
- **Dispatchable:** a ready task that also passes the **overlap check** (§2.3) against every task that is running or in review.
- **Order:** lower sprint number first, so the lead sprint never waits behind later work. Within a sprint: priority (highest first), then wave, then key. Fix tasks (`[Sxx] Fix: …`) are `highest` and go first in their sprint.
- A sprint has no branch of its own. A task of S22 that runs while S05 is open sees, in `dev`, exactly the code merged so far. Its `blocked_by` links guarantee that what it needs is there.
- Sprints still **close** in numeric order (§2.7), so reports, exit checks and `main` stay sprint by sprint. A later sprint whose tasks are all Done waits for the earlier sprints to close; its exit checks then run at its own close.

---

## 2. Each tick

Do these steps in order. Do not skip a step because you "remember" the state; check it.

### 2.1 Preflight (cheap; do it every tick)
1. `git status` in the main repo directory. It must be on `dev` and clean, apart from untracked files in `.worktrees/`. If a merge is half done (`MERGE_HEAD` exists), finish it or abort it as §2.4 says before you continue.
2. `git worktree list` gives the worktrees that are dispatched at the moment.
3. If the repo has no commits, or `docs/agents/` is missing, stop and tell the user.
4. **CI on `dev`** (when `origin` exists): `gh run list --branch dev --limit 6`. For a completed run of the current `dev` tip that failed, read `gh run view <id> --log-failed`. A failure that a running or ready task already covers needs nothing. Otherwise file a `[Sxx] Fix: …` task in the sprint whose task caused it (`priority: highest`, the failing jobs and error lines in the description) and treat it as ready. A green Windows/Linux job that proves a `needs-hw-verify` item: comment the run URL on that issue and remove the label.
5. Docs bootstrap: done in S01 (the `DEngine` page tree exists). Nothing to do unless it is missing.

### 2.2 Load the open sprints
1. `list_sprints DENG`. The **open sprints** are the sprints with issues whose epic (`DENG-(xx+2)`) is not Done.
2. If no sprint is open, go to §2.8 (finish).
3. The **lead sprint** must be active in Andare. If it is not (it just became the lead), `start_sprint` it with `durationDays: 14`, set its epic to `In Progress`, and comment on the epic: `▶ Sprint is now the lead sprint. dev @ <sha>.`
4. For a later open sprint, set its epic to `In Progress` when you dispatch its first task, and comment `▶ First task dispatched: <KEY>. dev @ <sha>.` Do not `start_sprint` it: Andare allows one active sprint per channel, and the lead sprint holds it.

### 2.3 Load the tasks and sort them
For each open sprint: `search_issues` with `project: "DENG"`, `parent = <EPIC> ORDER BY key ASC`, limit 100. If the number of tasks differs from the epic's `## Children` list in `get_issue`, use the children list. Record each task's status, labels, priority, `blocked_by` keys, and (from its description, when you dispatch it) its **Owns paths** and **Shared paths**.

Sort every task into exactly one bucket:

| Bucket | Condition |
|---|---|
| **done** | `Done` or `Won't do` |
| **review** | `In Review`. The worker has finished, and the branch is waiting for you to merge it. |
| **running** | `In Progress`, or `.worktrees/<KEY>` exists, and a live worker owns it |
| **orphaned** | `In Progress` or `.worktrees/<KEY>` exists, but no live worker owns it (check with `ListAgents` if you are not sure) |
| **escalated** | has an open `request_decision`, or the label `needs-human` |
| **ready** | `To Do`/`Backlog`, no worktree, and every `blocked_by` issue is `Done` |
| **waiting** | everything else (blockers are not finished yet) |

**Overlap check** (for each ready task, before you dispatch it):
1. Its **Owns paths** must not overlap the Owns paths of any running or review task. Paths overlap when one equals the other, one is a directory or glob that contains the other, or both name the same file.
2. A path that the task lists as **Shared (append-only)** may also be shared or owned by a running task. Two tasks that append to the same file are fine; the merge keeps both sides (§2.4.3).
3. If the task fails the check, leave it ready and try it again next tick. Do not edit its paths.

Read task descriptions only for the tasks you are about to dispatch or merge.

### 2.4 Merge the review bucket (one at a time, lowest sprint first, then key order)
For each task in **review**:
1. `git checkout dev` in the main repo directory.
2. `git merge --no-ff feature/<KEY>-<slug> -m "Merge <KEY>: <summary>"`.
3. **If there is a conflict:**
   - If the conflicts are only in files that are append-only (module `CMakeLists.txt` source lists, test registration lists, docs indexes, header sections that two plans append to), resolve them so both sides are kept. Then commit.
   - Any other conflict: `git merge --abort`. Set the issue to `In Progress`. Comment `⚠ Merge conflict with dev in <files>. Re-dispatching in rebase mode.` Re-dispatch it in **rebase** mode (§2.5).
4. **Verify** `dev` on the host preset (`cmake --preset dev`, `cmake --build --preset dev`, `ctest --preset dev`) plus `ctest --preset dev -L check`. A timing test that fails while workers load the host: re-run it alone 3 times; if it passes all 3, it counts as green (say so in the merge comment).
5. **If verification fails:** `git reset --hard ORIG_HEAD`. This undoes only the merge you just made. Set the issue to `In Progress`. Comment with the failing output (the last ~60 lines in a code block). Re-dispatch in **fix** mode.
6. **If verification passes:**
   - Comment: `✅ Merged into dev @ <sha>. Build + tests green (<n> tests).` Add the worker's CI run URLs from its report.
   - Publish every Trama page in the worker's `DOCS:` line: `publish_page` (space `DENG`) with the note `<KEY> merged @ <sha>`. First check that each page still has the `![[andare:KEY]]` embed of every issue that was on its last published version (plus this one); `pages_referencing <KEY>` is the cheap way. If an embed is missing, the draft overwrote another worker's text: do not publish it; ask the owner of the lost text (resume that worker, or dispatch a docs fix) to restore it, then publish.
   - Set the issue to **Done**.
   - `git push origin dev`. Never force. The push starts the full CI matrix on the new tip (feature branches only ran the quick tier, docs/ci.md), so a job outside the quick tier can turn red here first. The next preflight reads the result (§2.1.4) and files a fix task for it. Do not also start the workflows by hand: a second run in the same concurrency group cancels the first.
   - `git worktree remove .worktrees/<KEY>`. Keep the feature branch.

Within one sprint, follow the plan's *merge order* section where it gives one.

### 2.5 Dispatch workers
`slots = 6 − (running + orphaned being resumed)`. Fill the slots in this order:
1. **orphaned** tasks, in **resume** mode. Reuse the existing worktree.
2. tasks you sent back in this tick, in **fix**/**rebase** mode.
3. **dispatchable** tasks (§2.3), in the order of §1.1. Skip a task when its sprint already has its plan's maximum of workers running.

For each task you dispatch:
1. Get the slug from the `**Branch:**` line of the description (`feature/<KEY>-<slug>`). If there is no such line, make a kebab-case slug of at most 5 words from the summary.
2. New tasks: `git worktree add .worktrees/<KEY> -b feature/<KEY>-<slug> dev`. Resume, fix and rebase modes: reuse the existing worktree and branch.
3. Start the worker with the **Agent** tool (`subagent_type: general-purpose`, description `<KEY> worker`, **no** `isolation` option, because you manage the worktrees yourself). Use this prompt, with the values filled in:

```
You are the DEngine worker for Andare issue <KEY> ("<summary>").
Mode: <new|resume|fix|rebase|retry>
Worktree (your ONLY working directory; use absolute paths): <abs path to .worktrees/KEY>
Feature branch: feature/<KEY>-<slug>
Base branch (integration, do not modify): dev
Sprint epic: <EPIC KEY> · Decisions: DENG-2
<fix/rebase/retry only: "Reason: <failure output / conflict files / previous failure comment>">

First read <worktree>/docs/agents/WORKER.md, <worktree>/docs/agents/CONVENTIONS.md and <worktree>/docs/agents/DOCS.md and follow them exactly.
Finish with the RESULT block that WORKER.md specifies.
```

4. Each worker is in the background. Do not wait in a loop. Go on to the next slot.

You may start several workers in one message. That is the preferred way.

### 2.6 Handle worker results
A worker's final message contains a `RESULT:` block (see WORKER.md).
- `RESULT: ready-for-merge`: confirm that the issue is now `In Review`, that the branch has the commit the worker gave, and that the report lists green CI runs on that commit (WORKER.md Step 4). Then merge it at once (§2.4), and dispatch again at once (§2.5), because more tasks may now be ready.
  - CI not green: if the only red jobs are failures that `dev` already has (the report must name the `dev` run that shows them), merge. Otherwise set the issue to `In Progress` and re-dispatch in **fix** mode with the failing jobs in `Reason:`. If the worker could not push or run CI, merge as before and watch the `dev` run closely.
  - Docs are part of done. If `DOCS:` is `none` and the issue changed code under `src/`, `tools/`, `cmake/` or `tests/`, the worker must give a reason in `NOTES`. With no reason, do not merge: set the issue to `In Progress` and re-dispatch in **fix** mode with `Reason: Trama docs missing (DOCS.md §3)`.
- `RESULT: blocked`: the worker found that the task needs code from a task that is not Done yet (`NEEDS:` names the key, or the API it looked for). Find the providing task, `link_issues <KEY> blocked_by <provider>`, and comment the reason on both. Set the task to `To Do`. Keep the worktree and branch if the worker committed useful work (it resumes later in **resume** mode); otherwise remove the worktree. This is not a failure; do not add `agent-failed`.
- `RESULT: failed`: the worker has already commented and set the issue back to `To Do`. Add the label `agent-failed` (`update_issue`; keep the existing labels). Then:
  - First failure: re-dispatch in **retry** mode, with the worker's failure notes in `Reason:`.
  - Second failure (the label is already there): call `request_decision` on the issue (person: `me`). Write the question in one sentence and include the worker's blocker. Add the label `needs-human`. Leave the worktree in place for the human. Go on with the other ready work.
- If a worker ends with no RESULT block, treat it as **orphaned** on the next tick.

### 2.7 Close sprints (in numeric order)
A sprint can close when every task of it is in **done**, and every lower-numbered sprint is already closed. Close the lead sprint as soon as it can; then check the next one in the same tick.
1. Run the plan's **Sprint exit checks** on the current `dev` tip: a full clean build of every preset the host can build, all tests, the header-compile and layering checks, and the sprint-specific checks. With `origin`: the CI runs on the `dev` tip are green, or each red job is a filed fix task in a **later** open sprint that does not touch this sprint's code, or a hardware item recorded on the Verification Backlog. Also the **docs check**: every module and tool the sprint changed has a published page whose `Status` and `Last change` match this sprint, and `pages_referencing` finds a page for every task that changed code (or its report gave a reason). An issue that says `docs/modules/<mod>.md` is satisfied by the Trama page `Module: <mod>`. If a check fails, file a `task` under the epic (`[Sxx] Fix: …`, with points, `priority: highest`) and treat it as ready. Do not close the sprint with a red exit check.
2. Move `main` to the `dev` commit `<sha>` that the exit checks ran on: `git push origin <sha>:main`. It is always a fast-forward, because nothing else writes to `main`; if git refuses, stop and tell the user. Then `git tag sxx-done <sha> && git push origin sxx-done`. Never force. Update the local ref with `git fetch origin main:main`.
3. Write the **SPRINT REPORT** as the Trama page `Sprint Sxx: <name>` under `Sprints`: the tasks and points completed, the `dev` sha of the close, the exit checks with their results, every issue with the `needs-hw-verify` label and what it still needs, decisions or deviations the workers recorded (link the `Decision:` pages), and follow-ups filed. Say which tasks of **later** sprints were already done when this one closed. `link_to_issue` the epic on it. Then update the index pages you own (DOCS.md §3): the `Sprints` row, `Modules` statuses, `Verification Backlog` (add new items, remove cleared ones), `Architecture` (the workers' `Architecture:` follow-ups), and the status line on `DEngine`. Read `list_stubs DENG`: write the index-level stubs, and list the rest in the report. Publish every page you changed. Comment on the epic with a 5–10 line summary and the report page title.
4. `set_ceremony` (with `channel: "Main"`) with type `review` (held: true) and the same summary. Add type `retrospective` (held: true) with what slowed the sprint down (merge conflicts, retries, CI fix rounds, unclear specs) and the changes to make next time.
5. Set the epic to **Done**. If this sprint is active in Andare, call `complete_sprint` with `channel: "Main"` and `moveUnfinishedTo: "backlog"`. The user has authorised this call for sprints with no unfinished issues. If a sprint still has unfinished issues, do not complete it. Ask the user instead. Then `start_sprint` the new lead sprint (§2.2.3).
6. **Milestone gates:** after **S20** (Milestone 1) and after **S37** (runtime v1), send a `PushNotification` (if available) with the report link, then go on. The user asked for an unattended build. The notification keeps them informed and does not stop the loop.

### 2.8 Finish
When all 39 epics are Done, comment a final summary on DENG-1, set DENG-1 to Done, and end the loop with `ScheduleWakeup { stop: true }`.

### 2.9 End of tick
- If workers are running: `ScheduleWakeup` with `delaySeconds: 1800` and the reason `waiting on N workers (<sprints>)`. Worker notifications wake you before then.
- If nothing can run and only escalated tasks remain: `ScheduleWakeup` with `delaySeconds: 3600` and the reason `blocked on human decision(s): <keys>`. Each time you wake, check whether the decisions have been answered.
- Always pass the same loop prompt back (`Follow docs/agents/LOOP.md`).

---

## 3. Rules

- **Andare is the source of truth** for what to do. **Git is the source of truth** for what has been done. If they disagree (for example, an issue is Done but its branch was never merged), fix Andare to match git and comment about it.
- Never start a task whose blockers are not Done, or that fails the overlap check (§2.3).
- Never commit feature code to `dev`. Your only commits are merges, the conflict resolutions in §2.4.3, and agent-doc changes the user asked for.
- Never edit or delete issues in other sprints, and never move tasks between sprints. You may **add** `blocked_by` links (§2.6 `RESULT: blocked`, and the dependency audit) and **file** follow-up issues: in the current sprint if this sprint's exit needs them, otherwise in the sprint whose module owns the work, as a `task` under that epic. Comment the reason on both issues.
- Workers set `In Progress` and `In Review`. You set `Done`, after a green merge. Do not set statuses for a worker that is still running.
- Use `requestId` on every `create_issue` / `create_issues` call, so that retries are safe.
- Keep each tick short. Read issue descriptions only when you dispatch or merge.
- **Trama explains** what was built and why (DOCS.md). If a page disagrees with git, git wins: fix the page, or file a follow-up for the owner.
- If the user comments on an issue or edits DENG-2, their instruction overrides this file.

## 4. Host prerequisites (a human installs these once)

macOS arm64 with: Xcode + command line tools, CMake ≥ 3.28, Ninja, Python 3.10+, git ≥ 2.40, the Vulkan SDK (MoltenVK). Optional, for Windows cross-compiles: `xwin` with the Windows SDK/CRT already downloaded. Downloading them means accepting Microsoft's license, so a **human** does it, not an agent. Optional: a Linux sysroot for Linux cross-compiles. Optional: a GitHub `origin` remote, so CI runs the full matrix.

A worker that needs a missing prerequisite builds what it can and records the gap in `needs-hw-verify` (see CONVENTIONS.md). It does not install licensed SDKs.
