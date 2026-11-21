#!/bin/sh
#
# Check that a FreeUnit image ships a runtime at or above its security floor.
#
# Usage:
#   verify-runtime-floor.sh IMAGE VARIANT [EOL_JSON]
#   verify-runtime-floor.sh --check-eol [EOL_JSON]
#   verify-runtime-floor.sh --self-test
#
# The floor is the "min_patch" field of the runtime version in pkg/eol.json:
# the first patch release of that branch that carries the fix of a known
# security defect.  A version without "min_patch" has no floor, and a
# variant without a runtime version (wasm, minimal) has none either; the
# check prints SKIP for both.  A variant that pkg/eol.json does not have
# fails, so a misspelt variant does not pass as one without a floor.
#
# IMAGE VARIANT runs the runtime in the image and exits 1 when the version
# is below the floor, is not on the branch of the variant, or cannot be
# read.  The image is pulled first when it is not in the local store; the
# run itself has --pull=never and --network none.  Only the standard
# output of the container is parsed, and it must have the form that the
# runtime prints, so a docker message or a pre-release never passes.
# VARIANT is the Dockerfile suffix, for example "ruby-3.4" or
# "python-3.13-slim".
#
# --check-eol checks the file only: every "min_patch" is a release version
# (digits and dots) of its own branch.  --self-test checks the parsing, the
# comparison and the image check with a docker stub.  Neither needs docker.

set -eu

here=$(dirname "$0")
default_eol="$here/../eol.json"

usage() {
	echo "usage: $0 IMAGE VARIANT [EOL_JSON]" >&2
	echo "       $0 --check-eol [EOL_JSON]" >&2
	echo "       $0 --self-test" >&2
	exit 2
}

# version_ge HAVE WANT: HAVE is at least WANT.  "sort -V" orders 21.0.13
# before 21.0.13.1 and 3.4.9 before 3.4.13.
version_ge() {
	[ "$(printf '%s\n%s\n' "$2" "$1" | sort -V | head -n 1)" = "$2" ]
}

# on_branch VERSION BRANCH: VERSION is BRANCH or a release of it.  21.0.13
# is on 21, 3.4.13 is on 3.4, 3.40.1 is not on 3.4.
on_branch() {
	case "$1" in
	"$2"|"$2".*) return 0 ;;
	*) return 1 ;;
	esac
}

# split_variant VARIANT: sets "runtime" and "branch".  A "-slim" suffix
# belongs to the image, not to the runtime version.
split_variant() {
	v=${1%-slim}
	runtime=${v%%-*}
	branch=${v#*-}

	if [ "$runtime" = "$v" ] || [ -z "$branch" ]; then
		runtime=
		branch=
	fi
}

# floor EOL_JSON RUNTIME BRANCH: prints the "min_patch" of the entry,
# "none" when the entry has no floor, and nothing when pkg/eol.json has no
# such entry.
floor() {
	jq -r --arg rt "$2" --arg v "$3" \
		'[.runtimes[$rt] // [] | .[] | select(.version == $v)
		  | .min_patch // "none"][0] // empty' "$1"
}

# lookup EOL_JSON VARIANT: sets "runtime", "branch" and "min".  "min" is the
# floor, "none" for a version without a floor, "image" for a variant
# without a runtime version (wasm, minimal), and empty for a variant that
# pkg/eol.json does not have.  An empty "min" is a failure: a misspelt
# variant must not pass as one without a floor.
lookup() {
	split_variant "$2"
	min=

	if [ -z "$runtime" ]; then
		if [ "$(jq -r --arg v "${2%-slim}" \
			'.runtimes[$v] // [] | any(.version == null)' "$1")" = true ]
		then
			min=image
		fi
		return 0
	fi

	min=$(floor "$1" "$runtime" "$branch") || min=
}

# The command that prints the runtime version inside an image.  Its output
# is read by parse_version.
version_cmd() {
	case "$1" in
	ruby)   echo "ruby -e 'print RUBY_VERSION'" ;;
	java)   echo "java -version 2>&1" ;;
	php)    echo "php -r 'echo PHP_VERSION;'" ;;
	python) echo "python3 -c 'import platform; print(platform.python_version())'" ;;
	node)   echo "node -p process.versions.node" ;;
	perl)   echo "perl -e 'print \$^V'" ;;
	go)     echo "go env GOVERSION" ;;
	*)      echo ;;
	esac
}

# parse_version RUNTIME OUTPUT: prints the version when OUTPUT has the form
# that the command of version_cmd prints, and nothing otherwise.  A
# pre-release ("3.4.13rc1", "27-ea"), a shell error or a line of another
# program gives nothing.
#
#   java    the line 'openjdk version "21.0.13.1" 2026-11-18 LTS'; the JVM
#           can print other lines first ("Picked up JAVA_TOOL_OPTIONS: ...")
#   go      the single line "go1.26.3"
#   perl    the single line "v5.42.0"
#   others  the single line "3.4.13"
parse_version() {
	case "$1" in
	java)
		printf '%s\n' "$2" | sed -n \
			-e 's/^openjdk version "\([0-9][0-9]*\(\.[0-9][0-9]*\)*\)"\( .*\)\{0,1\}$/\1/p' \
			-e 's/^java version "\([0-9][0-9]*\(\.[0-9][0-9]*\)*\)"\( .*\)\{0,1\}$/\1/p' \
			| head -n 1
		return 0
		;;
	go)   prefix=go ;;
	perl) prefix=v ;;
	*)    prefix= ;;
	esac

	[ "$(printf '%s\n' "$2" | wc -l)" -eq 1 ] || return 0
	printf '%s\n' "$2" \
		| sed -n "s/^$prefix\\([0-9][0-9]*\\(\\.[0-9][0-9]*\\)\\{1,\\}\\)\$/\\1/p"
}

# compare VARIANT HAVE MIN: prints the verdict, returns 1 when HAVE is not
# on the branch of VARIANT or is below MIN.
compare() {
	split_variant "$1"
	# The floor belongs to the branch of the variant: an image of another
	# branch (java-17 with Java 21) must not pass on the floor of the label.
	if ! on_branch "$2" "$branch"; then
		echo "FAIL runtime floor: $1 ships $runtime $2, not a version of $branch"
		return 1
	fi
	if version_ge "$2" "$3"; then
		echo "PASS runtime floor: $1 ships $runtime $2, at least $3"
	else
		echo "FAIL runtime floor: $1 ships $runtime $2, below the floor $3; rebuild on a base with $runtime $3 or later"
		return 1
	fi
}

# check_image IMAGE VARIANT EOL_JSON: prints the verdict, returns 1 when the
# image must not be published.  Only the standard output of the container
# is parsed.  The messages of docker itself ("Unable to find image ...
# locally") go to a file and appear only in a FAIL line.
check_image() {
	lookup "$3" "$2"

	case "$min" in
	'')
		echo "FAIL runtime floor: $2 is not a variant in pkg/eol.json"
		return 1
		;;
	image)
		echo "SKIP runtime floor: $2 has no runtime version"
		return 0
		;;
	none)
		echo "SKIP runtime floor: $2 has no min_patch in pkg/eol.json"
		return 0
		;;
	esac

	cmd=$(version_cmd "$runtime")

	if [ -z "$cmd" ]; then
		echo "FAIL runtime floor: $2 has a floor, but the check cannot read a $runtime version"
		return 1
	fi

	err=$(mktemp) || return 1

	# Pull first and run with --pull=never, so the run never pulls and
	# never prints a pull message.
	if ! docker image inspect "$1" >/dev/null 2>&1 \
		&& ! docker pull --quiet "$1" >/dev/null 2>"$err"
	then
		echo "FAIL runtime floor: $2: cannot pull $1 ($(head -n 1 "$err"))"
		rm -f "$err"
		return 1
	fi

	if out=$(docker run --rm --pull=never --network none \
		--entrypoint sh "$1" -c "$cmd" 2>"$err")
	then
		rc=0
	else
		rc=$?
	fi
	msg=$(head -n 1 "$err")
	rm -f "$err"

	if [ "$rc" -ne 0 ]; then
		echo "FAIL runtime floor: $2: cannot read the $runtime version (exit $rc: ${msg:-no message})"
		return 1
	fi

	have=$(parse_version "$runtime" "$out")

	if [ -z "$have" ]; then
		echo "FAIL runtime floor: $2: cannot read the $runtime version (${out:-no output})"
		return 1
	fi

	compare "$2" "$have" "$min"
}

check_eol() {
	eol=${1:-$default_eol}
	bad=$(jq -r '.runtimes | to_entries[] | .key as $rt | .value[]
		| select(.min_patch != null) | .version as $v
		| select((.min_patch | type == "string"
			and test("^[0-9]+(\\.[0-9]+)+$")
			and startswith($v + ".")) | not)
		| "\($rt) \(.version): min_patch \(.min_patch)"' "$eol")

	if [ -n "$bad" ]; then
		printf '%s\n' "$bad" | sed 's/^/FAIL eol.json: /; s/$/ is not a version of the branch/'
		return 1
	fi

	echo "PASS eol.json: every min_patch is a version of its branch"
}

self_test() {
	n=0
	fail=0

	t() {
		n=$((n + 1))
		if [ "$2" != "$3" ]; then
			echo "FAIL self-test $1: got '$2', want '$3'"
			fail=1
		fi
	}

	ge() {
		if version_ge "$1" "$2"; then echo yes; else echo no; fi
	}

	t "ge equal"        "$(ge 3.4.13 3.4.13)"       yes
	t "ge below"        "$(ge 3.4.12 3.4.13)"       no
	t "ge two digits"   "$(ge 3.4.9 3.4.13)"        no
	t "ge above"        "$(ge 3.4.14 3.4.13)"       yes
	t "ge java .1"      "$(ge 21.0.13 21.0.13.1)"   no
	t "ge java .1 eq"   "$(ge 21.0.13.1 21.0.13.1)" yes
	t "ge java next"    "$(ge 21.0.14 21.0.13.1)"   yes
	t "ge java .1 old"  "$(ge 21.0.12.1 21.0.13.1)" no

	br() {
		if on_branch "$1" "$2"; then echo yes; else echo no; fi
	}

	t "branch java"     "$(br 21.0.13 21)"   yes
	t "branch java 17"  "$(br 21.0.13 17)"   no
	t "branch ruby"     "$(br 3.4.13 3.4)"   yes
	t "branch ruby 3.3" "$(br 3.4.13 3.3)"   no
	t "branch prefix"   "$(br 3.40.1 3.4)"   no

	t "parse java" \
		"$(parse_version java 'openjdk version "21.0.13" 2026-10-20 LTS')" 21.0.13
	t "parse java .1" \
		"$(parse_version java 'openjdk version "25.0.5.1" 2026-11-18 LTS')" 25.0.5.1
	t "parse java 27" \
		"$(parse_version java 'openjdk version "27" 2027-03-16')" 27
	t "parse java ea" \
		"$(parse_version java 'openjdk version "27-ea" 2027-03-16')" ""
	t "parse java picked up" "$(parse_version java 'Picked up JAVA_TOOL_OPTIONS: -Dx.version=99.0.1
openjdk version "21.0.13.1" 2026-11-18 LTS')" 21.0.13.1
	t "parse java other line" \
		"$(parse_version java 'Picked up JAVA_TOOL_OPTIONS: -Dx.version=99.0.1')" ""
	t "parse go"    "$(parse_version go go1.26.3)" 1.26.3
	t "parse go rc" "$(parse_version go go1.27rc1)" ""
	t "parse perl"  "$(parse_version perl v5.42.0)" 5.42.0
	t "parse ruby"  "$(parse_version ruby 4.0.10)" 4.0.10
	t "parse rc"    "$(parse_version ruby 3.4.13rc1)" ""
	t "parse rc -"  "$(parse_version ruby 3.4.13-rc1)" ""
	t "parse distro" "$(parse_version ruby 3.4.12-1+deb13u9)" ""
	t "parse docker" \
		"$(parse_version ruby "Unable to find image 'ruby:3.4.13-trixie' locally")" ""
	t "parse two lines" "$(parse_version ruby '3.4.2
3.4.13')" ""
	t "parse empty" "$(parse_version ruby 'sh: ruby: not found')" ""
	t "parse dash"  "$(parse_version java 'sh: 1: java: not found')" ""

	split_variant ruby-3.4;         t "split ruby"   "$runtime $branch" "ruby 3.4"
	split_variant python-3.13-slim; t "split slim"   "$runtime $branch" "python 3.13"
	split_variant java-21;          t "split java"   "$runtime $branch" "java 21"
	split_variant wasm;             t "split wasm"   "$runtime $branch" " "

	dir=$(mktemp -d)
	trap 'rm -rf "$dir"' EXIT

	cat > "$dir/eol.json" <<'EOF'
{"runtimes": {
  "ruby": [{"version": "3.4", "min_patch": "3.4.13"},
           {"version": "4.0"}],
  "java": [{"version": "21", "min_patch": "21.0.13.1"}],
  "lua": [{"version": "5.4", "min_patch": "5.4.7"}],
  "minimal": [{"version": null}]
}}
EOF
	t "floor set"     "$(floor "$dir/eol.json" ruby 3.4)" 3.4.13
	t "floor none"    "$(floor "$dir/eol.json" ruby 4.0)" none
	t "floor absent"  "$(floor "$dir/eol.json" ruby 3.3)" ""
	t "floor runtime" "$(floor "$dir/eol.json" go 1.26)" ""
	t "check-eol ok"  "$(check_eol "$dir/eol.json" >/dev/null && echo ok)" ok

	for bad in '"3.3.14"' '"3.4.x"' '"3.4."' '"3.4"' '3.4' '"3.4.l3"'; do
		printf '{"runtimes": {"ruby": [{"version": "3.4", "min_patch": %s}]}}\n' \
			"$bad" > "$dir/bad.json"
		t "check-eol bad $bad" \
			"$(check_eol "$dir/bad.json" >/dev/null 2>&1 || echo fail)" fail
	done

	# The image check, with a docker stub.  The stub refuses a run without
	# --pull=never and --network none.  stub_local=1 means the image is not
	# in the local store, and stub_pull is the exit status of the pull.
	docker() {
		case "$1" in
		image) return "$stub_local" ;;
		pull)  [ "$stub_pull" -eq 0 ] || echo "pull access denied" >&2
		       return "$stub_pull" ;;
		run)
			case " $* " in
			*" --pull=never "*) ;;
			*) echo "stub: docker run may pull: $*" >&2; return 125 ;;
			esac
			case " $* " in
			*" --network none "*) ;;
			*) echo "stub: docker run has a network: $*" >&2; return 125 ;;
			esac
			[ -z "$stub_err" ] || printf '%s\n' "$stub_err" >&2
			printf '%s' "$stub_out"
			return "$stub_rc"
			;;
		esac
		return 99
	}

	# img VARIANT STDOUT [STDERR [STATUS [LOCAL [PULL]]]]: the exit status
	# of check_image and the first word of each line it prints.  Every
	# verdict is one line, so "1 FAIL" is a refusal and "1 FAIL FAIL" is a
	# refusal that went on to a second check.
	img() {
		stub_out=$2 stub_err=${3:-} stub_rc=${4:-0}
		stub_local=${5:-0} stub_pull=${6:-0}
		if res=$(check_image img "$1" "$dir/eol.json" 2>/dev/null); then
			rc=0
		else
			rc=1
		fi
		printf '%s %s\n' "$rc" \
			"$(printf '%s\n' "$res" | cut -d ' ' -f 1 | paste -s -d ' ' -)"
	}

	t "image equal"      "$(img ruby-3.4 3.4.13)" "0 PASS"
	t "image above"      "$(img ruby-3.4 3.4.14)" "0 PASS"
	t "image below"      "$(img ruby-3.4 3.4.12)" "1 FAIL"
	t "image branch"     "$(img ruby-3.4 3.3.14)" "1 FAIL"
	t "image branch up"  "$(img ruby-3.4 4.0.10)" "1 FAIL"
	t "image no output"  "$(img ruby-3.4 '')" "1 FAIL"
	t "image unparsed"   "$(img ruby-3.4 3.4.13rc1)" "1 FAIL"
	t "image not found"  "$(img ruby-3.4 '' 'sh: ruby: not found' 127)" "1 FAIL"
	t "image run error"  "$(img ruby-3.4 3.4.13 'docker: error' 125)" "1 FAIL"
	t "image docker msg" "$(img ruby-3.4 3.4.2 \
		"Unable to find image 'ruby:3.4.13-trixie' locally")" "1 FAIL"
	t "image stderr"     "$(img ruby-3.4 3.4.13 \
		"Unable to find image 'ruby:3.4.13-trixie' locally")" "0 PASS"
	t "image pulled"     "$(img ruby-3.4 3.4.13 '' 0 1 0)" "0 PASS"
	t "image no pull"    "$(img ruby-3.4 3.4.13 '' 0 1 1)" "1 FAIL"
	t "image java" \
		"$(img java-21 'openjdk version "21.0.13.1" 2026-11-18 LTS')" "0 PASS"
	t "image java below" \
		"$(img java-21 'openjdk version "21.0.13" 2026-10-20 LTS')" "1 FAIL"
	t "image java opts" \
		"$(img java-21 'Picked up JAVA_TOOL_OPTIONS: -Dx.version=99.0.1')" "1 FAIL"
	t "image no command" "$(img lua-5.4 5.4.7)" "1 FAIL"
	t "image no floor"   "$(img ruby-4.0 4.0.1)" "0 SKIP"
	t "image no runtime" "$(img minimal '')" "0 SKIP"
	t "image unknown"    "$(img ruby-3.3 3.3.14)" "1 FAIL"
	t "image misspelt"   "$(img ruby3.4 3.4.13)" "1 FAIL"
	t "image not listed" "$(img wasm '')" "1 FAIL"

	if [ "$fail" -ne 0 ]; then
		exit 1
	fi

	echo "PASS self-test: $n cases"
}

case "${1:-}" in
--self-test)
	[ $# -eq 1 ] || usage
	self_test
	exit 0
	;;
--check-eol)
	[ $# -le 2 ] || usage
	check_eol "${2:-}"
	exit
	;;
-*|'')
	usage
	;;
esac

[ $# -ge 2 ] && [ $# -le 3 ] || usage

if check_image "$1" "$2" "${3:-$default_eol}"; then
	exit 0
fi
exit 1
