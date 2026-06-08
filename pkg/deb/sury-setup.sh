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
# without touching this file. When the repo is enabled it is also pinned above
# the Debian archive (preferences.d) so the build resolves PHP from one source
# deterministically — see the pin block below for why this stays invisible to
# end users.
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

    apt-get install -y --no-install-recommends ca-certificates curl
    install -d /usr/share/keyrings
    curl -fsSL https://packages.sury.org/php/apt.gpg -o /usr/share/keyrings/sury-php.gpg
    # Codename from /etc/os-release (always present on Debian) instead of pulling
    # in lsb-release; apt reads the binary keyring directly, so no gnupg either.
    local codename
    # shellcheck source=/dev/null  # /etc/os-release is a runtime container file
    codename=$(. /etc/os-release && printf '%s' "$VERSION_CODENAME")
    cat > /etc/apt/sources.list.d/sury-php.sources <<SURYSRC
Types: deb
URIs: https://packages.sury.org/php/
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
    cat > /etc/apt/preferences.d/sury-php.pref <<SURYPIN
Package: *php*
Pin: origin packages.sury.org
Pin-Priority: 600
SURYPIN
    apt-get update
}
