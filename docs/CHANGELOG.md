<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## Unreleased

- Added idle dimming to the Mini Golf play: after `GOLF_IDLE_DIM_MS` (60 s) without a key the backlight drops to 8%, so a battery-powered wearable is not left showing a bright screen while nobody is playing, and the next key restores full brightness. That waking press is consumed and not forwarded to the play, so "press something to see the screen" cannot fire a shot in the timing-sensitive power phase. The transition logic lives in the dependency-free `main/golf_idle.{h,c}` with host tests, because "report the change on exactly one frame" is easy to get wrong in both directions — reporting every frame re-drives the backlight continuously, and never reporting leaves the screen dark for good.

- Turned the Mini Golf firmware into a single-play build: the BSP demo menu and the seven default demo pages (`Display`, `Button`, `Audio`, `Battery`, `Wi-Fi`, `BLE`, `Low Power`) plus their `demo_radio` helper are gone, and `app_main()` now initializes the board and enters the course directly. The `ui_pixel` theme is kept intact — the play still builds its screen through `ui_pixel_screen_create()` / `ui_pixel_panel_create()` — and `main/demo.h` keeps the `enter` / `exit` / `key` page contract while dropping the now-unused `demo_entry_t` registry. `main` no longer requires `bt`, `esp_wifi`, `esp_netif`, `esp_event`, or `nvs_flash`. The play's key translation is unchanged; the only interaction change is that OK long-press no longer has a meaning, because there is no menu to return to.

- Added a Mini Golf play: three holes with distinct layouts ("Side Step" par 2 with a bunker punishing a high aim and a pillar punishing a flat pull, "Dogleg" par 3 with a short ceiling wall and a corner pond, and "Pond Side" par 3 with a large pond guarding the right approach), a two-phase aim-then-swing control that uses only the three physical keys, and per-terrain rolling friction. The course model, wall and boundary bouncing, cup capture with a lip-out threshold, the water penalty, scoring, and the per-pixel scene/overlay rendering are integer fixed-point and covered by host tests; a host regression test asserts that the default tee-to-cup aim line of every hole is free of water and walls, so a course edit can never silently turn the opening shot into a trap. The key translation lives in a separate host-tested layer (`golf_input`), because the button driver reports `PRESS_DOWN` immediately but `LONG_PRESS_START` only after the long-press threshold, and a `SINGLE_CLICK` may follow on release — so a long aim press used to apply the fine step *and* the coarse step (36° instead of 32°), and the trailing click after a stroke could advance the hole on its own. Sound effects reuse the asynchronous RTTTL player from the Tetris example.

- Added the supplied 80-byte CW2017 profile for the specified 520 mAh cell, including content/update-flag checks, verified writes, the required restart sequence, and bounded SOC-readiness polling.

- Expanded the environment bootstrap document: added Espressif's Git service mirror (`git.espressif.com.cn`) as the preferred mainland-China route for ESP-IDF v5.5.3 and its submodules, documented submodule long-wait/timeout handling, in-place repair, and the pinned-commit shallow fetch for large submodules such as `esp32-wifi-lib`, warned about stale per-repository Jihulab `insteadOf` residue, and added the official offline release archive as a last-resort fallback (learned from `esp-mosaico/esp-mosaico-vibe`).

- Reorganized the documentation by function area with a dual entry point: the root `AGENTS.md` is now a thin router (hard constraints + task routing only) and the detailed AI workflow lives in `docs/development/ai-guide.md`; `agent-guide.md` was folded in. `docs/development/` gained a second level (`engineering/`, `ci/`, `release/`), and the `plays/` application archive and `experiences/` moved into a `docs/reference/` area with a dedicated README. Removed `docs/software-design/` (empty scaffold); folded the three `assets/{fonts,images,music}/README` leaves into the `assets/` README; flattened the six `project-completion` sub-documents into a single file; and unified each directory to a single README, eliminating every `INDEX` file and a duplicated experience index. All cross-references and bibliographic links were updated; no content was dropped.

- Removed the obsolete app/test partition at `0x700000` and its related
  bootloader, validation, and documentation requirements. The fixed protected
  `cardid` partition and its CI checks remain unchanged.
- Documented a release-title convention for multi-app releases: name tags as `v<version>-<app-name>` (e.g. `v0.1.0-voice-keychain`) so the release title carries the version and the app, and confirm the title after the release is published so a release list is scannable by app.
- Added a post-release follow-up workflow: an `issue-suggestions` skill for filing user feedback as issues against the upstream project, an `experience-pr` skill for submitting reusable development experience as a documentation PR, a `docs/experiences/` directory for per-entry experience files, and supporting `project-completion`, `file-issues`, and experience-index documents.
- Simplified the tracked repository root: moved GitHub-recognized community documents into `.github/`, moved the changelog into `docs/`, updated every reference, and added a root-document allowlist to repository checks.
- Repository-wide language policy: every maintained Markdown default `.md` file is English, Simplified Chinese uses a paired `.zh_CN.md`, and both provide language switches. Static checks reject missing peers, missing switches, and Chinese prose in English defaults.
- Phase one of the AI development workflow: streamlined task-based context routing, unified local/CI validation, added PR checks and a template, and committed the dependency lock for reproducible builds.
- PR review fixes: pinned GitHub Actions to full commit SHAs, split build/release jobs by least privilege, disabled persisted sync checkout credentials, added Feature Request and Usage Question forms, clarified private security-report fallback, and corrected stale README, CI-trigger, and branch descriptions.
- Changed commit titles, PR titles, and PR bodies from Chinese-default to English; updated the Chinese punctuation rule so it no longer applies to PR descriptions.
- Reworked `build-firmware.yml` to pass `SDKCONFIG_DEFAULTS=sdkconfig.defaults`, enable `partitions.csv`, preserve the 8 MB image header, merge a flashable `FoloToy-AI-Passport-full.bin`, publish only that artifact, and use Actions cache v5.
- Integrated upstream PR #6 to resolve PR #4 conflicts: Wi-Fi, Bluetooth LE, radio lifecycle, and low-power demos; a 3 MB factory partition; build/menu/configuration updates; hardware-guide coverage; and bilingual capability tables.
- Defined English imperative Conventional Commit formatting for both commits and PR titles.
- Removed stale sync-workflow template comments and generalized an irrelevant Redis TTL rule to cache components.
- Added Chinese punctuation, credential safety, and recoverable file-deletion conventions.
- Expanded source-comment requirements for functions, state, ownership, concurrency, timing, registers, and magic values.
- Removed AI execution instructions from product READMEs so they remain human-facing product and repository overviews.
- Added `docs/development/agent-guide.md` as the focused AI workflow guide.
- Updated `AGENTS.md`, `docs/INDEX.md`, and the development index for the agent guide.
- Documented why the root README path is reserved for fork owners and how GitHub README precedence supports it.
- Created `main-update` from the upstream-aligned baseline and combined the repository-structure, firmware-CI, and upstream-sync work.
- Corrected the merged documentation index, workflow path, project tree, and CI references.
- Moved CI documentation from software design to `docs/development/`.
- Moved fork-only documentation assets from `assets/docs/` to `docs/assets/`.
- Moved the upstream English/Chinese project READMEs under `docs/` and renamed the documentation catalog to `docs/INDEX.md`.
- Initialized `AGENTS.md`, `CLAUDE.md`, and `CHANGELOG.md`.
- Standardized the initial project README language filenames.
- Added the `docs/`, `assets/`, and `skills/` directory structure.
- Moved the upstream hardware guide into `docs/hardware-design/`.
- Standardized subdirectory README capitalization and introduced fork conventions.
- Allowed fork-owned root README and supplemental documentation content on fork `main`.
- Added and documented the fork-only supplemental-document directory.
- Moved the build CI document to its dedicated CI branch before consolidation.
- Documented clean-`main` reasons, the direct-development exception, and Actions enablement for forks.
- Split the original agent rules into contribution, development, and fork documents with a compact root index.
- Updated software-design and project README references for the new documentation structure.
- Added the documentation catalog and task-triggered routing based on the earlier repository model.
- Added bilingual contribution, code-of-conduct, security, and support documents tailored to this ESP-IDF and fork workflow.
