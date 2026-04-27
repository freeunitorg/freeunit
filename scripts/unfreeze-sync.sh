#!/usr/bin/env bash

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

LIMIT="${1:-5}"   # max issues to fetch
DRY_RUN="${DRY_RUN:-true}"  # true = print only, false = apply changes

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
  --json number,title,body,author,url,state,createdAt)

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

  TEXT="$TITLE
$BODY"

  if ! matches_keywords "$TEXT"; then
    continue
  fi

  DRAFT_TITLE="[unfreeze] $TITLE"

  # --- dedup check (exact line match) ---
  if grep -qxF "$DRAFT_TITLE" <<< "$EXISTING_TITLES"; then
    echo "SKIP (exists): #$NUMBER $TITLE"
    continue
  fi

  echo "----"
  echo "#$NUMBER [$STATE] $TITLE"
  echo "Created: $CREATED"
  echo "Author: @$AUTHOR"
  echo "URL: $URL"

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

  # --- draft issue body ---
  NEW_ISSUE_BODY=$(cat <<EOF
Originally reported by @$AUTHOR:
$URL

---

$TITLE

---

## Context (2026)

This issue was reported in archived Unit and may still be relevant.

Needs:
- reproduction on FreeUnit
- environment update
- validation against modern runtimes

EOF
)

  if [ "$DRY_RUN" = "true" ]; then
    echo "[DRY] Would comment upstream + create issue: $DRAFT_TITLE"
  else
    echo "Commenting on upstream issue..."
    if ! gh issue comment "$NUMBER" \
      --repo "$UPSTREAM_REPO" \
      --body "$COMMENT_BODY" 2>/dev/null; then
      echo "WARN: could not comment on $UPSTREAM_REPO#$NUMBER (archived repo may be read-only)"
    fi

    echo "Creating issue in $TARGET_REPO..."
    gh issue create \
      --repo "$TARGET_REPO" \
      --title "$DRAFT_TITLE" \
      --body "$NEW_ISSUE_BODY" \
      --label "unfreeze" \
      --label "needs-repro"
  fi

done

echo
echo "Done."