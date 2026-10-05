# 🧊 UNFREEZE.md — Engineering Protocol for Legacy Issues

> **Status:** 🟢 Active
> **Context:** NGINX Unit is archived. Upstream no longer accepts feature development.
> **Mission:** FreeUnit resumes active development as a community LTS fork.

This document defines the **engineering protocol** for migrating issues from the archived `nginx/unit` repository. It is not a marketing statement; it is a working guideline for maintainers and contributors.

---

## 🎯 Core Principles

FreeUnit provides:
- 🔐 **Security backports** (CVEs, protocol abuse fixes)
- 🐘 **Modern runtime support** (PHP 8.3–8.5+, Python 3.14+, etc.)
- 📦 **Predictable release cadence** (LTS stability)

We do not blindly mirror old issues. We **revalidate** demand.

---

## 1. Priority Model (Strict)

Issues are triaged into 5 tiers. Resources are allocated strictly by priority.

### 🔴 P0 — LTS Integrity (Blockers)
**Must be addressed first. No exceptions.**

| Scope | Examples | Acceptance Criteria |
|-------|----------|---------------------|
| **Build Failures** | PHP 8.5 regressions, toolchain breaks on Ubuntu 24.04/Fedora 42 | Builds cleanly on CI matrix |
| **Crashes / UB** | Segfaults, memory leaks, signal 11 | Reproducible, covered by test, valgrind-clean |
| **Security** | CVEs, HTTP/2 CONTINUATION flood, Java module infinite loop | Patched, embargo respected, backported to LTS |

### 🟠 P1 — Adopt Orphan Demand
**Issues with real users but no upstream maintainer.**

| Scope | Examples | Goal |
|-------|----------|------|
| **Runtime Gaps** | Python 3.14 support, Bun.js runtime | Convert abandoned demand → FreeUnit contributors |
| **Packaging** | Missing `.deb`/`.rpm` for new distros, Docker image drift | Ensure installability on modern OS |
| **Production Breaks** | Features broken in specific envs (e.g., ASGI on macOS) | Restore parity with expected behavior |

### 🟡 P2 — Low-Cost, High-Signal Wins
**Features with existing patches or PoCs.**

| Candidate | Status | Rule |
|-----------|--------|------|
| `REQUEST_URI` preservation (#1615) | Has PoC patch | **Prioritize review over redesign** |
| WAMR integration (#1613) | Has proposal | Validate size/perf benefits |
| Per-app logging (#1610) | Requested | Implement if low overhead |

### 🟢 P3 — DX / Ergonomics
**Only after P0–P2 are stable.**

- Logging improvements (OpenTelemetry spans, granular access logs)
- Routing DSL enhancements
- Config UX improvements

### 🔵 P4 — Strategic Expansion
**Long-term differentiation.**

- WASI 0.2 support
- `njs` as application runtime
- Multi-runtime architecture (Bun/Node parity)

---

## 2. Issue Migration Protocol

### Source of Truth
All upstream ideas originate from [nginx/unit GitHub issues](https://github.com/nginx/unit/issues).

### Rules
1.  **No Blind Copy:** Do NOT mirror issues automatically. Each migrated issue must be rewritten.
2.  **Attribution Required:** Always link the original author:
    > *Originally reported by @username in nginx/unit#XXXX*
3.  **State Revalidation:** Before accepting, verify:
    - Is it still reproducible on FreeUnit?
    - Is it still relevant in 2026 context?
    - Was it solved implicitly by other changes?

### Mandatory Labeling

| Label | Meaning |
|-------|---------|
| `unfreeze` | Migrated from archived Unit |
| `needs-repro` | Missing reproducible case (will be closed if not provided) |
| `has-patch` | Upstream patch exists, needs review/porting |
| `lts-blocker` | Affects stability/security (P0) |
| `runtime` | Language/runtime related (PHP, Python, etc.) |

---

## 3. Required Depth of Discussion

**Superficial issues will be closed.** We require engineering rigor.

### Minimum Bar (ALL required)
1.  **Reproducibility:**
    - Minimal `config.json`
    - Request example (`curl` or HTTP snippet)
    - Exact runtime version (e.g., `PHP 8.4.5`)
2.  **Expected vs Actual:**
    - Clear delta description.
    - *Example:* "Expected: 200 OK with rewritten URI preserved. Actual: Original URI lost."
3.  **Environment Matrix:**
    - OS (distro + version)
    - Runtime
    - FreeUnit version/commit
4.  **Logs / Traces:**
    - Debug logs
    - Stack traces (if crash)
    - *(Future)* OpenTelemetry spans

### Strong Issues (Priority Boost)
- ✅ PoC patch included
- ✅ Failing test case
- ✅ Bisect result
- ✅ Performance regression data

---

## 4. Maintainer Interaction Model

### What We Expect from Authors
- Confirm relevance upon ping.
- Provide updated repro if environment changed.
- Validate fix when delivered.

### What Maintainers Guarantee
- **Response SLA:** Initial triage within **48–72 hours**.
- **Clear Classification:** Every issue gets a P0–P4 label.
- **No Silent Stagnation:** Issues are either moved, closed with reason, or marked `help-wanted`.

---

## 5. Security Handling

**Critical issues MUST NOT go to public issues first.**

- 📩 **Contact:** See [SECURITY.md](SECURITY.md) for encrypted contact info.
- 🛡️ **Policy:** Embargo respected, coordinated disclosure, patch released before announcement.

---

## 6. Definition of “Unfrozen”

An issue is considered **Unfrozen** when:
1.  Reproduced on current FreeUnit main branch.
2.  Triaged (Priority P0–P4 assigned).
3.  Owner assigned OR explicitly marked `help-wanted`.

---

## 7. Immediate Action List (Backlog)

### Batch 1 (Start Here — P0/P1)
- [ ] PHP 8.5 build / runtime validation
- [ ] Ubuntu 24.04 / Fedora 42 packaging fixes
- [ ] `REQUEST_URI` rewrite fix (port existing patch)
- [ ] Per-app logging implementation

### Batch 2 (P1/P2)
- [ ] WAMR integration PoC
- [ ] Python 3.14 support
- [ ] Docker image hardening (security updates)

### Batch 3 (P2/P4)
- [ ] Bun.js runtime exploration
- [ ] njs as runtime evaluation

---

## 8. Anti-Patterns (Hard Rules)

🚫 **Reject issues that:**
- Describe symptoms without config/repro.
- Rely on outdated Unit behavior assumptions (pre-2024).
- Mix multiple unrelated problems (split them).
- Propose redesign without a production use-case.

---

## 9. Positioning

FreeUnit is not a passive fork. It is:
1.  **LTS Maintainer:** Keeping the lights on and secure.
2.  **Runtime Evolution Layer:** Adding support for modern languages.
3.  **Compatibility Bridge:** For existing Unit users who need stability.

---

## 📬 Community & Support

- 💬 **Chat:** [t.me/freeunit_support](https://t.me/freeunit_support)
- 🐛 **Tracker:** [github.com/freeunitorg/freeunit/issues](https://github.com/freeunitorg/freeunit/issues)
- 📖 **Docs:** [freeunit.org](https://freeunit.org)

*Last updated: 2026-04-26*