# STRAGGLER.PLAN.2026-05-30.md — Hunt Session Summary

**Date:** 2026-05-30
**Session:** YuvrajKaushal/unitsss branch analysis → full straggler audit (#13-27)
**Branch:** `straggler-hunt`

---

## What was done

### 1. YuvrajKaushal/unitsss — Full Branch Analysis

56 branches checked (all via GitHub API):

| Category | Count | Author | Assessment |
|----------|-------|--------|------------|
| `master` | 1 | dependabot[bot] | wasmtime 24.0.0→24.0.1 — ✅ in FreeUnit |
| `snyk-fix-*` | 55 | snyk-bot | Base image bumps for bookworm — ❌ not in FreeUnit (different trixie base) |

**Key finding:** `snyk-fix-*` branches = automated Snyk security dependency updates (same category as dependabot). All patch bookworm-based Dockerfiles. FreeUnit uses trixie-based images — different OS, different CVE surface. **0 backport value.**

**Owner commits:** 0. All commits from nginx/unit upstream or bots.

Report: `snyk-fix.md`

### 2. All 26 SHAs from STRAGGLER.TABLE.md (#13-27) Verified Against Both Repos

| Repo | SHAs | In freeunitorg/freeunit | In nginx/unit | Backport needed? |
|------|------|------------------------|--------------|------------------|
| bright-spark | 1 | ✅ | ✅ | ❌ |
| ricardochi095 | 3 | ✅ | ✅ | ❌ |
| z-order | 2 | ✅ | ✅ | ❌ |
| brianng12 | 2 | ✅ | ✅ | ❌ |
| andrey-zelenkov | 2 | ✅ | ✅ | ❌ |
| alejandro-colomar | 3 | ✅ | ✅ | ❌ |
| callahad | 2 | ✅ | ✅ | ❌ |
| seanpm2001 | 2 | ✅ | ✅ | ❌ |
| andreivasiliu | 2 | ✅ | ✅ | ❌ |
| alexcrichton | 2 | ✅ | ✅ | ❌ |
| classicvalues | 2 | ✅ | ✅ | ❌ |
| mtrop-godaddy | 1 | ✅ | ✅ | ❌ |
| strophy | 1 | ✅ | ✅ | ❌ |
| osokin | 1 | ✅ | ✅ | ❌ |

**Result:** All 26 SHAs exist in both repos. Zero unique backport targets. "Unique commits" section in STRAGGLER.TABLE.md was wrong — corrected to "NGINX Contributor Commits (shared history)".

### 3. Archived Status & Issue Check

| Repo | Archived | Open Issues | Notes |
|------|----------|-------------|-------|
| andrey-tech/nginx-unit | ✅ Yes | 0 | VIP: Andrew Clayton, archiving fork |
| osokin/unit | ✅ Yes | 0 | Sergey Osokin, archived |
| mtrop-godaddy/unit | ❌ | **1** | Real developer (Pagely), SIGKILL fix |
| Остальные 13 | ❌ | 0 | Cosmetic forks |

### 4. STRAGGLER.TABLE.md Corrected

- "🔥 Unique Commits (not in nginx/unit upstream)" → "🔥 NGINX Contributor Commits (shared history)"
- "✅ Real patches worth backporting" → "✅ Real patches (shared history, outreach only)"
- Added verification notes for all 26 SHAs
- Added snyk-bot = dependabot insight to memory

---

## Key Learnings

1. **snyk-fix branches** = automated, same category as dependabot. Don't count as owner commits.
2. **"unique commits" ≠ truly unique** — must verify against both nginx/unit AND freeunitorg/freeunit. All SHAs from nginx/unit upstream were already in FreeUnit via shared history.
3. **FreeUnit uses trixie (Debian 13)** vs nginx/unit's bookworm (Debian 12). CVE patches for bookworm don't apply to trixie.
4. **FreeUnit base images:** `python:3.12-slim-trixie`, `golang:1.25-trixie`, `php:8.5-cli-trixie` — NOT bookworm.
5. **osokin/unit archived** — Sergey Osokin archived his fork. Potential VIP contact for FreeUnit.

---

## Outreach Priority

### 🔴 High Priority
1. **mtrop-godaddy/unit** — 1 open issue (SIGKILL fix), developer from Pagely. Comment on issue: "implemented in FreeUnit 1.35.x"
2. **andrey-tech/nginx-unit** — Andrew Clayton archived his fork. Outreach to invite as FreeUnit contributor.
3. **osokin/unit** — Sergey Osokin archived. Outreach + potential bug fix collaboration.

### 🟡 Medium Priority
4. **strophy/unit** — Leon White, `-Wstring-compare` fix. Outreach.
5. **bright-spark/nginx-unit** — 1 star, owner never pushed. Outreach.
6. **ricardochi095/unit** — 1 star, owner never pushed. Outreach.

### 🟢 Low Priority
7-16. **Остальные 10** — cosmetic forks (owner 0 commits). Outreach template sufficient.

---

## Files Modified

- `STRAGGLER.TABLE.md` — corrected "unique commits" → "shared history", added verification notes
- `snyk-fix.md` — created, full analysis of 56 branches
- `memory/feedback/snyk_bot_equivalency.md` — created, snyk-fix = dependabot

---

## mtrop-godaddy/unit — Open Issue #1

- **Title:** "Terminate PHP worker if app timeout is reached for a request"
- **State:** open since Aug 2025
- **Comments:** 0
- **Body:** empty/null

This is the SIGKILL fix issue, already implemented in FreeUnit. Owner **Matej Trop** from Pagely opened the issue, committed patch `81b67cc`, but never closed the issue or sent it upstream.

---

## Outreach Priority Summary

| Priority | Repo | Why |
|----------|------|-----|
| 🔴 | **mtrop-godaddy/unit** | 1 open issue, real developer (Pagely), SIGKILL fix |
| 🔴 | **andrey-tech/nginx-unit** | Archived, Andrew Clayton — VIP candidate |
| 🔴 | **osokin/unit** | Archived, Sergey Osokin — real contributor |
| 🟡 | Remaining 13 | Cosmetic forks, outreach template sufficient |

### Next Steps

1. **mtrop-godaddy/unit:** Comment on issue #1 — "implemented in FreeUnit"
2. **andrey-tech/nginx-unit:** Open outreach issue — invite Andrew Clayton as contributor
3. **osokin/unit:** Open outreach issue — Sergey Osokin
4. **Remaining 13:** Open outreach issues using template
5. **All 16:** Update outreach log in STRAGGLER.TABLE.md