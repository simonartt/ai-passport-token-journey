<p align="right">
  <a href="CHANGELOG.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Changelog

## 2026-10-01

- Diagnose balance refreshes instead of guessing at them. Screen text only ever said `DIRECT FAIL`, which is true for a DNS failure, an unreachable host, a TLS handshake failure, a rejected certificate, an invalid key and a changed response schema alike. The new `main/we_diag.{h,c}` records a snapshot of every refresh — the stage it reached, and per provider the `esp_err_t`, the mbedTLS error code, the certificate verification flags, the HTTP status, a response-body preview, the attempt count and timing, plus free/minimum heap and the largest free block (a TLS handshake on this PSRAM-less part is bounded by the largest block). A failing row now shows a short tag where the amount would be (`DNS`, `CONN`, `TOUT`, `TLS`, `X509`, `401`, `JSON`, `NOKEY`, `NOSTAT`) and the status line reads `DIRECT FAIL 401`. The portal gained `GET /diag`, a plain-text page behind the same Basic Auth (`no-store`) that prints that snapshot together with the configuration actually in effect — Wi-Fi SSIDs, per-provider key length and last four characters, and an explicit warning when a stored key contains whitespace or control characters. It is linked from the config page so the trace can be copied off the phone.

- API keys are sanitized on save and again immediately before each request: ASCII whitespace and control characters are dropped and a copied-along `Bearer ` prefix is stripped. A key pasted with a trailing newline otherwise travels inside the `Authorization` header, which servers reject as a malformed request — on screen that is indistinguishable from a wrong key. A value that is whitespace only counts as "not filled in" and leaves the stored key alone, matching the incremental save semantics.

- Refresh hardening: when a first pass returns nothing from any provider the fetch set is retried once (`FETCH_TRIES`), the refresh worker stack grew from 6 KB to 8 KB because an mbedTLS handshake on this part is stack-hungry, and SNTP now configures three servers (`CONFIG_LWIP_SNTP_MAX_SERVERS=3`, so `ntp.aliyun.com`, `cn.pool.ntp.org`, `time1.cloud.tencent.com`) because a single unreachable or firewalled NTP host silently leaves the clock unsynced.

- Corrected a wrong claim from the previous entry. ESP-IDF 5.5.3 defaults `CONFIG_MBEDTLS_HAVE_TIME_DATE` to `n`, which means certificate expiry is *not* verified by this build at all — the clock was never the reason a TLS handshake failed. Syncing the clock before the first request is still the right order (the `UPDATED` line, the ledger day boundary, and it doubles as a DNS/UDP reachability probe), but it is not what fixes `DIRECT FAIL`.

- Host tests: added `tests/test_we_diag.c` (error and status tag mapping, snapshot round-trip, overlong-string clamping, tiny/NULL buffers) and extended `tests/test_we_cfg.c` with key sanitizing and incremental key update cases; `tools/validate.sh` runs the new suite.

## 2026-09-05

- **feature/community-skeleton**: community edition of the Token Balance play — SoftAP config portal (`we_portal`), config model (`we_cfg`), provider adapter layer (`we_provider`) with HTTPS direct balance queries for DeepSeek and Kimi, dynamic platform rows and nickname-based lock signature. Host tests added for `we_cfg` and `we_provider`.
- Baseline dashboard firmware untouched by this change: multi-WiFi auto-find, host fetch, lock screen, staged dimming remain as before.

## Unreleased

- Fixed the balance page reporting `DIRECT FAIL` right after provisioning, which had two independent causes. First, saving the portal form rebuilt the configuration from an empty struct instead of merging into the stored one, and the form never pre-fills the API-key or Wi-Fi-password inputs — so submitting the Wi-Fi-only form silently wiped every provider key and the refresh had nothing left to query. Saving is now an incremental merge over the stored config (`we_cfg_set_wifi` / `we_cfg_set_prov_key` / `we_cfg_del_prov`): a blank field keeps the current value, a blank Wi-Fi password reuses the saved password for that SSID, and dropping a platform requires ticking its new remove checkbox. Provider rows now state whether they are configured (showing the last four key characters) and warn that leaving them blank keeps the key, and saving a config with zero provider keys logs a warning. Second, the SNTP wait ran *after* the balance fetch loop, so the first refresh reported no `UPDATED` time and the ledger day boundary was derived from a 1970 clock; the clock is now synced before the first request. (This entry originally blamed the certificate bundle for rejecting every server certificate as not yet valid — that was wrong, see the 2026-10-01 entry.) When no provider key is configured at all the page shows `NO API KEY` instead of the misleading `DIRECT FAIL`.

- Firmware builds now succeed from a clean checkout: `tools/validate.sh --firmware` reconfigures to pull the managed components, re-applies the `esp_lvgl_port` FAP_SCREENSHOT_V1 capture-hook patch (`scripts/patch-esp-lvgl-port.py`), and only then compiles, because `managed_components/` is untracked and a fresh dependency resolve overwrites the patched sources. Added `.github/workflows/build-firmware.yml` and `.github/workflows/static-checks.yml` so a tag push builds and publishes the merged firmware, with `espressif/mdns` pinned to 1.13.1 for reproducible component resolution. The patcher matches anchors by code shape and aborts the build when one is missing, instead of writing a half-patched file — a half-patch still compiles and only surfaces later as an undefined `lvgl_port_display_set_snapshot_cb` at link time.

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
