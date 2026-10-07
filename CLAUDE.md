# CLAUDE.md — ProWindows

Native C++17 / Win32 tiling window manager for Windows 11. No dependencies, no project files:
`build.bat` compiles everything. **The release in progress is 1.6** — the plan, the design spec
and the task list are in [`docs/PLAN-1.6.md`](docs/PLAN-1.6.md). Tick its checkboxes as tasks land.

## Spend tokens like they cost money (they do)

- **Never read `MAP.md` or `README.md` whole** (60 KB each). `grep -n` for the invariant number,
  file name or heading you need and read that range. MAP's invariants are numbered; code quotes them.
- Read a source file by range once you know where to look (`grep -n` first). `wm.cpp`,
  `settings.cpp`, `main.cpp` and `monitor.cpp` are 1.4k–3k lines.
- Look at test PNGs only when a task needs an eye on them, one or two at a time, zoomed.
- Do not re-review what `docs/REVIEW-*.md` already settled.
- The design references are `docs/design/ref/*.webp` (gitignored). The spec in PLAN-1.6 §2 is
  written so they are rarely needed; open one only to settle a visual question.

## Model routing — who does what

No Fable 5.1 anywhere in this project. The main session should run on **Sonnet 5.5** and
delegate through the agents in `.claude/agents/`:

| Agent | Model | Use it for |
| --- | --- | --- |
| `janitor` | Haiku 4.5 | Mechanical work with an exact recipe: deleting build output, moving files, string sweeps across docs, version bumps, registering a source in `build.bat` and the `tests\*.bat` lists, running a harness and reporting pass/fail with the failing lines. Nothing that needs a design decision. |
| `builder` | Sonnet 5.5 | Implementing a task from PLAN-1.6 whose spec is written down: theme and painter changes, layout code, optimisations, test-harness updates. Builds and runs the harnesses itself until green. |
| `reviewer` | Opus 5.5 | Judgement: the pre-release code review, threading / invariant questions, signing off a design decision the spec leaves open, the final diff review before tagging. Give it narrow questions with file paths — it is the expensive one. |

Rules of thumb: if the task fits in one sentence with no "decide", use `janitor`. If it needs code
written against a spec, use `builder`. If it needs someone to decide or to find what is wrong,
use `reviewer`. Run independent agents in parallel. Agents report back in ≤ 15 lines.

## Build and test

```bash
build.bat
```
- The exe must not be running when you build (locked file). Build beside it: `build.bat PW_dev.exe`.
- A new `.cpp` must be added to `build.bat` **and** to every `tests\*.bat` that compiles the UI
  (`uishot`, `clickprobe`, `launchshot`, `analyze`).
- Sources are UTF-8 **without BOM**, **LF** endings; `/utf-8` is required. Never let an editor add a BOM.
- Safe harnesses (do not start the tiler): `tests\run.bat` (layout asserts), `tests\uishot.bat`
  (settings UI, exit code 1 on failure, PNGs in `tests\shots`), `tests\launchshot.bat`,
  `tests\monshot.bat`, `tests\clockshot.bat`, `tests\iconcache.bat`, `tests\analyze.bat`.
- `tests\clickprobe.bat` moves the **real mouse** for a few seconds — say so before running it.
- `tests\ctl_test.ps1` starts a **real tiler that rearranges the desktop** — only with the user's OK.
- `PrintWindow` lies about client areas; trust `uishot`'s own capture (MAP "Testing").

## Code rules that bite

- The UI thread never waits: anything that can block goes on a thread; cross-process calls time out.
- Every edited settings field must be listed in `AWA_EDITED_FIELDS` or Apply silently drops it (inv. 78).
- Row callbacks must not touch a `Row&` after calling out — rows are rebuilt underneath (inv. 79).
- Footer prompts only show keys that really work, and clicking one sends that key (inv. 74).
- Spaced text: measure with `theme::SpacedWidth`, draw with `DrawSpaced` (inv. 77).
- Match the surrounding style: plain prose comments explaining *why*, `awa::` namespaces, no
  new dependencies, no STL-heavy cleverness in paint paths.

## Git

Commit only when asked. Message style: one summary line of what changed, terse clauses
separated by `;` / `,` (see `git log`). The 1.5 work is still uncommitted — see PLAN-1.6 task 0.1.
