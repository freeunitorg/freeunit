# STRAGGLER.PLAN.md — GitHub Hunt Plan

**Goal:** Identify, catalog, and outreach to NGINX Unit stragglers — developers who fork/use the old `nginx/unit` repo instead of `freeunitorg/freeunit`, or run outdated versions unaware of the FreeUnit fork.

**Branch:** `straggler-hunt`
**Date:** 2026-05-30
**Owner:** FreeUnit Community

---

## 1. Definition of "Straggler"

A **straggler** is any GitHub user or organization that meets at least one of the following criteria:

| # | Criterion | Why it matters |
|---|-----------|---------------|
| S1 | Forks `nginx/unit` (official NGINX repo) | Actively developing on old codebase |
| S2 | Forks `freeunitorg/freeunit` but never pushed | Aware of fork but not contributing |
| S3 | Has a repo named `unit`, `nginx-unit`, or similar with C code | May be a custom fork/rebrand |
| S4 | Last commit on `nginx/unit`-based fork is before 2026-04 | Abandoned or unaware of FreeUnit |
| S5 | Repo references `packages.nginx.org` instead of `packages.freeunit.org` | Explicitly still on old infrastructure |
| S6 | Repo description says "fork of NGINX Unit" without mentioning FreeUnit | Branding not updated |

---

## 2. Data Collection

### 2.1 Forks of `nginx/unit` (source stragglers)

```bash
# List all GitHub forks of nginx/unit
gh api repos/nginx/unit/forks?per_page=100 -X GET \
  --jq '.[] | {owner: .owner.login, name: .name, stars: .stargazers_count, pushed: .pushed_at, desc: .description}'
```

**Filter:** `pushed_at` after 2025-01-01 (active development) AND `pushed_at` before 2026-04-01 (missed FreeUnit fork announcement).

### 2.2 Forks of `freeunitorg/freeunit` (dormant adopters)

```bash
# List all forks of freeunitorg/freeunit
gh api repos/freeunitorg/freeunit/forks?per_page=100 -X GET \
  --jq '.[] | {owner: .owner.login, name: .name, stars: .stargazers_count, pushed: .pushed_at, desc: .description}'
```

**Filter:** `pushed_at` is NULL or older than 90 days — aware of FreeUnit but not active.

### 2.3 Keyword search (lost stragglers)

```bash
# Repos mentioning "nginx unit" that are not from freeunitorg or nginx
gh search repos "nginx unit" --json name,owner,description,updatedAt,stargazersCount \
  --limit 200 | jq -r '.[] | select(.owner.login != "freeunitorg" and .owner.login != "nginx") | ...'
```

### 2.4 Version fingerprinting

For each candidate repo, check the `version` file or `configure` script:

```bash
# Clone candidate repo and check version
git clone --depth 1 https://github.com/{owner}/{repo}.git
grep -E "NXT_VERSION|1\.35|1\.34" {repo}/version 2>/dev/null
grep -E "packages\.nginx\.org" {repo}/* 2>/dev/null
```

**Flag as straggler if:**
- Version < 1.35.0 (pre-FreeUnit fork)
- References `packages.nginx.org`
- No reference to `freeunit.org` in README or docs

---

## 3. Comparison Matrix: Our Current State vs Straggler State

| Signal | FreeUnit `master` (1.35.5) | Straggler Threshold |
|--------|---------------------------|---------------------|
| `NXT_VERSION` | `1.35.5` | `< 1.35.0` |
| `packages.nginx.org` | ❌ Removed | ✅ Present |
| `packages.freeunit.org` | ✅ Present | ❌ Missing |
| `freeunit.org` reference | ✅ Present | ❌ Missing |
| PHP 8.5 TrueAsync support | ✅ (1.35.2+) | ❌ Missing |
| chunked_transform setting | ✅ (1.35.5) | ❌ Missing |
| OpenTelemetry support | ✅ (--otel) | ❌ Missing |
| WASI 0.2 support | ✅ | ❌ Missing |
| GitHub org | `freeunitorg` | `nginx` or personal |
| clang-ast plugin | ✅ | ❌ |

---

## 4. Hunt Phases

### Phase 1 — Automated scan (this branch)
- [ ] Run all queries from §2
- [ ] Build candidate table in `STRAGGLER.TABLE.md`
- [ ] Flag S1–S6 stragglers

### Phase 2 — Manual verification
- [ ] Visit each candidate repo
- [ ] Confirm `version` file / `configure` script
- [ ] Check last commit date
- [ ] Check if straggler has any issues/Discussions about Unit

### Phase 3 — Outreach
- [ ] Open issues on straggler repos (template: "FreeUnit fork exists — here's why you might want it")
- [ ] Comment on relevant Discussions/Issues
- [ ] Engage in community spaces (Telegram, Discussions)

### Phase 4 — Monitor
- [ ] Re-scan every 30 days
- [ ] Track straggler → FreeUnit migrations

---

## 5. Outreach Template

> **Subject:** Heads up: NGINX Unit is now FreeUnit (community fork)
>
> Hi {owner},
>
> We noticed your repo {repo_url} is based on `nginx/unit`. Just wanted to let you know that the community has forked Unit as **FreeUnit** — an LTS fork with:
>
> - PHP 8.4+/8.5+ support (TrueAsync)
> - OpenTelemetry (`--otel`)
> - WASI 0.2 components
> - RFC 9112 chunked encoding support
> - Active community at https://freeunit.org
>
> Migration is simple: swap `nginx/unit` → `freeunitorg/freeunit` and update your package source to `packages.freeunit.org`.
>
> Questions? Drop by https://github.com/freeunitorg/freeunit/discussions or https://t.me/freeunit_support.

---

## 6. Straggler Candidates Found (verified — 2026-05-30)

### 🔴 Active / Archived Stragglers (forked from nginx/unit, not FreeUnit)

| Owner/Repo | Stars | Last Push | Version | Parent | Flags | Notes |
|------------|-------|-----------|---------|--------|-------|-------|
| `EdmondDantes/nginx-unit` | 6 | 2025-12-06 | 1.35.0 | `nginx/unit` | S1, S4 | ⚠️ **Archived**. Forked from `nginx/unit` at v1.35.0, homepage points to `unit.nginx.org`, no FreeUnit reference. Missed our April 2026 fork. |
| `mar0x/unit` | 9 | 2022-04-12 | 1.27.0 | `nginx/unit` | S3, S4 | 🚫 **Abandoned**. Default branch is `aspnet` (not `master`). Says "Please use original NGINX Unit, not the cover band." 4 years behind. |

### 🟡 Dormant FreeUnit Forks (aware of FreeUnit, not contributing)

| Owner/Repo | Stars | Last Push | Version | Parent | Notes |
|------------|-------|-----------|---------|--------|-------|
| `sertonix/freeunit` | 0 | 2026-05-26 | 1.35.4 | `freeunitorg/freeunit` | Active homepage → freeunit.org, but no stars, no community engagement |
| `Threedaws/freeunit` | 0 | 2026-05-08 | 1.35.4 | `freeunitorg/freeunit` | Same as above |
| `Pavlusha311245/freeunit` | 0 | 2026-05-24 | 1.35.4 | `freeunitorg/freeunit` | Same as above |

### Feature Gap: FreeUnit 1.35.5 vs straggler versions

| Feature | FreeUnit master (1.35.5) | EdmondDantes 1.35.0 | mar0x 1.27.0 | andrey-tech 1.35.0 |
|---------|:-:|:-:|:-:|:-:|
| NXT_VERSION | **1.35.5** | 1.35.0 | 1.27.0 | 1.35.0 |
| Copyright | FreeUnit Community | NGINX, Inc. | NGINX, Inc. | NGINX, Inc. |
| PHP 8.5 TrueAsync | ✅ | ❌ | ❌ | ❌ |
| OpenTelemetry | ✅ | ❌ | ❌ | ❌ |
| WASI 0.2 | ✅ | ❌ | ❌ | ❌ |
| chunked_transform | ✅ | ❌ | ❌ | ❌ |
| packages.freeunit.org | ✅ | ❌ (nginx.org) | ❌ | ❌ |
| freeunit.org reference | ✅ | ❌ | ❌ | ❌ |
| PHP 8.4+ support | ✅ | ❌ | ❌ | ❌ |
| `pre_request_init` guard | ✅ (`NXT_PHP_PRE_REQUEST_INIT`) | ❌ | ❌ | ⚠️ Partial (his own fix on branch) |

### ⭐ VIP Targets (former contributors, high-value stragglers)

| Owner/Repo | Stars | Last Push | Version | Parent | Why VIP |
|------------|-------|-----------|---------|--------|---------|
| `andrey-tech/nginx-unit` | 0 | 2026-02-12 | 1.35.0 | `nginx/unit` | 🎯 **Andrew Clayton** — ex-NGINX/F5 developer, original author of PHP 8.5 `pre_request_init` SAPI callback (nginx/unit#1660). His commit `5bd8813` on `1.35.0-php8.5` branch fixes `pre_request_init` — same fix FreeUnit backported. Currently **archiving his fork** ("Archiving NGINX Unit repository"). Has `nginx-unit-log-analyzer-php` tool. 25+ yrs PHP/backend experience, Stockholm. Perfect FreeUnit contributor candidate. |

### 🔍 Notable Related Repos (ecosystem stragglers)

| Owner/Repo | Stars | Last Push | Notes |
|------------|-------|-----------|-------|
| `lolgab/snunit` | 149 | 2026-04-05 | Scala Native HTTP server based on NGINX Unit — high-value straggler, active development |
| `Pavlusha311245/nginx-unit-php-sdk` | 0 | 2025-10-22 | PHP SDK for nginx unit — may need FreeUnit migration guide |
| `N0rthernL1ghts/wordpress` | 19 | 2026-02-03 | WordPress Docker + s6 supervised nginx unit — popular, actively maintained |
| `dejonghe/nginx_unit_examples` | 4 | 2026-05-05 | NGINX Unit config testing — very recent, likely unaware of FreeUnit |
| `N0rthernL1ghts/unit-php` | 4 | 2025-12-14 | Alpine Docker image for nginx-unit with PHP |
| `aknot242/nginx-unit-demo` | 0 | 2026-03-29 | Multi-app demo — very recent, straggler |

---

## 7. Methodology: Detecting Unique Commits

For each candidate fork, we compare against two baselines:

```bash
# Get all commits unique to fork (not in nginx/unit master)
gh api "repos/{owner}/{fork}/compare/nginx/unit/master...{owner}/{fork}/master"

# Get recent commits for manual inspection
gh api "repos/{owner}/{fork}/commits?per_page=10" --jq '.[] | {sha, message, author, date}'
```

**Exclusion criteria:**
- SHA `76b39e92` = "Archiving NGINX Unit repository" → mirror/snapshot, exclude
- `andypost/unit` → parent is `freeunitorg/freeunit`, not nginx/unit → exclude from this section
- Commits by Andrew Clayton, Alejandro Colomar, Alex Crichton that are NOT in nginx/unit master → **high-value backport targets**

---

## 8. Unique Commit Targets Found (2026-05-30)

| # | Repo | Author | Unique Commits | Priority |
|---|------|--------|----------------|----------|
| 13 | `bright-spark/nginx-unit` | Andrew Clayton | HTTP TSTR validation for rewrite option | 🔴 High |
| 14 | `ricardochi095/unit` | Andrew Clayton | njs 0.9.0 API, zlib/zstd/brotli CI, compression error handling | 🔴 High |
| 15 | `z-order/nginx-unit` | Alejandro Colomar | Node.js httpVersion fix, setup-unit improvements | 🔴 High |
| 16 | `brianng12/nginx-unit` | Alejandro Colomar | setup-unit ctl edit, restart flags | 🟡 Medium |
| 17 | `andrey-zelenkov/unit` | andrey-zelenkov | Docker slim python images | 🟡 Medium |
| 18 | `alejandro-colomar/unit` | A. Clayton / A. Colomar | EXTRA_CFLAGS, make help, disable -Werror | 🟡 Medium |
| 19 | `callahad/unit-tmp` | Andrew Clayton | CI unitctl release tags, wasm env inheritance | 🔴 High |
| 20 | `seanpm2001/NGINX_Unit` | Andrew Clayton | .mailmap, Go Homebrew paths | 🟡 Medium |
| 21 | `andreivasiliu/unit` | Alejandro Colomar | Deprecated options backport, .mailmap | 🟡 Medium |
| 22 | `alexcrichton/unit` | Alex Crichton | Unit 1.31.1 release, .mailmap | 🟡 Medium |
| 23 | `classicvalues/unit0` | classicvalues | Version bump, response_headers tests | 🟡 Medium |

### Verified: Real patches worth backporting (2026-05-30)

Unique commits by third-party developers, confirmed **absent from nginx/unit upstream**:

| # | Repo | Author | Commit | What it does | Priority |
|---|------|--------|--------|--------------|----------|
| 25 | `mtrop-godaddy/unit` | Matej Trop (Pagely) | `81b67cc` | Terminate PHP worker via SIGKILL if app timeout is reached — fixes PHP signal handler override issue | 🔴 High |
| 26 | `strophy/unit` | Leon White | `da54bd2` | Fix `-Wstring-compare` / `NXT_NONSTRING` truncation warning in `nxt_http_parse.c` | 🟡 Medium |
| 27 | `osokin/unit` | Sergey A. Osokin | `1c394e1` | Fix build with php85-beta2 — guard `disable_classes` with `NXT_PHP_PRE_REQUEST_INIT` | 🔴 High |

### 🗑️ Junk / Empty Forks (no unique owner commits)

Forks where the owner **never made any commits** — purely cosmetic clones, zero development value:

| # | Owner/Repo | ⭐ | Last Push | Version | Owner Commits | Assessment |
|---|------------|---|-----------|---------|---------------|------------|
| 24 | `YuvrajKaushal/unitsss` | 0 | 2026-02-17 | 1.34.0 | **0** | 🗑️ All commits by Andrew Clayton / Ava Hahn (F5/NGINX). Owner has 39 repos but 0 commits here. |

**Total unique commit targets:** 11 repos, 20+ unique patches

---

## 9. Anti-patterns (do not hunt these)

| Pattern | Reason to skip |
|---------|---------------|
| `nginx/unit` official | This is upstream, not a straggler |
| `freeunitorg/*` official org | This is us |
| Repos named `unit` by `nginx` org | Official repos |
| Ancient mirrors (last push 2018–2020) | Abandoned, no outreach value |
| Docker images / configs only (no C code) | Users, not developers — different funnel |
| `andypost/unit` | FreeUnit fork (parent: freeunitorg/freeunit) |
| SHA `76b39e92` repos | Mirrors of nginx/unit "Archiving" commit |
| **Junk/empty forks** | Owner never made any commits — cosmetic clones only (see §8b) |

---

## 10. Success Metrics

| Metric | Target |
|--------|--------|
| Stragglers identified | 50+ |
| Unique commit backports to FreeUnit | 10+ |
| Outreach issues opened | 30+ |
| Straggler → FreeUnit migration PRs | 5+ |
| Junk/empty forks identified | 10+ |
| New FreeUnit community members | TBD |

---

## Outreach Template
