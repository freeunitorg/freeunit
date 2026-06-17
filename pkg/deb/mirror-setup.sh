# shellcheck shell=bash
# mirror-setup.sh — shared local-mirror redirection for FreeUnit Debian
# packaging. Sourced (not executed) by the container scripts in
# pkg/deb/build-local.sh and bind-mounted at /mirror-setup.sh, mirroring
# sury-setup.sh / smoke-asserts.sh so the logic lives in exactly one place. It
# lets a build/smoke run resolve apt entirely from a local mirror instead of the
# Debian CDN — for reproducible or offline builds. The CI workflow does not
# source this file; it is a build-local.sh convenience.
#
# Usage: source this file, then call apply_deb_mirror once, before the first
# `apt-get update` in a container. Honors $DEB_MIRROR (empty = upstream
# deb.debian.org, the default — a no-op).
#
# DEB_MIRROR is the replacement origin (scheme://host[/path-prefix]) for the
# Debian CDN. A single origin swap redirects both the main archive and
# debian-security, since Debian trixie serves both under deb.debian.org:
#   full mirror:          DEB_MIRROR=http://mirror.lan
#                           -> http://mirror.lan/debian , .../debian-security
#   apt-cacher-ng cache:  DEB_MIRROR=http://cache.lan:3142/deb.debian.org
#                           -> http://cache.lan:3142/deb.debian.org/debian , .../debian-security
# DEB_MIRROR must be a plain http(s) origin URL with no sed metacharacters:
# '#' is the substitution delimiter below, '&' expands to the whole match in the
# replacement, and a newline would inject a second sed command. The guard at the
# top of apply_deb_mirror rejects all three so a malformed value fails loudly
# instead of silently corrupting every rewritten source line.
apply_deb_mirror() {
    [ -n "${DEB_MIRROR:-}" ] || return 0

    case "$DEB_MIRROR" in
        http://*|https://*) ;;
        *) echo "apply_deb_mirror: DEB_MIRROR must be an http(s) origin URL: '$DEB_MIRROR'" >&2; return 1 ;;
    esac
    case "$DEB_MIRROR" in
        *'#'*|*'&'*|*\\*|*'
'*) echo "apply_deb_mirror: DEB_MIRROR contains a sed metacharacter (# & \\ or newline): '$DEB_MIRROR'" >&2; return 1 ;;
    esac

    local f changed=0
    # The trixie base image keeps its sources in the deb822 debian.sources;
    # rewrite any legacy single-file .list too so the helper stays release-
    # agnostic within trixie. Only deb.debian.org is rewritten — trixie (13)
    # serves debian-security from that same origin, so a single swap covers both
    # the main archive and security. NOTE: this single-origin assumption is
    # trixie-and-later only; on bookworm/bullseye security lives on the separate
    # host security.debian.org, which this regex does not match — extend it
    # before reusing the helper against a pre-trixie base.
    for f in /etc/apt/sources.list \
             /etc/apt/sources.list.d/*.sources \
             /etc/apt/sources.list.d/*.list; do
        [ -f "$f" ] || continue
        if grep -qE 'https?://deb\.debian\.org' "$f"; then
            sed -i -E "s#https?://deb\.debian\.org#${DEB_MIRROR}#g" "$f"
            changed=1
        fi
    done

    if [ "$changed" -eq 1 ]; then
        echo "apt: Debian archive -> ${DEB_MIRROR}"
    else
        echo "apt: DEB_MIRROR set but no deb.debian.org source found to rewrite" >&2
    fi
}
