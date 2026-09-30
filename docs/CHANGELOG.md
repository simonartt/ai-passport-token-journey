<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## 2026-09-05

- **feature/community-skeleton**: community edition of the Token Balance play — SoftAP config portal (`we_portal`), config model (`we_cfg`), provider adapter layer (`we_provider`) with HTTPS direct balance queries for DeepSeek and Kimi, dynamic platform rows and nickname-based lock signature. Host tests added for `we_cfg` and `we_provider`.
- Baseline dashboard firmware untouched by this change: multi-WiFi auto-find, host fetch, lock screen, staged dimming remain as before.

## Unreleased

- Firmware builds now succeed from a clean checkout: `tools/validate.sh --firmware` reconfigures to pull the managed components, re-applies the `esp_lvgl_port` FAP_SCREENSHOT_V1 capture-hook patch (`scripts/patch-esp-lvgl-port.py`), and only then compiles, because `managed_components/` is untracked and a fresh dependency resolve overwrites the patched sources. Added `.github/workflows/build-firmware.yml` and `.github/workflows/static-checks.yml` so a tag push builds and publishes the merged firmware, with `espressif/mdns` pinned to 1.13.1 for reproducible component resolution.

- Configuration and ledger backup/restore over the portal: `GET /backup` downloads one JSON document holding the whole `we_cfg` blob (nickname, Wi-Fi profiles, provider API keys) plus the `balhist` ledger (per-provider daily baselines, 31 days), and `POST /restore` writes both back to NVS and reboots. The two blobs are written together on purpose — `hist_check_rows()` wipes the whole ledger whenever the provider order stops matching `row_id`, so restoring keys alone would silently drop the heatmap. The ledger data model moved to the new shared header `main/we_hist.h` so the added `main/we_backup.c` can read that NVS blob without reaching into `app_balance.c` internals. Wi-Fi and API-key fields that do not fit their slot are rejected instead of truncated, a rejected restore leaves NVS untouched, balances are rounded to four decimals on export so the file stays readable, and `days` is emitted in ascending date order. The config page gained a backup section (download link plus file upload) and the HTTP server stack grew to 8 KB.

- Config portal now runs on the local network instead of a captive AP: the device first tries the saved WiFi profiles as a station and shows the LAN address plus an mDNS name (`http://token-journey.local/`) on screen; it only falls back to the `BAL-XXXX` SoftAP when every profile fails or nothing is configured yet, so a brand-new device can still be provisioned. The portal now requires HTTP Basic Auth (`admin` / `12345678`) because a LAN-reachable page can rewrite configuration, and it closes itself after five idle minutes so the WiFi radio is not held on (a station link costs roughly 80-100 mA). Added the `espressif/mdns` component, made `demo_radio_network_prepare()` tolerate `ESP_ERR_INVALID_STATE` so the portal and the balance refresh can share the network stack, and let the balance refresh skip a cycle while the portal owns the radio.

- Token Journey dashboard layout tuning: the journey-page battery pill moved to `174,11` and shrank to `50×19` (corner radius is now derived as `h/2`), the two legend labels became independently positioned (`HIGH` at `15,283`, `LOW` at `194,283`), and `bat_pill_create()` now takes explicit geometry per call so the balance page keeps `184,8,50×22`. Pill and legend geometry are now named `BAT_PILL_*` / `LEGEND_*` macros, enabling round-trips with the visual layout editor.

- Made mini-program BLE install compatibility a template-level invariant: fixed
  protected `cardid`/Recovery partitions, retained the five-second UP-key
  Recovery boot hook, and added CI validation for merged-image structure,
  partition MD5/ranges, the 3 MB app limit, and protected payload exclusion.
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
