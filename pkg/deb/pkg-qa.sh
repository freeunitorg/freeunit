# shellcheck shell=bash
# pkg-qa.sh — shared Debian-package QA gates for FreeUnit packaging.
#
# Sourced (not executed) by pkg/deb/build-local.sh and the build-deb.yml CI
# workflow, mirroring smoke-asserts.sh / sury-setup.sh / mirror-setup.sh so the
# package-level QA lives in exactly one place and both the local runner and CI
# exercise identical gates.
#
# Contract for every gate below:
#   - env BRAND, RUNTIME, VERSION are set (package identity + expected version);
#   - $DEBS_DIR (default /debs) holds the built *.deb set;
#   - apt sources are configured and the caller applied any local mirror first.
#     Mirror application is the caller's responsibility: build-local.sh sources
#     mirror-setup.sh and runs apply_deb_mirror before delegating here, whereas
#     the CI workflow deliberately skips it (DEB_MIRROR is a build-local.sh-only
#     convenience; CI always resolves apt from the upstream Debian archive). The
#     gates themselves are identical either way;
#   - for pkg_lifecycle / pkg_dropin_upgrade the caller has already written a
#     policy-rc.d that blocks service starts (no init system in a container) and
#     run `apt-get update`.
# Each gate installs only the extra QA tooling it needs (lintian, systemd) and
# returns non-zero on a hard failure, so a caller running under `set -e` aborts.
#
# shellcheck disable=SC2012  # `ls <glob> | head -n1` is deliberate: .deb names
# here are controlled (BRAND_VERSION...arch.deb, never whitespace) and we want a
# glob match's first hit; `find` would be heavier for no gain.
: "${DEBS_DIR:=/debs}"

# Control-field sanity (drop-in replacement relies on Provides/Conflicts/
# Replaces), a residual-brand scan of the -dev pkg-config file, and a non-fatal
# lintian pass over every produced .deb. Installs lintian on top.
pkg_qa_control_lintian() {
    local core dev pc_list field val
    core="$(ls "${DEBS_DIR}"/"${BRAND}"_"${VERSION}"*.deb 2>/dev/null | head -n1)"
    [ -n "$core" ] || { echo "FAIL: no core ${DEBS_DIR}/${BRAND}_${VERSION}*.deb to QA"; return 1; }

    # Drop-in-replacement contract: renaming unit -> freeunit relies on
    # Provides/Conflicts/Replaces so the new package supersedes the old cleanly.
    echo "=== .deb control fields ($(basename "$core")) ==="
    for field in Depends Provides Conflicts Replaces; do
        val="$(dpkg-deb -f "$core" "$field" || true)"
        printf '%s: %s\n' "$field" "${val:-<empty>}"
        [ -n "$val" ] \
            || echo "WARN: control field $field is empty (drop-in replacement relies on Provides/Conflicts/Replaces)"
    done

    # Residual-brand scan of the -dev pkg-config file: on a rebrand the .pc must
    # be ${RUNTIME}.pc under the multiarch pkgconfig dir, never the upstream
    # unit.pc (caught lintian pkg-config-multi-arch-wrong-dir and a naming leak).
    dev="$(ls "${DEBS_DIR}"/"${BRAND}"-dev_"${VERSION}"*.deb 2>/dev/null | head -n1)"
    if [ -n "$dev" ]; then
        echo "=== -dev pkg-config scan ($(basename "$dev")) ==="
        pc_list="$(dpkg-deb -c "$dev" | awk '{print $NF}' | grep -E '/pkgconfig/[^/]+\.pc$' || true)"
        printf '%s\n' "$pc_list"
        if [ "${RUNTIME}" != unit ]; then
            printf '%s\n' "$pc_list" | grep -qE '/unit\.pc$' \
                && { echo "FAIL: -dev still ships unit.pc on a RUNTIME=${RUNTIME} build"; return 1; }
            printf '%s\n' "$pc_list" | grep -qE "/${RUNTIME}\.pc\$" \
                || { echo "FAIL: -dev missing ${RUNTIME}.pc"; return 1; }
        fi
        printf '%s\n' "$pc_list" | grep -qE '/usr/lib/[^/]+/pkgconfig/[^/]+\.pc$' \
            || echo "WARN: .pc not under /usr/lib/<triplet>/pkgconfig (pkg-config-multi-arch-wrong-dir may recur)"
        echo "-dev pkg-config scan: PASS"
    fi

    # lintian on every produced .deb. Inherited upstream packaging may carry
    # pre-existing tags, so errors are surfaced loudly but kept non-fatal.
    echo "=== lintian (errors only; informational) ==="
    apt-get update >/dev/null
    apt-get install -y --no-install-recommends lintian >/dev/null
    lintian --fail-on error --tag-display-limit 0 "${DEBS_DIR}"/"${BRAND}"*_"${VERSION}"*.deb \
        || echo "WARN: lintian reported errors (see above)"
    echo "package QA done"
}

# Package lifecycle: install -> remove -> purge -> reinstall over the core .deb,
# asserting conffile cleanup on purge and an idempotent (re-entrant) postinst on
# reinstall. Then a fatal systemd ExecStart-binary assertion and a non-fatal
# systemd-analyze verify on the packaged unit. There is no postrm, so the system
# user/group are retained on purge by design (Debian practice for accounts that
# may still own files).
pkg_lifecycle() {
    local core svc_file exec_bin unit_file
    core="$(ls "${DEBS_DIR}"/"${BRAND}"_"${VERSION}"*.deb 2>/dev/null | head -n1)"
    [ -n "$core" ] || { echo "FAIL: no core ${DEBS_DIR}/${BRAND}_${VERSION}*.deb for lifecycle"; return 1; }

    echo "=== install ==="
    apt-get install -y --no-install-recommends "$core"
    test -x "/usr/sbin/${RUNTIME}d" || { echo "FAIL: daemon not installed"; return 1; }
    getent passwd "${RUNTIME}" >/dev/null || { echo "FAIL: ${RUNTIME} user not created"; return 1; }

    echo "=== remove (config retained) ==="
    apt-get remove -y "${BRAND}"
    [ -x "/usr/sbin/${RUNTIME}d" ] && { echo "FAIL: daemon binary survived remove"; return 1; }

    echo "=== purge (config dropped) ==="
    apt-get purge -y "${BRAND}"
    [ -e "/etc/default/${RUNTIME}" ] && { echo "FAIL: conffile /etc/default/${RUNTIME} survived purge"; return 1; }
    echo "purge dropped conffiles: OK"

    echo "=== reinstall (idempotent postinst) ==="
    apt-get install -y --no-install-recommends "$core"
    test -x "/usr/sbin/${RUNTIME}d" || { echo "FAIL: daemon not reinstalled"; return 1; }
    # The retained user/group must not be duplicated: postinst's getent guards
    # make the second useradd/groupadd a no-op (set -e would already trip on an
    # error).
    [ "$(getent passwd "${RUNTIME}" | wc -l)" -eq 1 ] || { echo "FAIL: duplicate ${RUNTIME} passwd entry after reinstall"; return 1; }
    [ "$(getent group  "${RUNTIME}" | wc -l)" -eq 1 ] || { echo "FAIL: duplicate ${RUNTIME} group entry after reinstall"; return 1; }
    echo "lifecycle remove/purge/reinstall: PASS"

    echo "=== systemd ExecStart binary assertion ==="
    # Fatal, init-system-independent check: the packaged unit's ExecStart must
    # point at the daemon the package actually installs. The smoke tests launch
    # the daemon by hand, so a typo in ExecStart= would otherwise pass every QA
    # stage. Parse the shipped unit file directly (no boot required).
    svc_file="$(dpkg -L "${BRAND}" | grep -E "systemd/system/${RUNTIME}\.service\$" | head -n1)"
    [ -n "$svc_file" ] || { echo "FAIL: ${RUNTIME}.service not packaged"; return 1; }
    exec_bin="$(sed -n 's/^ExecStart=\([^ ]*\).*/\1/p' "$svc_file" | head -n1)"
    [ -n "$exec_bin" ] || { echo "FAIL: no ExecStart= in $svc_file"; return 1; }
    # Must be the daemon this package installs, not merely some executable: a
    # typo like ExecStart=/bin/sh would still be -x and would slip past a bare
    # check.
    [ "$exec_bin" = "/usr/sbin/${RUNTIME}d" ] \
        || { echo "FAIL: ExecStart=$exec_bin (from $svc_file), expected /usr/sbin/${RUNTIME}d"; return 1; }
    [ -x "$exec_bin" ] \
        || { echo "FAIL: ExecStart binary $exec_bin (from $svc_file) is not an installed executable"; return 1; }
    echo "systemd ExecStart assertion: PASS ($exec_bin)"

    echo "=== systemd unit verification ==="
    command -v systemd-analyze >/dev/null 2>&1 \
        || apt-get install -y --no-install-recommends systemd >/dev/null 2>&1 || true
    if command -v systemd-analyze >/dev/null 2>&1; then
        unit_file="$(dpkg -L "${BRAND}" | grep -E "systemd/system/${RUNTIME}\.service\$" | head -n1)"
        # Non-fatal: verify is strict and may flag tags inherited from upstream.
        systemd-analyze verify "$unit_file" \
            && echo "systemd-analyze verify: PASS ($unit_file)" \
            || echo "WARN: systemd-analyze verify flagged $unit_file"
    else
        echo "WARN: systemd-analyze unavailable; skipped unit verification"
    fi
}

# Drop-in upgrade: install a stand-in for the upstream "unit" package, then
# install the freeunit core .deb over it. Its Conflicts/Replaces: unit must make
# apt remove the upstream package and hand the daemon over cleanly. A clean
# Debian base ships no "unit" package (upstream NGINX Unit lives in
# packages.nginx.org, not Debian main), so a real "apt-get install unit" would
# always skip and leave the headline migration path unverified. We instead
# synthesize a minimal "unit" .deb that ships the upstream daemon path and
# systemd unit freeunit must supersede, then assert it is gone afterwards. The
# gate is fail-closed: a failure to build/install the stand-in is fatal.
pkg_dropin_upgrade() {
    # Run the gate body in a subshell so its cleanup trap stays scoped to this
    # gate and never leaks to the caller's shell. dash (the CI `sh -e`) has no
    # RETURN pseudo-signal, so an EXIT trap inside a subshell is the portable
    # equivalent of bash's function-scoped RETURN trap: it fires on every exit
    # path — a mid-way assertion failure or the normal success — yet stays
    # local to the subshell rather than the surrounding shell.
    (
        core="$(ls "${DEBS_DIR}"/"${BRAND}"_"${VERSION}"*.deb 2>/dev/null | head -n1)"
        [ -n "$core" ] || { echo "FAIL: no core ${DEBS_DIR}/${BRAND}_${VERSION}*.deb for upgrade test"; exit 1; }

        echo "=== build synthetic upstream 'unit' stand-in ==="
        arch="$(dpkg --print-architecture)"
        fake_ver=1.34.0-1
        # Private build dir; the EXIT trap below purges the stand-in package and
        # removes its .deb so neither lingers — harmless in the ephemeral --rm
        # container, but keeps the harness clean.
        work="$(mktemp -d)"
        fake="$work/unit-fake"
        deb="$work/unit_${fake_ver}_${arch}.deb"
        trap 'apt-get purge -y unit >/dev/null 2>&1 || true; rm -rf "$work"' EXIT
        mkdir -p "$fake/DEBIAN" "$fake/usr/sbin" "$fake/lib/systemd/system"
        cat > "$fake/DEBIAN/control" <<CTL
Package: unit
Version: ${fake_ver}
Architecture: ${arch}
Maintainer: Synthetic Upstream <noreply@example.invalid>
Description: synthetic stand-in for upstream NGINX Unit
 Built by the freeunit drop-in test to exercise the Conflicts/Replaces takeover;
 not a functional daemon.
CTL
        printf '#!/bin/sh\necho synthetic-unitd\n' > "$fake/usr/sbin/unitd"
        chmod +x "$fake/usr/sbin/unitd"
        printf '[Unit]\nDescription=synthetic unit\n[Service]\nExecStart=/usr/sbin/unitd\n[Install]\nWantedBy=multi-user.target\n' \
            > "$fake/lib/systemd/system/unit.service"
        dpkg-deb --build --root-owner-group "$fake" "$deb"

        echo "=== install synthetic upstream unit ==="
        apt-get install -y --no-install-recommends "$deb"
        test -e /usr/sbin/unitd || { echo "FAIL: synthetic unitd not installed"; exit 1; }
        test -e /lib/systemd/system/unit.service || { echo "FAIL: synthetic unit.service not installed"; exit 1; }
        dpkg-query -W -f '${Status}' unit 2>/dev/null | grep -q '^install ok installed$' \
            || { echo "FAIL: synthetic 'unit' not registered as installed"; exit 1; }

        echo "=== install ${BRAND} over unit (Conflicts/Replaces) ==="
        apt-get install -y --no-install-recommends "$core"
        test -x "/usr/sbin/${RUNTIME}d" || { echo "FAIL: ${RUNTIME}d missing after drop-in install"; exit 1; }
        # The upstream package must have been superseded, not left half-installed.
        if dpkg-query -W -f '${Status}' unit 2>/dev/null | grep -q '^install ok installed$'; then
            echo "FAIL: upstream 'unit' still installed after ${BRAND} drop-in"; exit 1
        fi
        # On a rebranded build the upstream daemon path AND its systemd unit must
        # be gone -- the takeover this test exists to prove supersedes both, not
        # just the binary.
        if [ "${RUNTIME}" != unit ]; then
            [ -e /usr/sbin/unitd ] \
                && { echo "FAIL: stale /usr/sbin/unitd after drop-in over upstream unit"; exit 1; }
            [ -e /lib/systemd/system/unit.service ] \
                && { echo "FAIL: stale upstream unit.service after drop-in over upstream unit"; exit 1; }
        fi
        echo "drop-in upgrade over upstream unit: PASS"
    )
}
