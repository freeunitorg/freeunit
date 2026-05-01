#!/usr/bin/env bash
#
# unfreeze-labels.sh — one-time: import nginx/unit labels into freeunitorg/freeunit
# Run once, then delete.
#
# Usage:
#   DRY_RUN=true  ./unfreeze-labels.sh   # preview
#   DRY_RUN=false ./unfreeze-labels.sh   # create
#   DELETE_ALL=true ./unfreeze-labels.sh  # delete ALL existing labels first, then create
exit 0
set -euo pipefail

UPSTREAM_REPO="nginx/unit"
TARGET_REPO="freeunitorg/freeunit"
DRY_RUN="${DRY_RUN:-true}"
DELETE_ALL="${DELETE_ALL:-false}"

echo "== FreeUnit label import =="
echo "Upstream: $UPSTREAM_REPO"
echo "Target:   $TARGET_REPO"
echo "Dry-run:  $DRY_RUN"
echo "Delete all: $DELETE_ALL"
echo

if ! command -v gh >/dev/null; then
  echo "ERROR: gh CLI not installed"
  exit 1
fi

gh auth status >/dev/null 2>&1 || {
  echo "ERROR: gh not authenticated"
  exit 1
}

if [ "$DELETE_ALL" = "true" ]; then
  echo "Fetching existing labels from $TARGET_REPO..."
  EXISTING=$(gh api --paginate "repos/$TARGET_REPO/labels?per_page=100" \
    -q '.[].name' 2>/dev/null || true)
  count=$(echo "$EXISTING" | grep -c . || true)
  echo "Found $count existing labels. Deleting..."
  if [ "$DRY_RUN" = "true" ]; then
    echo "[DRY] Would delete all $count labels"
  else
    echo "$EXISTING" | while read -r label; do
      [ -z "$label" ] && continue
      if gh label delete "$label" --repo "$TARGET_REPO" --yes 2>/dev/null; then
        echo "DELETED: $label"
      else
        echo "FAILED:  $label"
      fi
    done
  fi
  echo
fi

# --- core labels (always create first) ---
CORE_LABELS=(
  "unfreeze|Migrated from archived Unit|ededed"
  "needs-repro|Missing reproducible case|ededed"
)

# --- ported nginx/unit labels (name|description|color)
# GitHub API returns full names with emoji, e.g. "z-bug 🐞", "z-enhancement ⬆️"
# name may contain colons, use | as entry separator
declare -a LABELS=(
  "z-bug 🐞|Bug report from archived Unit|ededed"
  "z-enhancement ⬆️|Product Enhancement from archived Unit|ededed"
  "z-crasher|Bug causes a segfault/abort etc|ededed"
  "z-configuration|Configuration questions|ededed"
  "z-c|C Programming Language|ededed"
  "z-community|Community contribution|ededed"
  "z-documentation-update-needed|Needs docs update|ededed"
  "z-Difficulty: ⭐️|Difficulty: EASY|ededed"
  "z-Difficulty: ⭐️ ⭐️|Difficulty: MEDIUM|ededed"
  "z-Difficulty: ⭐️ ⭐️ ⭐️|Difficulty: HARD|ededed"
  "z-duplicate|Duplicate issue|ededed"
  "z-go|Go language module|ededed"
  "z-infrastructure|Infrastructure issue|ededed"
  "z-invalid|Invalid issue|ededed"
  "z-java|Java language module|ededed"
  "z-needs-investigation|Needs investigation|ededed"
  "z-node-js|Node.js language module|ededed"
  "z-notabug|Not a bug|ededed"
  "z-packages|Packages issue|ededed"
  "z-php|PHP language module|ededed"
  "z-python|Python language module|ededed"
  "z-question|Question|ededed"
  "z-ready4dev 🚀|Ready for development|ededed"
  "z-roadmap|Roadmap item|ededed"
  "z-ruby|Ruby language module|ededed"
  "z-rust|Rust in core or modules|ededed"
  "z-toolchain|Toolchain issue|ededed"
  "z-wasm|WebAssembly language module|ededed"
  "z-wontfix|Won't fix|ededed"
  "L-Go|Go language module (upstream)|2b67c6"
  "L-Java|Java language module (upstream)|2b67c6"
  "L-JavaScript|JavaScript language module (upstream)|2b67c6"
  "L-Libunit|Libunit test module (upstream)|2b67c6"
  "L-Perl|Perl language module (upstream)|2b67c6"
  "L-PHP|PHP language module (upstream)|2b67c6"
  "L-Python|Python language module (upstream)|2b67c6"
  "L-Ruby|Ruby language module (upstream)|2b67c6"
  "L-WebAssembly|WebAssembly module (upstream)|2b67c6"
  "T-Defect|Defect (upstream)|98e6ae"
  "T-Enhancement|Enhancement (upstream)|98e6ae"
  "T-Other|Other task (upstream)|98e6ae"
  "X-Needs-Discussion|Needs discussion (upstream)|ff7979"
  "X-Needs-Info|Needs info (upstream)|ff7979"
  "X-Release-Blocker|Release blocker (upstream)|ff7979"
)

echo "Fetching existing labels from $TARGET_REPO..."
EXISTING=$(gh api --paginate "repos/$TARGET_REPO/labels?per_page=100" \
  -q '.[].name' 2>/dev/null || true)
if [ -z "$EXISTING" ]; then
  echo "WARN: could not fetch existing labels from $TARGET_REPO"
fi

create_label() {
  local name="$1" desc="$2" color="$3"
  if echo "$EXISTING" | grep -qxF -- "$name"; then
    echo "EXISTS: $name"
    return
  fi
  if [ "$DRY_RUN" = "true" ]; then
    echo "[DRY] Would create: $name ($desc, #$color)"
  else
    if gh label create "$name" \
      --repo "$TARGET_REPO" \
      --description "$desc" \
      --color "$color" 2>/dev/null; then
      echo "CREATED: $name"
    else
      echo "FAILED:  $name"
    fi
  fi
}

echo
echo "--- Core labels ---"
for entry in "${CORE_LABELS[@]}"; do
  name="${entry%%|*}"
  rest="${entry#*|}"
  desc="${rest%%|*}"
  color="${rest##*|}"
  create_label "$name" "$desc" "$color"
done

echo
echo "--- Ported upstream labels (${#LABELS[@]} total) ---"
for entry in "${LABELS[@]}"; do
  # split on | (name may contain colons)
  name="${entry%%|*}"
  rest="${entry#*|}"
  desc="${rest%%|*}"
  color="${rest##*|}"
  create_label "$name" "$desc" "$color"
done

echo
echo "Done. Core labels + ${#LABELS[@]} upstream labels processed."
echo "Remove this script after run."
