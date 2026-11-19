#!/bin/bash
#
# Decide which Docker variants of a FreeUnit release need a rebuild because
# their base image changed.
#
# Usage:
#   docker-refresh-plan.sh VERSION DOCKER_DIR [EOL_JSON]
#   docker-refresh-plan.sh --self-test
#
# VERSION is the FreeUnit release, for example 1.37.2.  DOCKER_DIR holds
# the Dockerfiles of that release (pkg/docker at the tag).  EOL_JSON is the
# current pkg/eol.json: it names the variants that are still supported and
# the runtime floors.
#
# For each variant the script compares the base image now with the labels
# of the published image <REPO>:<VERSION>-<variant>:
#
#   runtime   the runtime version of the base changed (8.5.12 -> 8.5.13)
#   base      only the base digest changed (an OS update of the base)
#   missing   no published image
#   current   nothing changed
#   blocked   the runtime of the base is below its floor (min_patch); a
#             rebuild would fail the release check, so it is not started
#   error     the base or the published image could not be read (a
#             registry or network error); nothing is decided for it
#
# Only a "not found" answer makes an image missing.  Any other error is an
# error, so a registry outage or a rate limit never starts a rebuild of
# every variant.
#
# The variants to rebuild are "runtime" and "missing", and also "base" when
# INCLUDE_BASE=1.  The script writes these GitHub outputs (to GITHUB_OUTPUT
# when set, else to stdout): rebuild (a comma-separated list), blocked and
# errors (the same), and a Markdown table to GITHUB_STEP_SUMMARY when set.
#
# Environment:
#   REPO            the published repository (default docker.io/freeunitorg/freeunit)
#   INCLUDE_BASE    1 to rebuild the variants whose base digest alone changed
#   REFRESH_INSPECT a command used instead of "docker buildx imagetools
#                   inspect"; the self-test uses it

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
floor_sh="$root/pkg/docker/verify-runtime-floor.sh"

usage() {
	echo "usage: $0 VERSION DOCKER_DIR [EOL_JSON]" >&2
	echo "       $0 --self-test" >&2
	exit 2
}

inspect() {
	# inspect REF FORMAT
	if [ -n "${REFRESH_INSPECT:-}" ]; then
		$REFRESH_INSPECT "$1" "$2"
	else
		docker buildx imagetools inspect "$1" --format "$2"
	fi
}

# The variants of pkg/eol.json, as release-docker.yml derives them.
variants() {
	jq -r '.runtimes | to_entries[] | .key as $rt | .value[]
		| if .version == null then $rt
		  else ($rt + "-" + .version),
		       (if .slim then $rt + "-" + .version + "-slim" else empty end)
		  end' "$1"
}

# "jdk-21.0.13+9" -> 21.0.13, "8.5.12" -> 8.5.12, "" -> "".
norm_version() {
	printf '%s\n' "$1" | sed 's/^[^0-9]*//; s/[+_ ].*//'
}

# The runtime version in the Env of an image config (the same variables
# release-docker.yml reads).
env_runtime() {
	jq -r '[.config.Env // [] | .[]
		| capture("^(GOLANG|JAVA|NODE|PERL|PHP|PYTHON|RUBY)_VERSION=(?<v>.*)$")
		| .v][0] // empty'
}

# The linux/amd64 config of a multi-platform ".Image", or the image itself.
amd64_config() {
	jq -c 'if has("config") then . else (.["linux/amd64"] // (to_entries[0].value)) end'
}

plan() {
	local version=$1 dir=$2 eol=$3
	local repo=${REPO:-docker.io/freeunitorg/freeunit}
	local variant file base_ref base_digest base_cfg base_rt
	local pub_cfg pub_err pub_digest pub_rt new old action out summary v
	local -a rebuild=() blocked=() errors=() rows=()
	local tmp_err
	tmp_err=$(mktemp)

	while read -r variant; do
		file="$dir/Dockerfile.$variant"

		if [ ! -f "$file" ]; then
			rows+=("| $variant | | | | not in $version |")
			continue
		fi

		base_ref=$(awk '/^FROM /{print $2; exit}' "$file")

		base_digest=$(inspect "$base_ref" '{{json .Manifest}}' 2>/dev/null \
			| jq -r '.digest // empty') || base_digest=
		base_cfg=$(inspect "$base_ref" '{{json .Image}}' 2>/dev/null \
			| amd64_config) || base_cfg=
		base_rt=$(printf '%s' "${base_cfg:-{\}}" | env_runtime)

		pub_err=
		pub_cfg=$(inspect "$repo:$version-$variant" '{{json .Image}}' \
			2>"$tmp_err" | amd64_config) || pub_cfg=
		if [ -z "$pub_cfg" ]; then
			if grep -qiE ': not found|manifest unknown' "$tmp_err"; then
				pub_err=missing
			else
				pub_err=error
			fi
		fi
		pub_digest=$(printf '%s' "${pub_cfg:-{\}}" \
			| jq -r '.config.Labels["org.opencontainers.image.base.digest"] // empty')
		pub_rt=$(printf '%s' "${pub_cfg:-{\}}" \
			| jq -r '.config.Labels["org.freeunit.runtime.version"] // empty')

		new=$(norm_version "$base_rt")
		old=$(norm_version "$pub_rt")

		if [ -z "$base_digest" ] || [ -z "$base_cfg" ] \
			|| [ "$pub_err" = error ]; then
			action=error
		elif [ -n "$new" ] \
			&& ! "$floor_sh" --check-version "$variant" "$new" "$eol" >/dev/null
		then
			action=blocked
		elif [ "$pub_err" = missing ]; then
			action=missing
		elif [ -n "$new" ] && [ "$new" != "$old" ]; then
			action=runtime
		elif [ "$base_digest" != "$pub_digest" ]; then
			action=base
		else
			action=current
		fi

		case "$action" in
		runtime|missing) rebuild+=("$variant") ;;
		base) [ "${INCLUDE_BASE:-0}" = 1 ] && rebuild+=("$variant") ;;
		blocked) blocked+=("$variant") ;;
		error) errors+=("$variant") ;;
		esac

		rows+=("| $variant | $base_ref | ${old:--} | ${new:--} | $action |")
	done < <(variants "$eol")

	rm -f "$tmp_err"

	list() { local IFS=,; echo "$*"; }

	out=${GITHUB_OUTPUT:-/dev/stdout}
	{
		echo "rebuild=$(list "${rebuild[@]+"${rebuild[@]}"}")"
		echo "blocked=$(list "${blocked[@]+"${blocked[@]}"}")"
		echo "errors=$(list "${errors[@]+"${errors[@]}"}")"
	} >> "$out"

	summary=${GITHUB_STEP_SUMMARY:-/dev/null}
	{
		echo "### Docker refresh plan for $version"
		echo
		echo "| Variant | Base | Published runtime | Base runtime | Action |"
		echo "|---------|------|-------------------|--------------|--------|"
		printf '%s\n' "${rows[@]}"
	} >> "$summary"

	for v in "${blocked[@]+"${blocked[@]}"}"; do
		echo "::warning::$v: the base image runtime is below its floor in pkg/eol.json; not rebuilt"
	done
	for v in "${errors[@]+"${errors[@]}"}"; do
		echo "::warning::$v: an image could not be read; not rebuilt"
	done
}

self_test() {
	dir=$(mktemp -d)
	trap 'rm -rf "$dir"' EXIT

	mkdir -p "$dir/docker" "$dir/fix"

	cat > "$dir/eol.json" <<'EOF'
{"runtimes": {
  "php":  [{"version": "8.3"}, {"version": "8.4"}, {"version": "8.5"}],
  "ruby": [{"version": "3.4", "min_patch": "3.4.13"}],
  "perl": [{"version": "5.42"}],
  "node": [{"version": "26"}],
  "java": [{"version": "21", "min_patch": "21.0.13.1"}],
  "python": [{"version": "3.12"}, {"version": "3.13", "slim": true}],
  "minimal": [{"version": null}],
  "go": [{"version": "1.27"}]
}}
EOF
	# go-1.27 is not in this release.
	for v in php-8.3 php-8.4 php-8.5 ruby-3.4 perl-5.42 node-26 java-21 python-3.12 python-3.13 python-3.13-slim minimal; do
		base=$v
		case "$v" in minimal) base=debian ;; esac
		echo "FROM base-$base:tag" > "$dir/docker/Dockerfile.$v"
	done

	# fixture NAME DIGEST ENV_RUNTIME [LABEL_DIGEST LABEL_RUNTIME]
	fixture() {
		printf '{"digest":"%s"}\n' "$2" > "$dir/fix/$1.manifest"
		printf '{"linux/amd64":{"config":{"Env":["PATH=/bin"%s],"Labels":{%s}}},"linux/arm64":{}}\n' \
			"${3:+,\"$3\"}" \
			"${4:+\"org.opencontainers.image.base.digest\":\"$4\",\"org.freeunit.runtime.version\":\"$5\"}" \
			> "$dir/fix/$1.image"
	}

	# The base images now.  As the real perl:5.42-trixie, the perl base
	# has no PERL_VERSION, so a new Perl release is only a new digest.
	fixture base-php-8.3:tag      sha256:a3 PHP_VERSION=8.3.30
	fixture base-php-8.4:tag      sha256:a1 PHP_VERSION=8.4.28
	fixture base-php-8.5:tag      sha256:b2 PHP_VERSION=8.5.12
	fixture base-ruby-3.4:tag     sha256:c3 RUBY_VERSION=3.4.13
	fixture base-perl-5.42:tag    sha256:c9 ""
	fixture base-node-26:tag      sha256:d9 NODE_VERSION=26.2.0
	fixture repo:1.0.0-node-26    sha256:x "" sha256:d0 26.1.0
	fixture base-java-21:tag      sha256:d4 JAVA_VERSION=jdk-21.0.13+9
	fixture base-python-3.12:tag  sha256:e2 PYTHON_VERSION=3.12.12
	fixture base-python-3.13:tag  sha256:e5 PYTHON_VERSION=3.13.9
	fixture base-python-3.13-slim:tag sha256:f6 PYTHON_VERSION=3.13.9
	fixture base-debian:tag       sha256:a7 ""

	# The published images.
	fixture repo:1.0.0-php-8.4      sha256:x "" sha256:a0 8.4.27
	fixture repo:1.0.0-php-8.5      sha256:x "" sha256:b2 8.5.12
	fixture repo:1.0.0-ruby-3.4     sha256:x "" sha256:c0 3.4.12
	# The published perl image carries the Debian release as its runtime.
	fixture repo:1.0.0-perl-5.42    sha256:x "" sha256:c8 13.7
	fixture repo:1.0.0-python-3.12  sha256:x "" sha256:e1 3.12.11
	fixture repo:1.0.0-java-21      sha256:x "" sha256:d0 jdk-21.0.12+8
	fixture repo:1.0.0-python-3.13  sha256:x "" sha256:e0 3.13.9
	fixture repo:1.0.0-minimal      sha256:x "" sha256:a0 13.2
	# python-3.13-slim is not published.

	cat > "$dir/inspect" <<EOF
#!/bin/sh
ref=\$1
case "\$2" in *Manifest*) f="$dir/fix/\$ref.manifest" ;; *) f="$dir/fix/\$ref.image" ;; esac
# node-26: the base config fails, python-3.12: the base digest fails,
# php-8.3: the published image fails.  Each one alone is an error.
case "\$ref:\$2" in
base-node-26:tag:*Image*|base-python-3.12:tag:*Manifest*|repo:1.0.0-php-8.3:*)
	echo "ERROR: 429 Too Many Requests" >&2; exit 1 ;;
esac
[ -f "\$f" ] || { echo "ERROR: \$ref: not found" >&2; exit 1; }
cat "\$f"
EOF
	chmod +x "$dir/inspect"

	run() {
		: > "$dir/out"
		REPO=repo REFRESH_INSPECT="$dir/inspect" INCLUDE_BASE=$1 \
			GITHUB_OUTPUT="$dir/out" GITHUB_STEP_SUMMARY="$dir/summary" \
			plan 1.0.0 "$dir/docker" "$dir/eol.json" >/dev/null 2>&1
		cat "$dir/out"
	}

	fail=0
	want() {
		if [ "$2" != "$3" ]; then
			echo "FAIL self-test $1: got '$2', want '$3'"
			fail=1
		fi
	}

	got=$(run 0)
	want "rebuild, runtime only" "$(sed -n 's/^rebuild=//p' <<< "$got")" \
		"php-8.4,ruby-3.4,python-3.13-slim"
	want "blocked" "$(sed -n 's/^blocked=//p' <<< "$got")" "java-21"
	want "errors" "$(sed -n 's/^errors=//p' <<< "$got")" \
		"php-8.3,node-26,python-3.12"

	got=$(run 1)
	want "rebuild, with base" "$(sed -n 's/^rebuild=//p' <<< "$got")" \
		"php-8.4,ruby-3.4,perl-5.42,python-3.13,python-3.13-slim,minimal"

	want "norm java" "$(norm_version 'jdk-21.0.13+9')" 21.0.13
	want "norm java .1" "$(norm_version 'jdk-21.0.13.1+2')" 21.0.13.1
	want "norm php" "$(norm_version 8.5.13)" 8.5.13
	want "norm empty" "$(norm_version '')" ""

	grep -q '| go-1.27 | | | | not in 1.0.0 |' "$dir/summary" \
		|| { echo "FAIL self-test: go-1.27 not reported as absent"; fail=1; }
	grep -q '| perl-5.42 | base-perl-5.42:tag | 13.7 | - | base |' "$dir/summary" \
		|| { echo "FAIL self-test: perl-5.42 not base"; fail=1; }
	grep -q '| php-8.5 | base-php-8.5:tag | 8.5.12 | 8.5.12 | current |' "$dir/summary" \
		|| { echo "FAIL self-test: php-8.5 not current"; fail=1; }

	[ "$fail" -eq 0 ] || exit 1
	echo "PASS self-test: docker refresh plan"
}

case "${1:-}" in
--self-test) [ $# -eq 1 ] || usage; self_test ;;
-*|'') usage ;;
*) [ $# -ge 2 ] && [ $# -le 3 ] || usage; plan "$1" "$2" "${3:-$root/pkg/eol.json}" ;;
esac
