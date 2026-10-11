# DEngine build loop: orchestrator instructions

You are the **orchestrator**. You build DEngine sprint by sprint from the Andare project **DENG**. You run **two sprints in parallel**, one in each **lane** (§1.1). You run parallel **worker** agents (one per issue), merge their work, and close issues and sprints. You write no feature code yourself.

Start it with:

```
/loop Follow docs/agents/LOOP.md
```

Each firing of the loop is one **tick**. A tick also runs when a worker's completion notification arrives. A tick covers both lanes. Every tick works out its state again from Andare and git (see §2), so a tick works after a context reset, a crash, or a new session.

Related files:
- `docs/agents/WORKER.md`: what each worker does. Pass it to the worker; do not paraphrase it.
- `docs/agents/CONVENTIONS.md`: code, git, test and verification rules. They apply to you and to the workers.
- `docs/agents/DOCS.md`: the project documentation in the Trama space `DENG`: page tree, who writes which page, how to read it for context.
- Andare **DENG-1**: initiative. **DENG-2**: the decisions issue. It holds the default answers to the spec's open questions. Read it at the start of every sprint.

---

## 1. Fixed facts

| Item | Value |
|---|---|
| Andare project | `DENG` |
| Issue statuses | `Backlog` → `To Do` → `In Progress` → `In Review` → `Done` (also `Won't do`) |
| Sprint names | `S01 …` to `S39 …`. Start them in numeric order (the number after `S`), **not** in `list_sprints` order: `list_sprints` sorts by creation date. One sprint is active in each lane, so two at most (§1.1). **Ignore any sprint with 0 issues.** A sprint that moved to the `Alternate Development` channel leaves an empty sprint with the same name in `Main` (§2.2.2). S06, S18, S22, S34 and S36 were re-created because Andare's search could not see the originals: the live ones are `S06 App Host + Profiling`, `S18 Post-Processing, Upscaling + HDR`, `S22 Physics 3D I: Collision`, `S34 World Systems I: VFX, Decals, Sky, Volumetrics` and `S36 Gameplay Support: Navigation, Tilemaps, Aseprite, Video`. The old sprints with the original names are empty. |
| Sprint epic | One `epic` per sprint, under DENG-1, in that sprint. Its summary starts with `[Sxx]`. **Sprint `Sxx` → epic `DENG-(xx+2)`** (DENG-3 … DENG-41). By default, epic N is `blocked_by` epic N-1. The parallel check (§2.2.3) can replace that link with links to the earlier epics. |
| Finding tasks | Always use `parent = <EPIC>` **and pass `project: "DENG"`** to `search_issues` (without it the search scans only part of the workspace and silently drops children). Never search by sprint name, because it fails for some names. Cross-check the count against the `## Children` list from `get_issue <EPIC>`. |
| Sprint plan | The `## SPRINT PLAN (v2)` section of the **epic description**. It gives the sprint branch, **Max parallel workers**, the outline-id → key map, waves, critical path, merge hotspots and exit checks. The older `## SPRINT PLAN` comment on the epic is superseded; ignore it. |
| Tasks | `task` issues whose parent is the epic. `blocked_by` links between tasks are **the rule** for ordering. Waves are only a guide. |
| Integration checkouts | One per lane (§1.1). During a sprint, each has its lane's sprint branch checked out. Only you write to them. |
| Worker checkouts | `.worktrees/<KEY>/` (gitignored). One per issue, on `feature/<KEY>-<slug>`. |
| Max parallel workers | Per lane: the plan's number, or 4 if the plan does not give one. **Both lanes together: never more than 6.** If the two plans add up to more than 6, the lower-numbered sprint keeps its full number and the other lane gets the rest. |
| Project docs | Trama space `DENG`. Workers write module, tool and decision pages as drafts; you publish them after a green merge and own the index pages (DOCS.md §3). |

### 1.1 Lanes

DENG has two Andare **sprint channels**. Each channel can have one active sprint, so you run one sprint in each. A **lane** is a channel plus its own integration checkout:

| Lane | Andare channel | Integration checkout |
|---|---|---|
| **Main** | `Main` | The main repo directory (this one). |
| **Alt** | `Alternate Development` | `.worktrees/_alt/`. This is a git worktree (gitignored), not a worker worktree. Create it when lane Alt starts its first sprint (§2.2.2). |

- A lane's sprint is the **active** sprint in its channel. `list_sprints DENG` shows the channel on each row, so the state comes from Andare and not from memory.
- `main` is checked out only in the main repo directory. Every merge **into** `main` happens there (§2.7.2), also for lane Alt.
- The two sprint branches never merge into each other. Code goes from one lane to the other only through `main` (the sync in §2.3).
- Lane Main usually runs the lowest-numbered open sprint. Lane Alt runs a later sprint that passed the parallel check (§2.2.3). Either lane can run either kind of sprint; what matters is the check.
- Pass `channel` on every sprint call (`start_sprint`, `plan_sprint`, `complete_sprint`, `set_ceremony`, `list_ceremonies`). A moved sprint has the same name in both channels, so a call without `channel` is ambiguous.

---

## 2. Each tick

Do these steps in order. Do not skip a step because you "remember" the state; check it.

Do §2.1 once. Do §2.2 once (it starts a sprint in each idle lane). Then do §2.3–§2.7 **for each lane** that has an active sprint, the lower-numbered sprint first. Then do §2.9. In §2.3–§2.7, "the integration checkout", "the sprint branch" (`sprint/<…>`) and "the epic" mean **that lane's**. Run git commands for lane Alt with `git -C .worktrees/_alt …`, or from that directory. A worker's completion notification wakes you for its lane; do §2.6 for it, then finish the tick for both lanes.

### 2.1 Preflight (cheap; do it every tick)
1. `git status` in **each** integration checkout (the main repo directory, and `.worktrees/_alt/` if it exists). Each must be clean, apart from untracked files in `.worktrees/`. If a merge is half done (`MERGE_HEAD` exists), finish it or abort it as §2.4 (or §2.3 for a sync) says before you continue.
2. `git worktree list` gives the worktrees that are dispatched at the moment. `.worktrees/_alt` is lane Alt's checkout, not a dispatched task.
3. If the repo has no commits, or `docs/agents/` is missing, stop and tell the user. The bootstrap commit is required.
   - **CI on each active sprint branch** (when `origin` exists): `gh run list --branch sprint/<…> --limit 6`. For a completed run of the current sprint tip that failed, read `gh run view <id> --log-failed`. A failure that a running or ready task already covers needs nothing. Otherwise file a `[Sxx] Fix: …` task (`priority: highest`, the failing jobs and error lines in the description) and treat it as ready. A green Windows/Linux job that proves a `needs-hw-verify` item: comment the run URL on that issue and remove the label.
4. Docs bootstrap (once): if `get_page_tree DENG` has no `DEngine` page, create the DOCS.md §2 skeleton: every page down to the index level (not the `Module:`, `Tool:`, `Decision:` or `Sprint Sxx:` pages), each with a short body that says what will go there. Seed the `Modules` table from `git show c9ab5f6:docs/modules/README.md`, and embed DENG-2 on `Decisions` with `link_to_issue`. Publish them.

### 2.2 Find the active sprints and fill idle lanes
1. `list_sprints DENG`. Each **active** sprint's row names its channel, and so its lane (§1.1).
2. A lane with an active sprint keeps it. Do §2.3–§2.7 for it.
3. For each lane with **no** active sprint (lane Main first), choose a sprint (§2.2.1) and start it (§2.2.2).
4. If neither lane has an active sprint, no future sprint has issues, and every epic is Done, go to §2.8 (finish).

#### 2.2.1 Choose the next sprint for an idle lane
The **candidates** are the future sprints that have issues, in numeric order. A sprint that is active in the other lane is not a candidate. If two future sprints share a number, use the one with issues.

For each candidate, find its epic from the fixed mapping: **the epic for sprint `Sxx` is `DENG-(xx+2)`**. For example, S01 → DENG-3, S20 → DENG-22, S39 → DENG-41. Confirm with `get_issue` that the summary starts with `[Sxx]`. Do **not** look up the epic by searching on the sprint name: AQL `sprint = "<name>"` returns nothing for some sprint names (S06, S18, S22, S34, S36).

1. Take the first candidate whose epic has **no unfinished blocker** in `get_issue`. Usually this is the next sprint after the last one that closed.
2. If there is none, and the other lane has an active sprint, look ahead: run the **parallel check** (§2.2.3) on the first **5** candidates, in numeric order. Take the first one that passes.
3. If nothing passes, this lane stays idle in this tick. Do not start a sprint whose epic still has an unfinished blocker.

#### 2.2.2 Start the chosen sprint
1. Read DENG-2 (decisions) and the `## SPRINT PLAN (v2)` section of the epic description (`get_issue` on the epic).
2. Read the docs for context (DOCS.md §5): `DEngine`, `Architecture`, the previous `Sprint Sxx: …` page and `Verification Backlog`.
3. **Put the sprint in the lane's channel.** All sprints were created in `Main`. If the sprint is in the other channel, move it: `create_sprint` with the same name and goal and `channel: <this lane's channel>`, then `plan_sprint` (with `channel`) for the epic **and** every child in the epic's `## Children` list. Run `list_sprints` again: the new sprint must have all the issues, and the old one 0. A sprint already in the lane's channel does not move.
4. `start_sprint` with `durationDays: 14` and `channel: <this lane's channel>`.
5. Create the sprint branch from `main`. Use the exact name from the plan.
   - Lane Main: `git checkout main && git checkout -b sprint/<code>-<slug>` in the main repo directory.
   - Lane Alt, first time: `git worktree add .worktrees/_alt -b sprint/<code>-<slug> main`. Later times (the worktree exists and is clean): `git -C .worktrees/_alt checkout -b sprint/<code>-<slug> main`.
6. Set the epic to `In Progress`. Comment on it: `▶ Sprint started in lane <Main|Alt>. Branch sprint/… from main @ <sha>. Max parallel N. Parallel with <other lane's sprint, or "none">.`

#### 2.2.3 Parallel check
Run this check only to start a sprint whose epic still has an unfinished blocker. The candidate is `Sy`. The **open earlier sprints** are every sprint `Sx` with x < y whose epic is not Done. Usually that is the other lane's active sprint, but it can include sprints that have not started. `Sy` passes only if it is independent of **every** open earlier sprint.

First, skip the check if the `Sy` epic already has a `⏸ Not parallel` comment whose blocking sprints are all still open. The answer has not changed.

For each open earlier sprint `Sx`, `Sy` is independent of `Sx` when all of these are true:
1. **Links.** No issue in `Sy` (the epic or a task) is `blocked_by` an issue in `Sx`, apart from the epic-to-epic link. Read the `links:` line of each `Sy` task with `get_issue`.
2. **Text.** The `Sy` plan and task descriptions (Context, Scope, Notes) do not say they use, extend or call something that `Sx` builds. Look for `Sx`'s task keys, its epic key, `S<x>` and the names of `Sx`'s APIs. A mention that only says a feature is out of scope or belongs to `Sx` does not count.
3. **Modules.** The modules that `Sy` changes or calls do not include a module that `Sx` creates or changes. Use the epics' `**Headers:**` lines, the tasks' `Owns paths` and `Shared paths`, and the module table in `tools/check_layers/layers.toml`. A module that `Sy` includes, or that is in a band `Sy` builds on, counts as "calls".
4. **Files.** No path is in the `Owns paths` of both sprints. Shared files that both plans list as append-only (CMake source lists, `third_party/vendor.toml`, test registration lists) are allowed.

If you are not sure about a point, the check fails.

**If `Sy` fails:** comment on the `Sy` epic: `⏸ Not parallel while <Sx, …> open: <one-line reason>`. Go to the next candidate.

**If `Sy` passes:**
1. Comment on the `Sy` epic: `⇉ Parallel check passed: independent of <Sx, …> (links, text, modules, files). Starts in lane <lane> alongside <other lane's sprint>.` Give the main reason for each `Sx` in one line.
2. Re-link the epics, so that Andare stays the source of truth for order:
   - For each open earlier `Sx` whose epic the `Sy` epic is `blocked_by`: `unlink_issues` the two epics.
   - For each epic that is `blocked_by` the `Sy` epic (usually `Sy+1`): `link_issues` it `blocked_by` each epic you just unlinked. This keeps every later sprint behind both.
   - Comment the change on each epic you changed.
3. `get_issue` the `Sy` epic again. It must now show no unfinished blocker. Then start it (§2.2.2).

### 2.3 Sync with main, then load the sprint's tasks
**Sync.** If `main` has moved since the sprint branch was cut (`git merge-base --is-ancestor main sprint/<…>` fails), the other lane closed a sprint. Bring it in before you merge anything else:
1. `git merge --no-ff main -m "Sync main into sprint/<…>"` in the integration checkout.
2. If there is a conflict only in append-only files (see §2.4.3), keep both sides and commit. For any other conflict: `git merge --abort`. File a task under the epic in this sprint (`[Sxx] Fix: integrate main (<files>)`, with points, `priority: highest`), and add the comment `⚠ Sync with main conflicts in <files>`. To dispatch it, create its worktree from the sprint branch, run `git -C .worktrees/<KEY> merge --no-ff main`, and dispatch it in **fix** mode with `Reason: a merge of main is in progress in your worktree. Resolve the conflicts so that both sides keep their behaviour, commit the merge, then continue with the normal steps.` Do not merge other branches into this sprint until that task is Done.
3. Verify as §2.4.4 says. If verification fails, `git reset --hard ORIG_HEAD`, and file and dispatch a fix task in the same way, with the failing output in `Reason:`.
4. If verification passes, comment on the epic: `🔄 Synced main @ <sha> into sprint/<…>. Build + tests green.` If an `origin` remote exists, push and start CI as §2.4.6 says.

**Load.** `search_issues` with `project: "DENG"`, `parent = <EPIC> ORDER BY key ASC`, limit 100. If the number of tasks differs from the epic's `## Children` list in `get_issue`, use the children list. For each task, record its status, labels, priority and `blocked_by` keys. Call `get_issue` when you need the blocking details or the latest comments.

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

### 2.4 Merge the review bucket (one at a time, in key order)
For each task in **review**:
1. `git checkout sprint/<…>` in the integration checkout.
2. `git merge --no-ff feature/<KEY>-<slug> -m "Merge <KEY>: <summary>"`.
3. **If there is a conflict:**
   - If the conflicts are only in files the plan lists as *append-only* (module `CMakeLists.txt` source lists, test registration lists, docs indexes), resolve them so both sides are kept. Then commit.
   - Any other conflict: `git merge --abort`. Set the issue to `In Progress`. Comment `⚠ Merge conflict with sprint branch in <files>. Re-dispatching in rebase mode.` Re-dispatch it in **rebase** mode (§2.5).
4. **Verify** the merged sprint branch on the host preset (`cmake --preset <host>-dev`, `cmake --build --preset <host>-dev`, `ctest --preset <host>-dev`), plus the header-compile and layering checks once they exist (S01 onward). Use the commands from `docs/building.md` once S01 has written that file. Before then, use plain `cmake -S . -B build/dev && cmake --build build/dev && ctest --test-dir build/dev`.
5. **If verification fails:** `git reset --hard ORIG_HEAD`. This undoes only the merge you just made. Set the issue to `In Progress`. Comment with the failing output (the last ~60 lines in a code block). Re-dispatch in **fix** mode.
6. **If verification passes:**
   - Comment: `✅ Merged into sprint/<…> @ <sha>. Sprint build + tests green (<n> tests).`
   - Publish every Trama page in the worker's `DOCS:` line: `publish_page` (space `DENG`) with the note `<KEY> merged @ <sha>`. First `get_page` each one and check that it still has the `![[andare:KEY]]` embed of every issue that was on its last published version (plus this one). If an embed is missing, the draft overwrote another worker's text: do not publish it; ask the owner of the lost text (resume that worker, or dispatch a docs fix) to restore it, then publish.
   - Set the issue to **Done**.
   - If an `origin` remote exists: `git push origin sprint/<…>`. Never force. Then start CI on the new tip: `for w in CI Sanitizers Checks; do gh workflow run "$w" --ref sprint/<…>; done` (push events have not been starting runs on this repo; `cancel-in-progress` drops a duplicate if they do). The next preflight reads the result (§2.1.3).
   - `git worktree remove .worktrees/<KEY>`. Keep the feature branch.

Merge in key order unless the plan's *merge order* section says otherwise.

### 2.5 Dispatch workers
`slots = maxParallel − (running + orphaned being resumed)`, where `maxParallel` is this lane's share (§1, *Max parallel workers*). Fill the slots in this order:
1. **orphaned** tasks, in **resume** mode. Reuse the existing worktree.
2. tasks you sent back in this tick, in **fix**/**rebase** mode.
3. **ready** tasks, sorted by priority (highest first), then by the wave in the description, then by key.

For each task you dispatch:
1. Get the slug from the `**Branch:**` line of the description (`feature/<KEY>-<slug>`). If there is no such line, make a kebab-case slug of at most 5 words from the summary.
2. New tasks: `git worktree add .worktrees/<KEY> -b feature/<KEY>-<slug> sprint/<…>`. Resume, fix and rebase modes: reuse the existing worktree and branch.
3. Start the worker with the **Agent** tool (`subagent_type: general-purpose`, description `<KEY> worker`, **no** `isolation` option, because you manage the worktrees yourself). Use this prompt, with the values filled in:

```
You are the DEngine worker for Andare issue <KEY> ("<summary>").
Mode: <new|resume|fix|rebase|retry>
Worktree (your ONLY working directory; use absolute paths): <abs path to .worktrees/KEY>
Feature branch: feature/<KEY>-<slug>
Sprint branch (base, do not modify): sprint/<…>
Sprint epic: <EPIC KEY> · Decisions: DENG-2
Parallel sprint (NOT in your base): <the other lane's sprint name and branch, or "none">
<fix/rebase/retry only: "Reason: <failure output / conflict files / previous failure comment>">

First read <worktree>/docs/agents/WORKER.md, <worktree>/docs/agents/CONVENTIONS.md and <worktree>/docs/agents/DOCS.md and follow them exactly.
Finish with the RESULT block that WORKER.md specifies.
```

4. Each worker is in the background. Do not wait in a loop. Go on to the next slot.

You may start several workers in one message. That is the preferred way.

### 2.6 Handle worker results
A worker's final message contains a `RESULT:` block (see WORKER.md).
- `RESULT: ready-for-merge`: confirm that the issue is now `In Review` and that the branch has the commit the worker gave. Then merge it at once (§2.4), and dispatch again at once (§2.5), because more tasks may now be ready.
  - Docs are part of done. If `DOCS:` is `none` and the issue changed code under `src/`, `tools/`, `cmake/` or `tests/`, the worker must give a reason in `NOTES`. With no reason, do not merge: set the issue to `In Progress` and re-dispatch in **fix** mode with `Reason: Trama docs missing (DOCS.md §3)`.
- `RESULT: failed`: the worker has already commented and set the issue back to `To Do`. Add the label `agent-failed` (`update_issue`; keep the existing labels). Then:
  - First failure: re-dispatch in **retry** mode, with the worker's failure notes in `Reason:`.
  - Second failure (the label is already there): call `request_decision` on the issue (person: `me`). Write the question in one sentence and include the worker's blocker. Add the label `needs-human`. Leave the worktree in place for the human. Go on with the other ready work.
- If a worker ends with no RESULT block, treat it as **orphaned** on the next tick.

### 2.7 Close the sprint
When every task is in **done**:
0. Do the sync of §2.3 again if `main` has moved. The exit checks must run on the sprint branch with every closed sprint in it.
1. Run the plan's **Sprint exit checks** on the sprint branch: a full clean build of every preset the host can build, all tests, the header-compile and layering checks, and the sprint-specific checks. With `origin`: the CI runs on the sprint tip are green, or each red job is either a filed fix task (then wait for it) or a hardware item recorded on the Verification Backlog. Also the **docs check**: every module and tool the sprint changed has a published page whose `Status` and `Last change` match this sprint, and `pages_referencing` finds a page for every task that changed code (or its report gave a reason). An issue that says `docs/modules/<mod>.md` is satisfied by the Trama page `Module: <mod>`. If a check fails, file a `task` under the epic in this sprint (`[Sxx] Fix: …`, with points, `priority: highest`) and treat it as ready. Do not close the sprint with a red exit check.
2. Merge into main, **in the main repo directory** for both lanes: `git checkout main && git merge --no-ff sprint/<…> -m "Sprint <Sxx> <name>"`, then `git tag sxx-done`. If an `origin` remote exists, `git push origin main sprint/<…> --tags`. Never force-push. Then, if lane Main still has an active sprint (this was lane Alt's sprint), `git checkout sprint/<lane Main's sprint>` in the main repo directory again. The other lane picks up the new `main` with its next sync (§2.3).
3. Write the **SPRINT REPORT** as the Trama page `Sprint Sxx: <name>` under `Sprints`: the tasks and points completed, the merge sha, the exit checks with their results, every issue with the `needs-hw-verify` label and what it still needs, decisions or deviations the workers recorded (link the `Decision:` pages), and follow-ups filed. `link_to_issue` the epic on it. Then update the index pages you own (DOCS.md §3): the `Sprints` row, `Modules` statuses, `Verification Backlog` (add new items, remove cleared ones), `Architecture` (the workers' `Architecture:` follow-ups), and the status line on `DEngine`. Read `list_stubs DENG`: write the index-level stubs, and list the rest in the report. Publish every page you changed. Comment on the epic with a 5–10 line summary and the report page title.
4. `set_ceremony` (with `channel`) with type `review` (held: true) and the same summary. Add type `retrospective` (held: true) with what slowed the sprint down (merge conflicts, retries, unclear specs) and the changes to make next time.
5. Set the epic to **Done**. Call `complete_sprint` with `channel: <this lane's channel>` and `moveUnfinishedTo: "backlog"`. The user has authorised this call for sprints with no unfinished issues. If a sprint still has unfinished issues, do not complete it. Ask the user instead.
6. **Milestone gates:** after **S20** (Milestone 1) and after **S37** (runtime v1), send a `PushNotification` (if available) with the report link, then go on. The user asked for an unattended build. The notification keeps them informed and does not stop the loop.
7. Go straight back to §2.2 in the same tick, to start the next sprint in this lane. A closed sprint can unblock a sprint for either lane.

### 2.8 Finish
When all 39 epics are Done and neither lane has an active sprint, comment a final summary on DENG-1, set DENG-1 to Done, and end the loop with `ScheduleWakeup { stop: true }`.

### 2.9 End of tick
- If workers are running: `ScheduleWakeup` with `delaySeconds: 1800` and the reason `waiting on N workers in <sprint> + M in <sprint>` (one entry per lane, or `lane Alt idle`). Worker notifications wake you before then.
- If nothing can run in either lane and only escalated tasks remain: `ScheduleWakeup` with `delaySeconds: 3600` and the reason `blocked on human decision(s): <keys>`. Each time you wake, check whether the decisions have been answered.
- Always pass the same loop prompt back (`Follow docs/agents/LOOP.md`).

---

## 3. Rules

- **Andare is the source of truth** for what to do. **Git is the source of truth** for what has been done. If they disagree (for example, an issue is Done but its branch was never merged), fix Andare to match git and comment about it.
- At most one active sprint per lane, so two at most. A sprint whose epic was blocked starts only after it passes the parallel check (§2.2.3). Never start a task whose blockers are not Done.
- Keep the lanes apart. A lane's workers, merges and verification use only that lane's sprint branch and integration checkout. Never merge one sprint branch into the other. Code goes between lanes only through `main`.
- Never commit feature code in an integration checkout. Your only commits are merges, the conflict resolutions in §2.4.3, syncs of `main` (§2.3), and the sprint merges into `main`.
- Never edit or delete issues in other sprints. The only exception is the epic re-linking in §2.2.3 and moving a sprint's issues to its lane's channel (§2.2.2). You may **file** follow-up issues: in the current sprint if this sprint's exit needs them, otherwise in the sprint whose module owns the work, as a `task` under that epic. Comment the reason on both issues.
- Workers set `In Progress` and `In Review`. You set `Done`, after a green merge. Do not set statuses for a worker that is still running.
- Use `requestId` on every `create_issue` / `create_issues` call, so that retries are safe.
- Keep each tick short. Read issue descriptions only when you dispatch or merge.
- **Trama explains** what was built and why (DOCS.md). If a page disagrees with git, git wins: fix the page, or file a follow-up for the owner.
- If the user comments on an issue or edits DENG-2, their instruction overrides this file.

## 4. Host prerequisites (a human installs these once)

macOS arm64 with: Xcode + command line tools, CMake ≥ 3.28, Ninja, Python 3.10+, git ≥ 2.40, the Vulkan SDK (MoltenVK). Optional, for Windows cross-compiles: `xwin` with the Windows SDK/CRT already downloaded. Downloading them means accepting Microsoft's license, so a **human** does it, not an agent. Optional: a Linux sysroot for Linux cross-compiles. Optional: a GitHub `origin` remote, so CI runs the full matrix.

A worker that needs a missing prerequisite builds what it can and records the gap in `needs-hw-verify` (see CONVENTIONS.md). It does not install licensed SDKs.
