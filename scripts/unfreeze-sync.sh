#!/bin/sh
#
# unfreeze-sync.sh — migrate relevant nginx/unit issues to freeunitorg/freeunit
#
# Prerequisites:
#   ./unfreeze-labels.sh   # run once to import nginx/unit labels
#
# Usage:
#   ./unfreeze-sync.sh [LIMIT]          # dry-run (default)
#   ./unfreeze-sync.sh --preview 50    # preview first 50 issues
#   DRY_RUN=false ./unfreeze-sync.sh    # actually create issues

set -eu

# pipefail is not in POSIX; use it when the shell supports it.
# shellcheck disable=SC3040
if (set -o pipefail) 2>/dev/null; then
  set -o pipefail
fi

UPSTREAM_REPO="nginx/unit"
TARGET_REPO="freeunitorg/freeunit"

# keywords for filtering (extend as needed)
KEYWORDS="
  php
  crash
  segfault
  wasm
  wamr
  bun
  python
  docker
  fedora
  ubuntu
  logging
  rewrite
"

# ported labels check — must match labels imported by unfreeze-labels.sh
# GitHub API returns full names with emoji, e.g. "z-bug 🐞", "z-enhancement ⬆️"
is_known_label() {
  case "$1" in
    "z-bug 🐞" | \
    "z-enhancement ⬆️" | \
    "z-crasher" | \
    "z-configuration" | \
    "z-c" | \
    "z-community" | \
    "z-documentation-update-needed" | \
    "z-Difficulty: ⭐️" | \
    "z-Difficulty: ⭐️ ⭐️" | \
    "z-Difficulty: ⭐️ ⭐️ ⭐️" | \
    "z-duplicate" | \
    "z-go" | \
    "z-infrastructure" | \
    "z-invalid" | \
    "z-java" | \
    "z-needs-investigation" | \
    "z-node-js" | \
    "z-notabug" | \
    "z-packages" | \
    "z-php" | \
    "z-python" | \
    "z-question" | \
    "z-ready4dev 🚀" | \
    "z-roadmap" | \
    "z-ruby" | \
    "z-rust" | \
    "z-toolchain" | \
    "z-wasm" | \
    "z-wontfix" | \
    "L-Go" | \
    "L-Java" | \
    "L-JavaScript" | \
    "L-Libunit" | \
    "L-Perl" | \
    "L-PHP" | \
    "L-Python" | \
    "L-Ruby" | \
    "L-WebAssembly" | \
    "T-Defect" | \
    "T-Enhancement" | \
    "T-Other" | \
    "X-Needs-Discussion" | \
    "X-Needs-Info" | \
    "X-Release-Blocker")
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

# --- args ---
DRY_RUN="${DRY_RUN:-true}"

LIMIT=5
while [ $# -gt 0 ]; do
  case "$1" in
    --preview)
      DRY_RUN="true"
      shift
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "ERROR: unknown option: $1" >&2
      exit 1
      ;;
    *)
      LIMIT="$1"
      shift
      ;;
  esac
done

echo "== FreeUnit UNFREEZE sync =="
echo "Upstream: $UPSTREAM_REPO"
echo "Target:   $TARGET_REPO"
echo "Limit:    $LIMIT"
echo "Dry-run:  $DRY_RUN"
echo

# --- check gh ---
if ! command -v gh >/dev/null 2>&1; then
  echo "ERROR: gh CLI not installed" >&2
  exit 1
fi

gh auth status >/dev/null 2>&1 || {
  echo "ERROR: gh not authenticated" >&2
  exit 1
}

# --- keyword filter ---
matches_keywords() {
  text="$1"
  for kw in $KEYWORDS; do
    if printf '%s\n' "$text" | grep -i -e "$kw" >/dev/null; then
      return 0
    fi
  done
  return 1
}

# --- create issue helper with labels ---
create_issue_with_labels() {
  target_repo="$1"
  draft_title="$2"
  new_issue_body="$3"
  shift 3

  # In POSIX sh, convert label list into --label "$lbl" arguments
  n_labels=$#
  while [ "$n_labels" -gt 0 ]; do
    set -- "$@" --label "$1"
    shift
    n_labels=$((n_labels - 1))
  done

  gh issue create \
    --repo "$target_repo" \
    --title "$draft_title" \
    --body "$new_issue_body" \
    "$@"
}

# --- fetch existing unfreeze issue titles to detect duplicates ---
echo "Fetching existing unfreeze issues from $TARGET_REPO..."
EXISTING_TITLES=$(gh issue list \
  --repo "$TARGET_REPO" \
  --label "unfreeze" \
  --state all \
  --limit 9999 \
  --json title \
  --jq '.[].title')

# --- fetch issues ---
echo "Fetching issues from $UPSTREAM_REPO..."

ISSUES_JSON=$(gh issue list \
  --repo "$UPSTREAM_REPO" \
  --state open \
  --limit "$LIMIT" \
  --json number,title,body,author,url,state,createdAt,labels)

COUNT=$(printf '%s\n' "$ISSUES_JSON" | jq length)
echo "Fetched $COUNT issues"
echo

# --- process ---
printf '%s\n' "$ISSUES_JSON" | jq -c '.[]' | while read -r issue; do
  NUMBER=$(printf '%s\n' "$issue" | jq -r '.number')
  TITLE=$(printf '%s\n' "$issue" | jq -r '.title')
  BODY=$(printf '%s\n' "$issue" | jq -r '.body // ""')
  AUTHOR=$(printf '%s\n' "$issue" | jq -r '.author.login')
  URL=$(printf '%s\n' "$issue" | jq -r '.url')
  STATE=$(printf '%s\n' "$issue" | jq -r '.state')
  CREATED=$(printf '%s\n' "$issue" | jq -r '.createdAt | split("T")[0]')
  UPSTREAM_LABELS_JSON=$(printf '%s\n' "$issue" | jq -r '[.labels[] | .name] | join("\n")')

  TEXT="$TITLE"
  if [ -n "$BODY" ]; then
    TEXT="${TEXT}
${BODY}"
  fi

  if ! matches_keywords "$TEXT"; then
    continue
  fi

  DRAFT_TITLE="[unfreeze] $TITLE"

  # --- dedup check (exact line match against "[unfreeze] title") ---
  if printf '%s\n' "$EXISTING_TITLES" | grep -xF "$DRAFT_TITLE" >/dev/null; then
    echo "SKIP (exists): #$NUMBER $TITLE"
    continue
  fi

  echo "----"
  echo "#$NUMBER [$STATE] $TITLE"
  echo "Created: $CREATED"
  echo "Author: @$AUTHOR"
  echo "URL: $URL"
  echo "Upstream labels: $UPSTREAM_LABELS_JSON"

  # --- build label list for new issue ---
  set -- "unfreeze" "needs-repro"
  if [ -n "$UPSTREAM_LABELS_JSON" ]; then
    while IFS= read -r ul || [ -n "$ul" ]; do
      [ -z "$ul" ] && continue
      if is_known_label "$ul"; then
        set -- "$@" "$ul"
      fi
    done <<EOF
$UPSTREAM_LABELS_JSON
EOF
  fi

  # --- upstream comment ---
  # shellcheck disable=SC2034
  COMMENT_BODY=$(cat <<EOF
Hi @$AUTHOR,

The original Unit repository has been archived.

We are continuing development as a community LTS fork:
https://github.com/$TARGET_REPO

Your issue is still relevant:
$TITLE

We'd like to revive it in FreeUnit.

Could you please re-open or restate it here:
https://github.com/$TARGET_REPO/issues

If you still have a reproducible case or context, it would greatly help prioritization.

— FreeUnit maintainers
EOF
)

  # --- escape body for markdown safe embedding ---
  if [ -n "$BODY" ]; then
    # shellcheck disable=SC2016
    BODY_ESCAPED=$(printf '%s' "$BODY" | sed 's/\\/\\\\/g; s/`/\\`/g; s/---/\\-\\-\\-/g')
    BODY_BLOCK="
## Original Issue

\`\`\`
${BODY_ESCAPED}
\`\`\`
"
  else
    BODY_BLOCK=""
  fi

  NEW_ISSUE_BODY="Originally reported by @$AUTHOR:
$URL

---

$TITLE

---
${BODY_BLOCK}## Context (2026)

This issue was reported in archived Unit and may still be relevant.

Needs:
- reproduction on FreeUnit
- environment update
- validation against modern runtimes
"

  if [ "$DRY_RUN" = "true" ]; then
    echo "[DRY] Would create issue: $DRAFT_TITLE"
    echo "Labels: $*"
    echo "--- draft body preview ---"
    printf '%s\n' "$NEW_ISSUE_BODY" | head -n 30
    echo "... (truncated)"
    echo "--- end preview ---"
  else
    # if upstream repo reopens for comments, uncomment:
    # echo "Commenting on upstream issue..."
    # if ! gh issue comment "$NUMBER" \
    #   --repo "$UPSTREAM_REPO" \
    #   --body "$COMMENT_BODY" 2>/dev/null; then
    #   echo "WARN: could not comment on $UPSTREAM_REPO#$NUMBER (archived repo may be read-only)"
    # fi

    echo "Creating issue in $TARGET_REPO..."
    create_issue_with_labels "$TARGET_REPO" "$DRAFT_TITLE" "$NEW_ISSUE_BODY" "$@"
  fi

done

printf '\nDone.\n'
