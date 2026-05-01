#!/usr/bin/env bash
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

set -euo pipefail

UPSTREAM_REPO="nginx/unit"
TARGET_REPO="freeunitorg/freeunit"

# keywords for filtering (extend as needed)
KEYWORDS=(
  "php"
  "crash"
  "segfault"
  "wasm"
  "wamr"
  "bun"
  "python"
  "docker"
  "fedora"
  "ubuntu"
  "logging"
  "rewrite"
)

# ported labels map (api name => color) — must match labels imported by unfreeze-labels.sh
# GitHub API returns full names with emoji, e.g. "z-bug 🐞", "z-enhancement ⬆️"
# name may contain colons, use | as entry separator
declare -A LABELS=(
  ["z-bug 🐞"]="ededed"
  ["z-enhancement ⬆️"]="ededed"
  ["z-crasher"]="ededed"
  ["z-configuration"]="ededed"
  ["z-c"]="ededed"
  ["z-community"]="ededed"
  ["z-documentation-update-needed"]="ededed"
  ["z-Difficulty: ⭐️"]="ededed"
  ["z-Difficulty: ⭐️ ⭐️"]="ededed"
  ["z-Difficulty: ⭐️ ⭐️ ⭐️"]="ededed"
  ["z-duplicate"]="ededed"
  ["z-go"]="ededed"
  ["z-infrastructure"]="ededed"
  ["z-invalid"]="ededed"
  ["z-java"]="ededed"
  ["z-needs-investigation"]="ededed"
  ["z-node-js"]="ededed"
  ["z-notabug"]="ededed"
  ["z-packages"]="ededed"
  ["z-php"]="ededed"
  ["z-python"]="ededed"
  ["z-question"]="ededed"
  ["z-ready4dev 🚀"]="ededed"
  ["z-roadmap"]="ededed"
  ["z-ruby"]="ededed"
  ["z-rust"]="ededed"
  ["z-toolchain"]="ededed"
  ["z-wasm"]="ededed"
  ["z-wontfix"]="ededed"
  ["L-Go"]="2b67c6"
  ["L-Java"]="2b67c6"
  ["L-JavaScript"]="2b67c6"
  ["L-Libunit"]="2b67c6"
  ["L-Perl"]="2b67c6"
  ["L-PHP"]="2b67c6"
  ["L-Python"]="2b67c6"
  ["L-Ruby"]="2b67c6"
  ["L-WebAssembly"]="2b67c6"
  ["T-Defect"]="98e6ae"
  ["T-Enhancement"]="98e6ae"
  ["T-Other"]="98e6ae"
  ["X-Needs-Discussion"]="ff7979"
  ["X-Needs-Info"]="ff7979"
  ["X-Release-Blocker"]="ff7979"
)

# --- args ---
DRY_RUN="${DRY_RUN:-true}"

LIMIT=5
while [[ $# -gt 0 ]]; do
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
if ! command -v gh >/dev/null; then
  echo "ERROR: gh CLI not installed"
  exit 1
fi

gh auth status >/dev/null 2>&1 || {
  echo "ERROR: gh not authenticated"
  exit 1
}

# --- keyword filter ---
matches_keywords() {
  local text="$1"
  for kw in "${KEYWORDS[@]}"; do
    if grep -iq "$kw" <<< "$text"; then
      return 0
    fi
  done
  return 1
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

COUNT=$(echo "$ISSUES_JSON" | jq length)
echo "Fetched $COUNT issues"
echo

# --- process ---
echo "$ISSUES_JSON" | jq -c '.[]' | while read -r issue; do
  NUMBER=$(echo "$issue" | jq -r '.number')
  TITLE=$(echo "$issue" | jq -r '.title')
  BODY=$(echo "$issue" | jq -r '.body // ""')
  AUTHOR=$(echo "$issue" | jq -r '.author.login')
  URL=$(echo "$issue" | jq -r '.url')
  STATE=$(echo "$issue" | jq -r '.state')
  CREATED=$(echo "$issue" | jq -r '.createdAt | split("T")[0]')
  UPSTREAM_LABELS_JSON=$(echo "$issue" | jq -r '[.labels[] | .name] | join("\n")')

  TEXT="$TITLE"
  if [ -n "$BODY" ]; then
    TEXT="$TEXT"$'\n'"$BODY"
  fi

  if ! matches_keywords "$TEXT"; then
    continue
  fi

  DRAFT_TITLE="[unfreeze] $TITLE"

  # --- dedup check (exact line match against "[unfreeze] title") ---
  if grep -qxF "$DRAFT_TITLE" <<< "$EXISTING_TITLES"; then
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
  LABELS_TO_ADD=("unfreeze" "needs-repro")
  IFS=$'\n' read -ra UL <<< "$UPSTREAM_LABELS_JSON"
  for ul in "${UL[@]}"; do
    # GitHub API normalizes label names
    if [[ -v "LABELS[$ul]" ]]; then
      LABELS_TO_ADD+=("$ul")
    fi
  done

  # --- upstream comment ---
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
    echo "Labels: ${LABELS_TO_ADD[*]}"
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
    # build --label args
    LABEL_ARGS=()
    for lbl in "${LABELS_TO_ADD[@]}"; do
      LABEL_ARGS+=(--label "$lbl")
    done

    gh issue create \
      --repo "$TARGET_REPO" \
      --title "$DRAFT_TITLE" \
      --body "$NEW_ISSUE_BODY" \
      "${LABEL_ARGS[@]}"
  fi

done

printf '\nDone.\n'
