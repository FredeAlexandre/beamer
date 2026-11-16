# Rune is the UI toolkit of the Rust rewrite, with GPUI as the fallback

The Rust rewrite draws its windows, tray menus and click-through overlays with
[Rune](https://github.com/anomalyco/rune) (Anomaly, MIT, git only, no releases), a toolkit that was
about 3½ weeks old when chosen (2026-11-16). Rune, GPUI and Slint each built the same slice on the
Mac and the PC, and they felt the same to use. Rune was the only one whose per-display, click-through,
all-Spaces overlays come from its own API, with no platform code. Unstable APIs and refactors were
accepted as the price. What would disqualify a toolkit is abandonment, and Rune's risk of that was
judged low because Anomaly's own shipping app, Hex, is built on it alone.

## Consequences

- **GPUI is the fallback**, and there is deliberately **no abstraction layer over the toolkit**.
  Readiness comes from separation instead: crossing logic, protocol, settings model and OS platform
  code stay out of the UI crate, so a switch rewrites only that crate. Don't add a toolkit-neutral UI
  layer "to make switching easy". Don't let non-drawing logic creep into the UI crate either.
- **Rune's gaps are ours to fix**, because upstream largely doesn't answer issues:
  - the tray uses `tray-icon`;
  - we query the displays ourselves, because Rune reads them only at startup and the shared GN10
    comes and goes;
  - Windows frame pacing and Windows overlay bugs are fixed, only when they bite, as patches on top
    of the pinned Rune version.
- **Strings go through our own `tr()`**, not Rune's i18n crate, so they don't depend on the toolkit.
  Accessibility uses Rune's AccessKit as is.
- **Rune is pinned to one dev commit**, together with the crates it patches. The pin moves only by
  hand during the port. Once the app is declared stable, a weekly agent follows the Rune version
  Hex pins and opens a PR.
- **A monthly agent check flags Rune going quiet**: it is archived or made private; fewer than 50
  commits a week for 4 weeks; or Hex drops Rune or keeps its Rune version for 8 weeks. It reopens
  this decision rather than acting on its own.

Sources: the wayfinder tickets "UI prototype bake-off between the top Rust toolkits" (#12), "Will
Rune still be maintained?" (#19) and "Choose the Rust UI toolkit" (#13) on FredeAlexandre/beamer;
the prototypes on branch `prototype/rust-ui-bakeoff`; the findings on `research/rune-viability`.
