# Will Rune still be maintained?

Research for [#19](https://github.com/FredeAlexandre/beamer/issues/19), which blocks the toolkit
choice in [#13](https://github.com/FredeAlexandre/beamer/issues/13) (part of the Rust port map,
[#1](https://github.com/FredeAlexandre/beamer/issues/1)). Surveyed 2026-11-16 (a Monday) from the
GitHub API, the Rune repository at `dev`, the repositories of Anomaly's other projects, and the
company's own pages. Counts are as of that day. **Evidence** is marked with a link. Anything marked
*Inference* is our reading of that evidence.

The question is only whether [anomalyco/rune](https://github.com/anomalyco/rune) will be dropped or
go quiet. API churn and the lack of releases are out of scope, except as signals of commitment.

## Answer

**Dropped-risk: low at ~3 months, low-to-medium at ~12 months.**

Rune is not a side experiment. It is the UI layer of products Anomaly already ships:

- **Hex**, their voice-dictation app with 7k stars and weekly releases, has Rune as its only UI
  toolkit.
- Four more Anomaly apps (Deadbolt, Hokum, Clip, Flicks) are built on it.
- Its stated flagship goal is to rebuild OpenCode Desktop, the desktop app of their main product.

Three Anomaly people commit to Rune every week, one of them a co-founder. Over the last 14 days
about 1,000 commits landed, as many as in any two-week stretch since launch.

The 12-month risk is not "archived". It is "goes quiet in public":

- Two people do almost all the work.
- The OpenCode Desktop port is a goal, not a commitment. OpenCode's desktop app is still Electron.
- Anomaly has a history of moving its attention (SST is in low maintenance) and of archiving
  experiments quickly.
- The issue tracker is mostly untriaged.

## 1. Who is behind it

- **The company.** The [Anomaly GitHub organisation](https://github.com/anomalyco) ("For whatever
  you build"), [anoma.ly](https://anoma.ly). The landing page credits Anomaly as "Creators of"
  [OpenCode](https://opencode.ai), and lists side projects (Rebase trivia, an Antarctica bike
  challenge). It says nothing about the team or funding.
- **Founders and history.** This comes from a secondary source,
  [Tech Funding News, 2026-01-13](https://techfundingnews.com/opencode-the-background-story-on-the-most-popular-open-source-coding-agent-in-the-world/),
  and is not verified against a primary one:
  - Jay V (CEO) and Frank Wang (CTO) founded it. Dax Raad (`thdxr`) and Adam Elmore are
    co-founders of OpenCode.
  - SST went through Y Combinator and raised about $1M in 2021, and turned profitable in 2025.
  - OpenCode raised an undisclosed round within months of launch.
  - OpenCode's hosted models make "several million dollars in annualized revenue".

  We found no primary funding announcement.
- **Business model.** This one is first-party. OpenCode sells pay-per-token hosted models, a
  fixed-price "OpenCode Go" subscription, Teams and Enterprise plans
  ([opencode.ai/zen](https://opencode.ai/zen)).
  [anomalyco/opencode](https://github.com/anomalyco/opencode) has about 227k stars.
- **The people on Rune** ([contributors API](https://api.github.com/repos/anomalyco/rune/contributors)):

  | Person | Commits | Affiliation |
  |---|---|---|
  | `kitlangton` (Kit Langton) | 1,812 | Profile says "Anomaly". Author of the original Hex ([kitlangton/Hex](https://github.com/kitlangton/Hex)) |
  | `Hona` (Luke Parker) | 508 | Public member of `anomalyco` |
  | `thdxr` (Dax Raad) | 99 | Co-founder. Public member. He made the `Rune` root commit and the decision to make it public ([hex `rune-dependency.md`](https://github.com/anomalyco/hex/blob/dev/docs/plans/rune-dependency.md)) |
  | `jlongster` (James Long) | 23 | No affiliation on his profile |
  | `rekram1-node` (Aiden Cline) | 4 | Profile says `@Anomalyco` |
  | 7 others | 1–2 each | Outside contributors |

### Track record of keeping and killing projects

The [organisation's repositories](https://github.com/orgs/anomalyco/repositories) number 86, of
which 29 are archived. The pattern:

- **Products are kept, but attention moves.**
  - SST v2 was superseded by v3, not abandoned. [anomalyco/v2](https://github.com/anomalyco/v2)
    is unarchived, still has docs at v2.sst.dev, and was pushed on 2026-11-10.
  - The `ion` repository, where v3 was built, was archived once its work moved into
    [anomalyco/sst](https://github.com/anomalyco/sst) ("This repository was for work on sst v3
    which is now releated").
  - SST itself is now quiet. It had 8 commits in the 30 days to 2026-11-16, and its last release
    was v4.17.2 on 2026-10-08.
  - OpenAuth (7.4k stars) had no commits in the same 30 days.
- **Experiments are archived fast, within weeks to months.** Examples are `browser-control`
  (created 2026-07, archived), `haunt`, `opencode-voice`, `oc-plugin-rainbow`,
  `effect-http-recorder`, `opencode-sdk-go` and `opencode-bench`.
- *Inference:* what gets dropped is what nothing else they ship depends on. A project that loses
  its owners' focus does not get archived. It slows to a trickle, as SST has.

## 2. Why Rune exists

This is the most important point. Rune is the UI layer of products Anomaly ships, and it was built
because the alternatives failed for them.

- **Stated purpose.** [`PLAN.md`](https://github.com/anomalyco/rune/blob/dev/PLAN.md) says:
  - "The flagship test is **recreating OpenCode Desktop** in Rune and raising its bar".
  - OpenCode's theme is "the first theme built on it and the reference example".
  - `rune-kit` has "the same names as `@opencode/ui`".
  - The "first milestones" are "Hex's Settings as the first real screen … then OpenCode Desktop's
    main screens".

  Section 1, "Why build this at all", explains the cause. Kit rebuilt the same surfaces three
  times in ten days, across GPUI, Metal and SwiftUI, and Hex's native branch "stalled because Hex
  also ships on Windows and Linux".
- **OpenCode Desktop is not on Rune yet.** Evidence:
  [`packages/desktop/package.json`](https://github.com/anomalyco/opencode/blob/dev/packages/desktop/package.json)
  is Electron 44 with electron-builder. A code search of `anomalyco/opencode` finds no Rune
  dependency. The [README](https://github.com/anomalyco/rune#readme) shows screenshots of "A coding
  agent app built with Rune" (`demo/opencode/`, committed 2026-10-22). No public repository holds
  that app. *Inference:* it is a private prototype or golden-image test, and the port is not
  announced.
- **Hex ships on Rune.** [anomalyco/hex](https://github.com/anomalyco/hex) ("Local voice dictation
  for mac, windows and linux", 7.0k stars, product page [anoma.ly/hex](https://anoma.ly/hex)):
  - Its [`Cargo.toml`](https://github.com/anomalyco/hex/blob/dev/Cargo.toml) depends on `rune`,
    `rune-kit`, `rune-theme-opencode`, `rune-motion` and `procwatch`, all pinned to one Rune rev.
    It also patches `winit`, `parley_engine`, `wgpu-hal` and both AccessKit backends from Rune's
    repository.
  - [`docs/plans/rune-ui.md`](https://github.com/anomalyco/hex/blob/dev/docs/plans/rune-ui.md)
    says: "Rune is Hex's only UI toolkit and GPUI is deleted. Every surface runs on Rune"
    (macOS, Windows and Linux; only the macOS dictation HUD stays native Metal).
  - Hex released [3.0.0 through 3.4.0](https://github.com/anomalyco/hex/releases) between
    2026-10-31 and 2026-11-11, with about 10k downloads across them. Its commits bump the Rune
    pin repeatedly (for example "Bump Rune to f401bce5", 2026-10-29).
  - Hex had a Rune front end before Rune went public. The decision record says "Rune was published
    with a clean history", and earlier pins "no longer exist". Rune was made public so that Hex's
    CI, Nix build and contributors could fetch it.
- **Other Anomaly apps on Rune.** Each depends on `anomalyco/rune` in its own `Cargo.toml`:
  - [Deadbolt](https://github.com/anomalyco/deadbolt), a password manager:
    [`crates/app`](https://github.com/anomalyco/deadbolt/blob/dev/docs/app.md), "built in Rune".
  - [Hokum](https://github.com/anomalyco/hokum), a Wayland control centre: "drawn with Rune, our
    Rust UI framework".
  - [Clip](https://github.com/anomalyco/clip): "A Rune app for macOS".
  - [Flicks](https://github.com/anomalyco/flicks), a Plex alternative: desktop, web and UI crates.
  - Kit's own [psychopomp](https://github.com/kitlangton/psychopomp).
- **Outside adopters already exist.** For example
  [freenet/river](https://github.com/freenet/river/blob/main/apps/Cargo.toml), found by
  [code search for `github.com/anomalyco/rune`](https://github.com/search?q=%22github.com%2Fanomalyco%2Frune%22&type=code).
  None of them is large.

*Inference:* dropping Rune would mean rewriting the UI of Hex and four younger apps. Hex is the
company's second-largest product by stars. That is far costlier for them than keeping Rune.

## 3. Activity signals

All counts are from the [commits API](https://api.github.com/repos/anomalyco/rune/commits?sha=dev)
on `dev` (2,458 commits). The `stats/commit_activity` endpoint returned no data that day (GitHub
was still computing it).

| Week (Thu–Wed) | Commits |
|---|---|
| 2026-10-22 – 10-28 (launch week) | 1,022 |
| 10-29 – 11-04 | 508 |
| 11-05 – 11-11 | 586 |
| 11-12 – 11-16 (5 days) | 342 |

- **The last 14 days** (2026-11-02 to 11-15) had 1,016 commits:

  | Author | Commits |
  |---|---|
  | Hona | 507 |
  | kitlangton | 457 |
  | thdxr | 53 |
  | jlongster | 1 |

  Every day had commits. Peaks were 181 on 11-10 and 164 on 11-11. Hona merged PR
  [#1058](https://github.com/anomalyco/rune/pull/1058) on 11-16. Kit's last Rune push was 11-14,
  and his [public events](https://api.github.com/users/kitlangton/events/public) show him working
  on Hex (Rune's consumer) through 11-16.
- **Humans or agents.** All substantive authors are the human accounts above. Five commits carry
  a `claude` author and five carry a `Co-authored-by: Claude` trailer. The README and
  `AGENTS.md`/`CLAUDE.md` make clear the work is agent-driven under those humans ("our agents
  usually turn it around fast").
  - Kit commits straight to `dev` (1,802 commits with him as committer).
  - Hona works through pull requests he merges himself: 96 of the last 100 merged PRs are his,
    4 are thdxr's.
  - Outside pull requests are not taken: "We don't take pull requests"
    ([README](https://github.com/anomalyco/rune#contributing)).
- **Issues.** 429 issues have been filed: 408 are open and 21 closed. 415 were filed in the last
  14 days, of which 14 were closed. 320 have no comment at all. Maintainers do comment (`kitlangton`
  57, `thdxr` 15 and `Hona` 8 across issues and PRs). Closed issues were mostly closed within a week,
  many the same or next day. Only 3 issue numbers appear in commit messages.
  - *Inference:* the "turn it around fast" claim is not visible in aggregate. Either issues feed an
    internal agent loop that does not link back, or most go unread. For Beamer this means: expect
    to fix your own blockers, not to get them fixed on request.
- **Process signals of commitment.** These are inferences:
  - A maintained [`CHANGELOG.md`](https://github.com/anomalyco/rune/blob/dev/CHANGELOG.md) of
    output-changing edits for consumers.
  - The promise that "`dev` is never rewritten" ([README](https://github.com/anomalyco/rune#add-it)).
  - 34 research notes in [`research/`](https://github.com/anomalyco/rune/tree/dev/research).
  - A website build under `www/` ("Rune · A Rust UI framework with its own GPU renderer").

  A team planning to drop a project does not build these.
- **No public statement on lifespan.** We found no announcement post, roadmap commitment or
  statement about lifespan from Anomaly. The X searches returned nothing first-party. Coverage is
  second-hand, for example [ayu-sh-kr.com](https://www.ayu-sh-kr.com/news/rune-native-ui-framework/)
  and [dev.to](https://dev.to/dishant0406/rune-is-a-rust-ui-framework-written-for-agents-not-you-g92).
  The only first-party commitment is `PLAN.md`'s milestone list.

## 4. Bus factor and fallback

- **Concentration.** Kit Langton wrote 74% of all commits. He and Hona together wrote 94%. Dax
  (co-founder) writes about 4%.
  - *Inference:* if Kit left Anomaly, Rune's direction would stall. Hona's current rate, over half
    of the last 14 days' commits, suggests it would not stop. The bus factor is about 2.
- **Licence.** MIT, "Copyright (c) 2026 Anomaly"
  ([LICENSE](https://github.com/anomalyco/rune/blob/dev/LICENSE)). Code already published cannot
  be withdrawn. Rune was private until 2026-10-22, so it going private again for new work is a
  conceivable, if unlikely, failure mode.
- **Size.** Measured from the [git tree API](https://api.github.com/repos/anomalyco/rune/git/trees/dev?recursive=1):

  | Part | Files | Size |
  |---|---|---|
  | `crates/` | 1,471 `.rs` files | 16 MB |
  | `crates/rune` alone | 1,064 `.rs` files | 11.8 MB |
  | `vendor/` | 300 `.rs` files | 5.1 MB |

  *Inference:* that is roughly 350–450k lines, written in about 25 days of public history plus
  some private history before it. Nobody maintains that by hand.
- **Patched upstream crates.** Rune vendors and patches six crates through `[patch.crates-io]`:
  `winit`, `wgpu-hal`, `parley_engine`, `accesskit_macos`, `accesskit_windows` and
  `tiny-skia-path`. Each has a `.patch` file of 9–32 KB, and `winit` is re-made by
  `scripts/vendor-winit.sh` ([Cargo.toml](https://github.com/anomalyco/rune/blob/dev/Cargo.toml)).
  Consumers must repeat the patches, as Hex does. A fork inherits the job of rebasing six patches
  on every upstream bump.
- **Forkability.** It is agent-oriented by design: `AGENTS.md`, `CLAUDE.md`, headless rendering to
  PNG, an element-tree dump and scripted input. That is the realistic way a two-person fork (Alex
  plus agents) could keep it alive.
  - Pinning a rev is already the supported mode: "Pin a rev of `dev`".
  - A frozen pin keeps building for as long as its pinned toolchain
    ([`rust-toolchain.toml`](https://github.com/anomalyco/rune/blob/dev/rust-toolchain.toml)) and
    crates do.
- **Existing forks.** There are 286
  [forks](https://github.com/anomalyco/rune/forks?include=active&sort_by=stargazers_count), none
  with more than 1 star or an independent life. No community fork exists to fall back on.

## What would change the verdict

**Toward lower risk:**

- OpenCode Desktop (or any part of it) switching from Electron to Rune in `anomalyco/opencode`.
- A public announcement or post by Anomaly about Rune.
- A tagged release.
- More Anomaly engineers in the contributor list.

**Toward higher risk:**

- Hex replacing Rune or freezing its Rune pin for many weeks. Check the pin in
  [Hex's `Cargo.toml`](https://github.com/anomalyco/hex/blob/dev/Cargo.toml).
- Kit Langton's commits moving elsewhere for good.
- Weekly commits falling below ~50 for several weeks. SST's 8 in 30 days is what "quiet" looks like
  at Anomaly.
- New work moving to a private repository.
- The issue tracker being closed or archived.

A cheap monthly check:

```sh
gh api repos/anomalyco/rune/commits?per_page=1
gh api repos/anomalyco/hex/contents/Cargo.toml
```

The second command shows whether Hex is still on a recent Rune pin.
