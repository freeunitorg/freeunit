# STRAGGLER.TABLE.md — Database of NGINX Unit Stragglers

**Updated:** 2026-05-30 (revised — 26 SHAs verified in both repos, no unique backports)
**Source:** `STRAGGLER.PLAN.md` hunt on `straggler-hunt` branch
**Status:** 🟢 Active hunting — 148 nginx/unit forks scanned (2023-01-01 → present)

---

## 🔴 Critical: Forked from nginx/unit (missed FreeUnit fork)

| # | Owner/Repo | ⭐ | Last Push | Version | Parent | Status | Outreach |
|---|------------|---|-----------|---------|--------|--------|----------|
| 1 | [EdmondDantes/nginx-unit](https://github.com/EdmondDantes/nginx-unit) | 6 | 2025-12-06 | 1.35.0 | `nginx/unit` | ⚠️ **Archived** | ⬜ Not started |
| 2 | [mar0x/unit](https://github.com/mar0x/unit) | 9 | 2022-04-12 | 1.27.0 | `nginx/unit` | 🚫 **Abandoned** | ⬜ Not started |

---

## 🟡 Dormant FreeUnit forks (aware, not contributing)

| # | Owner/Repo | ⭐ | Last Push | Version | Parent | Homepage | Notes |
|---|------------|---|-----------|---------|--------|----------|-------|
| 3 | [sertonix/freeunit](https://github.com/sertonix/freeunit) | 0 | 2026-05-26 | 1.35.4 | `freeunitorg/freeunit` | freeunit.org | Active fork, low engagement |
| 4 | [Threedaws/freeunit](https://github.com/Threedaws/freeunit) | 0 | 2026-05-08 | 1.35.4 | `freeunitorg/freeunit` | freeunit.org | Same as above |
| 5 | [Pavlusha311245/freeunit](https://github.com/Pavlusha311245/freeunit) | 0 | 2026-05-24 | 1.35.4 | `freeunitorg/freeunit` | freeunit.org | Also owns `nginx-unit-php-sdk` |

---

## ⭐ VIP Targets (high-value stragglers — former contributors / active developers)

| # | Owner/Repo | ⭐ | Last Push | Version | Parent | Status | Notes |
|---|------------|---|-----------|---------|--------|--------|-------|
| 12 | [andrey-tech/nginx-unit](https://github.com/andrey-tech/nginx-unit) | 0 | 2026-02-12 | 1.35.0 | `nginx/unit` | 🔴 **Archived by owner** | 🎯 **High-value target**. **Andrew Clayton** — ex-NGINX developer, original author of PHP 8.5 `pre_request_init` SAPI callback (commit `ad736f9`, closes nginx/unit#1660). He is now **archiving his fork** (commit message: "Archiving NGINX Unit repository"). Previously worked at NGINX/F5, Stockholm, 25+ yrs backend/PHP experience. Has `nginx-unit-log-analyzer-php` tool. His own fork commit `5bd8813` fixes `pre_request_init` — same fix FreeUnit backported. Perfect candidate for FreeUnit contributor. |

---

## 🔥 NGINX Contributor Commits (shared history — not unique to these forks)

All commits below are from the **shared history** between nginx/unit and freeunitorg/freeunit. They exist in both repos. These forks are stragglers because: (a) the owner never made any commits, or (b) the fork is archived/stale, missing FreeUnit. None of these SHAs are "unique backport targets" — they are already in FreeUnit.

| # | Owner/Repo | ⭐ | Last Push | SHA | Author | What it does | Owner Commits |
|---|------------|---|-----------|-----|--------|--------------|---------------|
| 13 | [bright-spark/nginx-unit](https://github.com/bright-spark/nginx-unit) | 1 | 2024-01-05 | `49aee676` | Andrew Clayton | HTTP: TSTR validation flag for rewrite option | ❌ Owner never pushed |
| 14 | [ricardochi095/unit](https://github.com/ricardochi095/unit) | 1 | 2025-05-14 | `7e1f4a7f`, `0a89e837`, `68ae5b52` | Andrew Clayton | njs 0.9.0 API; CI --zlib --zstd --brotli; auto/compression error out | ❌ Owner never pushed |
| 15 | [z-order/nginx-unit](https://github.com/z-order/nginx-unit) | 1 | 2024-01-29 | `6452ca11`, `ba56e50e` | Alejandro Colomar | Node.js httpVersion fix; setup-unit -hh help | ❌ Owner never pushed |
| 16 | [brianng12/nginx-unit](https://github.com/brianng12/nginx-unit) | 0 | 2023-07-01 | `543d478e`, `d73526d2` | Alejandro Colomar | setup-unit ctl "edit"; restart -l/-s flags | ❌ Owner never pushed |
| 17 | [andrey-zelenkov/unit](https://github.com/andrey-zelenkov/unit) | 0 | 2024-08-27 | `8eb5d128`, `4778099b` | andrey-zelenkov | Docker slim python images; leave artifacts on success | ⚠️ 2 commits by owner |
| 18 | [alejandro-colomar/unit](https://github.com/alejandro-colomar/unit) | 0 | 2024-12-16 | `a171b399`, `f55fa70c`, `d23812b8` | A. Clayton / A. Colomar | EXTRA_CFLAGS; make help; disable -Werror | ❌ Owner never pushed |
| 19 | [callahad/unit-tmp](https://github.com/callahad/unit-tmp) | 0 | 2024-09-11 | `5c58f9d0`, `56c237b3` | Andrew Clayton | unitctl release tags; wasm-wc env inheritance | ❌ Owner never pushed |
| 20 | [seanpm2001/NGINX_Unit](https://github.com/seanpm2001/NGINX_Unit) | 1 | 2023-12-06 | `846a7f48`, `9a36de84` | Andrew Clayton | .mailmap Danielle; Go Homebrew paths | ❌ Owner never pushed |
| 21 | [andreivasiliu/unit](https://github.com/andreivasiliu/unit) | 0 | 2023-05-21 | `f32858dc`, `766a565b` | Alejandro Colomar | Deprecated options backport; .mailmap Alex | ❌ Owner never pushed |
| 22 | [alexcrichton/unit](https://github.com/alexcrichton/unit) | 0 | 2023-10-31 | `fb33ec86`, `299e783e` | Alex Crichton | Unit 1.31.1 release; .mailmap Alex | ❌ Owner never pushed |
| 23 | [classicvalues/unit0](https://github.com/classicvalues/unit0) | 2 | 2023-09-04 | `34cf6e77`, `8fae3468` | classicvalues | Version bump; response_headers tests | ⚠️ 2 commits by owner |

> **Verification (2026-05-30):** All 23 SHAs above verified ✅ in both `freeunitorg/freeunit` and `nginx/unit`. None are unique backport targets.

### 🗑️ Junk / Empty Forks

These repos are forks of nginx/unit where the **owner never made any commits** — purely cosmetic clones, zero development value.

| # | Owner/Repo | ⭐ | Last Push | Version | Owner Commits | Assessment |
|---|------------|---|-----------|---------|---------------|------------|
| 24 | [YuvrajKaushal/unitsss](https://github.com/YuvrajKaushal/unitsss) | 0 | 2026-02-17 | 1.34.0 | **0** | 🗑️ **Cosmetic fork only**. All commits by Andrew Clayton / Ava Hahn (F5/NGINX). Owner YuvrajKaushal has 39 repos but made 0 commits here. No development value. |

### ✅ Verified: Real patches worth backporting

These repos have **unique commits by third-party developers** not in nginx/unit upstream — genuine bug fixes / features:

| # | Owner/Repo | ⭐ | Last Push | Commit | Author | What it does |
|---|------------|---|-----------|--------|--------|--------------|
| 25 | [mtrop-godaddy/unit](https://github.com/mtrop-godaddy/unit) | 0 | 2025-08-28 | `81b67cc` | Matej Trop (Pagely) | Terminate PHP worker via SIGKILL if app timeout is reached — fixes PHP signal handler override issue |
| 26 | [strophy/unit](https://github.com/strophy/unit) | 0 | 2025-08-29 | `da54bd2` | Leon White | Fix `-Wstring-compare` / `NXT_NONSTRING` truncation warning in `nxt_http_parse.c` |
| 27 | [osokin/unit](https://github.com/osokin/unit) | 0 | 2025-10-06 | `1c394e1` | Sergey A. Osokin | Fix build with php85-beta2 — guard `disable_classes` / `zend_disable_class` with `NXT_PHP_PRE_REQUEST_INIT` |

> **Verification (2026-05-30):** All 23 SHAs above verified ✅ in both `freeunitorg/freeunit` and `nginx/unit`. None are unique backport targets.

### ✅ Verified: Real patches (also in nginx/unit — shared history)

These repos had commits by **third-party developers** (not NGINX employees). The commits exist in **both** nginx/unit and freeunitorg/freeunit — same shared history, no backport needed. The straggler value is outreach opportunity (see §5), not code.

| # | Owner/Repo | ⭐ | Last Push | Commit | Author | What it does |
|---|------------|---|-----------|--------|--------|--------------|
| 25 | [mtrop-godaddy/unit](https://github.com/mtrop-godaddy/unit) | 0 | 2025-08-28 | `81b67cc` | Matej Trop (Pagely) | PHP worker SIGKILL on timeout |
| 26 | [strophy/unit](https://github.com/strophy/unit) | 0 | 2025-08-29 | `da54bd2` | Leon White | Fix `-Wstring-compare` / `NXT_NONSTRING` warning |
| 27 | [osokin/unit](https://github.com/osokin/unit) | 0 | 2025-10-06 | `1c394e1` | Sergey A. Osokin | Guard `disable_classes` with `NXT_PHP_PRE_REQUEST_INIT` |

> **Verification (2026-05-30):** All 3 SHAs verified ✅ in both `nginx/unit` and `freeunitorg/freeunit`. No backport needed. Outreach value only.

> **Note:** `andypost/unit` excluded — it's a FreeUnit fork (parent: `freeunitorg/freeunit`), not nginx/unit.
> **Note:** `muyideen211/unit`, `ScorpiusDraconis83/unit`, `EdmondDantes/nginx-unit` excluded from this section — they are mirrors/snapshots of nginx/unit (SHA `76b39e92` = "Archiving NGINX Unit repository" commit).

---

## 🔍 Ecosystem stragglers (related tools, integrations)

| # | Owner/Repo | ⭐ | Last Push | Notes | Priority |
|---|------------|---|-----------|-------|----------|
| 6 | [lolgab/snunit](https://github.com/lolgab/snunit) | 149 | 2026-04-05 | Scala Native HTTP server based on NGINX Unit | 🔴 High |
| 7 | [N0rthernL1ghts/wordpress](https://github.com/N0rthernL1ghts/wordpress) | 19 | 2026-02-03 | WordPress Docker + s6 supervised nginx unit | 🔴 High |
| 8 | [Pavlusha311245/nginx-unit-php-sdk](https://github.com/Pavlusha311245/nginx-unit-php-sdk) | 0 | 2025-10-22 | PHP SDK for nginx unit | 🟡 Medium |
| 9 | [dejonghe/nginx_unit_examples](https://github.com/dejonghe/nginx_unit_examples) | 4 | 2026-05-05 | NGINX Unit config testing — updated recently | 🟡 Medium |
| 10 | [N0rthernL1ghts/unit-php](https://github.com/N0rthernL1ghts/unit-php) | 4 | 2025-12-14 | Alpine Docker image for nginx-unit with PHP | 🟡 Medium |
| 11 | [aknot242/nginx-unit-demo](https://github.com/aknot242/nginx-unit-demo) | 0 | 2026-03-29 | Multi-app demo — very recent | 🟡 Medium |

---

## 📋 Outreach Log

| Date | Target | Action | Result |
|------|--------|--------|--------|
| — | — | — | — |

*This table is populated as outreach progresses.*

---

## 📊 Summary Stats

| Category | Count | With Stars | Without Stars |
|----------|-------|-----------|---------------|
| 🔴 Critical (nginx/unit fork) | 2 | 2 | 0 |
| 🟡 Dormant FreeUnit forks | 3 | 0 | 3 |
| ⭐ VIP Targets | 1 | 0 | 1 |
| 🔥 NGINX contributor (shared history) | 11 | 3 | 8 |
| 🗑️ Junk / Empty forks | 1 | 0 | 1 |
| ✅ Real patches (shared history, outreach only) | 3 | 0 | 3 |
| 🔍 Ecosystem stragglers | 6 | 3 | 3 |
| **Total** | **27** | **8** | **19** |

**Note:** 26 SHAs across #13-27 verified in both `nginx/unit` and `freeunitorg/freeunit`. Zero unique backport targets identified. Straggler value = outreach only.

---

## 🔗 References

- [STRAGGLER.PLAN.md](STRAGGLER.PLAN.md) — hunt strategy, anti-patterns, metrics
- [straggler metaphor](straggler_metaphor.md) — why "straggler"
- [freeunitorg/freeunit](https://github.com/freeunitorg/freeunit) — our repo
