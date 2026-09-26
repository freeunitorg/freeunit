#!/bin/bash
#
# FreeUnit Drupal core dev kit entrypoint (see README.md).
#
#   serve              run FreeUnit in the foreground (the default);
#                      with DRUPAL_AUTO_INSTALL=1, run "si" first if needed
#   si [args]          composer install if needed, then install Drupal on SQLite
#   test <path> [...]  run PHPUnit against the running FreeUnit
#   lint [paths...]    phpcs (drupal/coder), then phpstan when the project has it
#   shell              an interactive shell in the project, as the web user
#   anything else      exec'd as given

set -euo pipefail

DRUPAL_BASE=${DRUPAL_BASE:-/var/www/drupal}
DRUPAL_DOCROOT=${DRUPAL_DOCROOT:-$DRUPAL_BASE/web}
DRUPAL_PROFILE=${DRUPAL_PROFILE:-standard}
DRUPAL_ADMIN_USER=${DRUPAL_ADMIN_USER:-admin}
DRUPAL_ADMIN_PASS=${DRUPAL_ADMIN_PASS:-admin}
DRUPAL_DB_URL=${DRUPAL_DB_URL:-sqlite://sites/default/files/.ht.sqlite}

# Drupal cron as a FreeUnit schedule; unset adds no "schedules" member.
DRUPAL_CRON_KEY=${DRUPAL_CRON_KEY:-}
DRUPAL_CRON_INTERVAL=${DRUPAL_CRON_INTERVAL:-300}
DRUPAL_CRON_JITTER=${DRUPAL_CRON_JITTER:-15}
DRUPAL_CRON_HOST=${DRUPAL_CRON_HOST:-}

WEB_USER=${WEB_USER:-www-data}
UNITD=${UNITD:-unitd}
UNIT_CONTROL=${UNIT_CONTROL:-/var/run/control.unit.sock}
UNIT_CONF_TEMPLATE=${UNIT_CONF_TEMPLATE:-/usr/share/unit/drupal/unit-drupal.json}
UNIT_CONF=${UNIT_CONF:-/docker-entrypoint.d/unit-drupal.json}
UNIT_APP_TIMEOUT=${UNIT_APP_TIMEOUT:-300}
UNIT_APP_TIMEOUT_XDEBUG=${UNIT_APP_TIMEOUT_XDEBUG:-3600}

XDEBUG=${XDEBUG:-0}
XDEBUG_INI=/usr/local/etc/php/conf.d/zz-drupal-dev-xdebug.ini

export SIMPLETEST_BASE_URL=${SIMPLETEST_BASE_URL:-http://localhost}
export SIMPLETEST_DB=${SIMPLETEST_DB:-sqlite://localhost//tmp/test.sqlite}
export COMPOSER_HOME=${COMPOSER_HOME:-/tmp/composer-home}
export COMPOSER_CACHE_DIR=${COMPOSER_CACHE_DIR:-/tmp/composer-cache}

log() { echo "drupal-dev: $*" >&2; }
die() { log "error: $*"; exit 1; }


# composer.json is at the base for a recommended-project, at the docroot for
# a core clone.
project_root()
{
    local d

    for d in "$DRUPAL_BASE" "$DRUPAL_DOCROOT"; do
        [ -f "$d/composer.json" ] && { echo "$d"; return 0; }
    done

    die "no composer.json in $DRUPAL_BASE or $DRUPAL_DOCROOT"
}


# Give the web user the owner of the bind-mounted checkout, so what Drupal,
# Composer and PHPUnit write stays editable on the host.  The docroot is in
# the mount either way (README.md); the base is the image's own for a core
# clone.
match_owner()
{
    local dir uid gid

    [ "$(id -u)" = 0 ] || return 0

    dir=$DRUPAL_DOCROOT
    [ -d "$dir" ] || dir=$DRUPAL_BASE
    [ -d "$dir" ] || return 0

    uid=$(stat -c %u "$dir")
    gid=$(stat -c %g "$dir")

    [ "$uid" != 0 ] || return 0
    [ "$(id -u "$WEB_USER")" = "$uid" ] || usermod -o -u "$uid" "$WEB_USER"
    [ "$(id -g "$WEB_USER")" = "$gid" ] || groupmod -o -g "$gid" "$WEB_USER"
}


as_web()
{
    mkdir -p "$COMPOSER_HOME" "$COMPOSER_CACHE_DIR"
    chown "$WEB_USER:" "$COMPOSER_HOME" "$COMPOSER_CACHE_DIR" 2>/dev/null || true

    if [ "$(id -u)" = 0 ]; then
        HOME=/tmp setpriv --reuid="$WEB_USER" --regid="$WEB_USER" \
            --init-groups -- "$@"
    else
        "$@"
    fi
}


xdebug_setup()
{
    if [ "$XDEBUG" != 1 ]; then
        rm -f "$XDEBUG_INI"
        return
    fi

    cat > "$XDEBUG_INI" <<EOF
zend_extension=xdebug
xdebug.mode=${XDEBUG_MODE:-debug,develop}
xdebug.start_with_request=${XDEBUG_START_WITH_REQUEST:-trigger}
xdebug.client_host=${XDEBUG_CLIENT_HOST:-host.docker.internal}
xdebug.client_port=${XDEBUG_CLIENT_PORT:-9003}
xdebug.discover_client_host=0
xdebug.log_level=0
EOF
}


# Render the FreeUnit configuration.  A worker paused on a breakpoint is a
# request that does not answer, so XDEBUG=1 raises limits.timeout.
unit_conf()
{
    local timeout=$UNIT_APP_TIMEOUT cron

    [ "$XDEBUG" != 1 ] || timeout=$UNIT_APP_TIMEOUT_XDEBUG

    mkdir -p "$(dirname "$UNIT_CONF")"

    # shellcheck disable=SC2016  # PHP source, expanded by PHP.
    TEMPLATE="$UNIT_CONF_TEMPLATE" OUT="$UNIT_CONF" TIMEOUT="$timeout" \
    DOCROOT="${DRUPAL_DOCROOT%/}" WEB_USER="$WEB_USER" \
    CRON_KEY="$DRUPAL_CRON_KEY" CRON_INTERVAL="$DRUPAL_CRON_INTERVAL" \
    CRON_JITTER="$DRUPAL_CRON_JITTER" CRON_HOST="$DRUPAL_CRON_HOST" \
    php -n -r '
        $raw = file_get_contents(getenv("TEMPLATE"));
        $raw = str_replace("/var/www/drupal/web", getenv("DOCROOT"), $raw);
        $conf = json_decode($raw, true, 512, JSON_THROW_ON_ERROR);
        $timeout = (int) getenv("TIMEOUT");
        $app = &$conf["applications"]["drupal"];
        $app["limits"]["timeout"] = $timeout;
        $app["user"] = $app["group"] = getenv("WEB_USER");

        if (getenv("CRON_KEY") !== "") {
            $interval = (int) getenv("CRON_INTERVAL");
            $schedule = [
                "pass" => "applications/drupal/index",
                "uri" => "/cron/" . getenv("CRON_KEY"),
                "interval" => $interval,
                "jitter" => (int) getenv("CRON_JITTER"),
                // Never past limits.timeout: a timeout does not stop PHP.
                "timeout" => $interval > 0 ? min($timeout, $interval)
                                           : $timeout,
                "overlap" => "skip",
            ];

            if (getenv("CRON_HOST") !== "") {
                $schedule["headers"] = ["Host" => getenv("CRON_HOST")];
            }

            $conf["schedules"]["drupal-cron"] = $schedule;
        }

        file_put_contents(getenv("OUT"), json_encode($conf,
            JSON_PRETTY_PRINT | JSON_UNESCAPED_SLASHES) . "\n");
    '

    cron=${DRUPAL_CRON_KEY:+, cron every ${DRUPAL_CRON_INTERVAL}s}
    log "config rendered to $UNIT_CONF (limits.timeout=${timeout}s$cron)"
}


# docker-entrypoint.sh only applies a configuration to an empty state
# directory, and this one is rendered on every start.
prepare()
{
    match_owner
    xdebug_setup
    unit_conf

    [ -S "$UNIT_CONTROL" ] || rm -rf /var/lib/unit/*
}


unit_running()
{
    curl -fs --unix-socket "$UNIT_CONTROL" http://localhost/ >/dev/null 2>&1
}


# Start FreeUnit in the background for a subcommand run as the container's
# command.
ensure_unit()
{
    unit_running && return 0

    prepare
    /usr/local/bin/docker-entrypoint.sh "$UNITD" --control "unix:$UNIT_CONTROL"

    for _ in $(seq 20); do
        unit_running && return 0
        sleep 0.5
    done

    die "FreeUnit did not come up; see /var/log/unit.log"
}


cmd_si()
{
    local project

    project=$(project_root)
    match_owner

    if [ ! -f "$project/vendor/autoload.php" ]; then
        (cd "$project" && as_web composer install --no-interaction --no-progress)
    fi

    as_web mkdir -p "$DRUPAL_DOCROOT/sites/default/files"
    cd "$DRUPAL_DOCROOT"

    if [ -x "$project/vendor/bin/drush" ]; then
        as_web "$project/vendor/bin/drush" -y site:install "$DRUPAL_PROFILE" \
            --db-url="$DRUPAL_DB_URL" --account-name="$DRUPAL_ADMIN_USER" \
            --account-pass="$DRUPAL_ADMIN_PASS" "$@"
    else
        # A core clone has no Drush; core's installer also uses SQLite.
        as_web php core/scripts/drupal install "$DRUPAL_PROFILE" \
            --no-interaction "$@"
    fi
}


cmd_test()
{
    local project config=core/phpunit.xml

    [ $# -ge 1 ] || die "usage: test <path> [phpunit args...]"

    project=$(project_root)
    [ -x "$project/vendor/bin/phpunit" ] || die "no vendor/bin/phpunit; run 'si' first"

    cd "$DRUPAL_DOCROOT"
    [ -f "$config" ] || config=core/phpunit.xml.dist

    ensure_unit

    export BROWSERTEST_OUTPUT_DIRECTORY=${BROWSERTEST_OUTPUT_DIRECTORY:-$DRUPAL_DOCROOT/sites/simpletest/browser_output}
    as_web mkdir -p "$BROWSERTEST_OUTPUT_DIRECTORY"
    as_web "$project/vendor/bin/phpunit" -c "$config" "$@"
}


cmd_lint()
{
    local project phpcs standard=Drupal,DrupalPractice conf="" c rc=0
    local -a paths=("$@")

    project=$(project_root)
    cd "$project"

    if [ ${#paths[@]} = 0 ]; then
        # The files changed against HEAD.
        mapfile -t paths < <({
            git -c safe.directory='*' diff --name-only --diff-filter=ACMR HEAD
            git -c safe.directory='*' ls-files --others --exclude-standard
        } 2>/dev/null | sort -u)
        [ ${#paths[@]} -gt 0 ] || { log "no changed files to lint"; return 0; }
    fi

    phpcs=$project/vendor/bin/phpcs
    [ -x "$phpcs" ] || phpcs=/opt/drupal-tools/vendor/bin/phpcs

    # A core clone lints with core's own ruleset.
    if [ "$project" = "$DRUPAL_DOCROOT" ] && [ -f core/phpcs.xml.dist ]; then
        standard=$DRUPAL_DOCROOT/core/phpcs.xml.dist
    fi

    as_web "$phpcs" -p --standard="$standard" \
        --extensions=php,module,inc,install,test,profile,theme,info,yml \
        "${paths[@]}" || rc=$?

    [ -x vendor/bin/phpstan ] || return $rc

    for c in "$DRUPAL_DOCROOT/core/phpstan.neon.dist" phpstan.neon \
             phpstan.neon.dist
    do
        if [ -f "$c" ]; then conf=$c; break; fi
    done

    as_web vendor/bin/phpstan analyse --no-progress --memory-limit=2G \
        ${conf:+-c "$conf"} "${paths[@]}" || rc=$?

    return $rc
}


cmd_shell()
{
    cd "$(project_root 2>/dev/null || echo "$DRUPAL_BASE")"
    match_owner

    if [ "$(id -u)" = 0 ]; then
        export HOME=/tmp
        exec setpriv --reuid="$WEB_USER" --regid="$WEB_USER" --init-groups \
            -- bash -l
    fi

    exec bash -l
}


cmd=${1:-serve}
[ $# = 0 ] || shift

case "$cmd" in
    serve)
        if [ "${DRUPAL_AUTO_INSTALL:-0}" = 1 ] \
           && ! grep -qs "databases\['default'\]" \
                "$DRUPAL_DOCROOT/sites/default/settings.php"
        then
            (cmd_si "$@")
        fi

        prepare
        exec /usr/local/bin/docker-entrypoint.sh "$UNITD" --no-daemon \
            --control "unix:$UNIT_CONTROL"
        ;;
    si|test|lint|shell)
        "cmd_$cmd" "$@"
        ;;
    *)
        exec "$cmd" "$@"
        ;;
esac
