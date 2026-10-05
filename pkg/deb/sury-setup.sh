# shellcheck shell=bash
# sury-setup.sh — shared deb.sury.org enablement helper for FreeUnit Debian
# packaging. Sourced (not executed) by .github/workflows/build-deb.yml and
# pkg/deb/build-local.sh so the logic lives in exactly one place.
#
# Usage: source this file, then call
#   setup_sury_if_needed "<space-separated php versions, e.g. 8.3 8.4>"
#
# It enables the deb.sury.org PHP repository only when a requested
# libphpX.Y-embed runtime is not already available from the base apt sources.
# Honors $SURY: auto (default, detect per version), on (always), off (never).
# Debian trixie main ships a single PHP line, so multi-version PHP normally
# needs sury; a release that carries the version natively makes auto a no-op
# without touching this file. Honors $SURY_MIRROR (empty = upstream
# packages.sury.org): set it to resolve the sury source and key from a local
# mirror for offline/reproducible builds. When the repo is enabled it is also pinned above
# the Debian archive (preferences.d) so the build resolves PHP from one source
# deterministically — see the pin block below for why this stays invisible to
# end users.

# Retry an apt-get invocation across transient mirror/network hiccups. A single
# momentary fetch failure (the sury mirror or deb.debian.org) otherwise aborts
# the whole script under `set -e`, which surfaced as spurious smoke failures.
# Retries with linear backoff; the final attempt's exit status propagates so a
# genuinely broken apt still fails loudly. Defined here because this file is
# sourced before any apt-get on every build/smoke path, so the smoke scripts
# reuse it without redefining it.
apt_retry() {
    local i=1
    while [ "$i" -lt 5 ]; do
        if "$@"; then
            return 0
        fi
        echo "apt_retry: '$*' failed (attempt ${i}/5); retrying in $((i * 3))s" >&2
        sleep "$((i * 3))"
        i=$((i + 1))
    done
    "$@"
}

setup_sury_if_needed() {
    local need="${1:-}" mode="${SURY:-auto}" v cand missing=0

    case "$mode" in
        off) echo "sury: disabled (SURY=off)"; return 0 ;;
        on)  echo "sury: forced on (SURY=on)" ;;
        auto)
            for v in $need; do
                cand=$(apt-cache policy "libphp${v}-embed" 2>/dev/null \
                       | awk '/Candidate:/ {print $2}')
                if [ -z "$cand" ] || [ "$cand" = "(none)" ]; then
                    echo "sury: libphp${v}-embed absent from base sources -> enabling"
                    missing=1
                fi
            done
            if [ "$missing" -eq 0 ]; then
                echo "sury: requested PHP runtimes already available -> not enabling"
                return 0
            fi ;;
        *) echo "sury: invalid SURY='$mode' (auto|on|off)" >&2; return 1 ;;
    esac

    apt_retry apt-get install -y --no-install-recommends ca-certificates curl
    install -d /usr/share/keyrings
    # SURY_MIRROR (empty = upstream) replaces the packages.sury.org base for both
    # the signing key and the apt source, so an offline/local build can resolve
    # php8.3/8.5 from a mirror. The pin host below is derived from the same base.
    local sury_base="${SURY_MIRROR:-https://packages.sury.org}"
    local sury_host="${sury_base#*://}"; sury_host="${sury_host%%/*}"; sury_host="${sury_host%%:*}"
    # The key fetched below becomes apt's trust anchor (Signed-By). Over a non-https
    # base its transport is unauthenticated, so an integrity pin is mandatory there:
    # refuse rather than trust a key an http mirror — or a MITM — could swap. The
    # https default/base keeps web-PKI transport trust, so the pin stays optional.
    case "$sury_base" in
        https://*) : ;;
        *)
            if [ -z "${SURY_KEY_SHA256:-}" ]; then
                echo "sury: refusing to fetch the signing key over non-https '$sury_base' without SURY_KEY_SHA256" >&2
                return 1
            fi ;;
    esac
    curl -fsSL "${sury_base}/php/apt.gpg" -o /usr/share/keyrings/sury-php.gpg
    # Unlike the deb.debian.org rewrite — where only data moves over the mirror
    # and apt verifies Release signatures against the pre-installed Debian keyring
    # — here the trust anchor itself (the signing key apt then trusts via
    # Signed-By below) is fetched over SURY_MIRROR, possibly plain http. Optional
    # SURY_KEY_SHA256 pins its integrity over any transport, symmetric with the
    # RUSTUP_INIT_SHA256 guard in build-local.sh; empty (default) keeps the prior
    # TLS-only trust for the upstream https base.
    if [ -n "${SURY_KEY_SHA256:-}" ]; then
        echo "${SURY_KEY_SHA256}  /usr/share/keyrings/sury-php.gpg" | sha256sum -c -
    fi
    # Codename from /etc/os-release (always present on Debian) instead of pulling
    # in lsb-release; apt reads the binary keyring directly, so no gnupg either.
    local codename
    # shellcheck source=/dev/null  # /etc/os-release is a runtime container file
    codename=$(. /etc/os-release && printf '%s' "$VERSION_CODENAME")
    cat > /etc/apt/sources.list.d/sury-php.sources <<SURYSRC
Types: deb
URIs: ${sury_base}/php/
Suites: ${codename}
Components: main
Signed-By: /usr/share/keyrings/sury-php.gpg
SURYSRC
    # Pin deb.sury.org above the Debian archive for PHP packages so the build
    # resolves every PHP line from one consistent source. A priority pin wins
    # regardless of which side carries the higher version, so the build-time
    # runtime no longer flips between trixie and sury when their version numbers
    # cross. This only affects php8.4 (the single line trixie ships too); 8.3 and
    # 8.5 exist only in sury. The runtime Depends is declared by package name
    # without a version, so this never leaks into what users must install: on
    # plain trixie unit-php8.4 still resolves libphp8.4-embed natively.
    # apt's "origin" pin matches the source host, not the Release Origin field,
    # so it tracks SURY_MIRROR: default packages.sury.org, else the mirror host.
    cat > /etc/apt/preferences.d/sury-php.pref <<SURYPIN
Package: *php*
Pin: origin ${sury_host}
Pin-Priority: 600
SURYPIN
    apt_retry apt-get update
}
