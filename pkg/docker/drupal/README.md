# FreeUnit Drupal core dev kit

One image for Drupal core work: FreeUnit and PHP 8.5 with the extensions
Drupal needs, plus Composer, drupal/coder, SQLite and Xdebug. It serves a
Drupal checkout mounted from the host, and runs the installer, the tests and
the linters against it.

- Image: `Dockerfile.drupal-core-dev-php8.5`, on the FreeUnit `php-8.5` image.
- FreeUnit configuration: [`unit-drupal.json`](unit-drupal.json).
- Entrypoint: [`drupal-dev.sh`](drupal-dev.sh).

## Build

```sh
cd pkg/docker
make build-drupal-core-dev-php8.5          # tag: unit:1.36.1-drupal-core-dev-php8.5
# on top of a locally built base (make build-php-8.5):
make build-drupal-core-dev-php8.5 \
    DEVKIT_BASE_drupal-core-dev-php8.5=unit:1.36.1-php-8.5
```

## Quickstart

From a Drupal core clone:

```sh
docker run -d --name drupal-dev -p 8080:80 \
    -e DRUPAL_AUTO_INSTALL=1 \
    -v "$PWD:/var/www/drupal/web" \
    "$IMAGE"
```

Then open <http://localhost:8080>. The first start runs `composer install`
(needs the network once) and installs the `standard` profile on SQLite. A
core clone has no Drush, so core's `core/scripts/drupal install` is used; it
prints the admin password to `docker logs drupal-dev`. A
`drupal/recommended-project` mounted at `/var/www/drupal` (docroot in `web/`)
is installed with Drush, as `admin`/`admin`.

The container's `www-data` takes the uid and gid of the mounted checkout, so
everything written there stays editable on the host.

## Commands

`docker exec drupal-dev drupal-dev.sh <command>`, or pass the command to
`docker run`, in which case FreeUnit is started in the background first.

| command | what it does |
|---|---|
| `serve` | the default: render the FreeUnit configuration and run `unitd` in the foreground; with `DRUPAL_AUTO_INSTALL=1`, run `si` first if the site is not installed |
| `si [args]` | `composer install` if `vendor/` is missing, then `drush site:install` on SQLite, or core's installer for a core clone; extra arguments go to the installer |
| `test <path> [args]` | PHPUnit with `core/phpunit.xml(.dist)` against this FreeUnit; paths are relative to the docroot |
| `lint [paths]` | `phpcs` with `core/phpcs.xml.dist` (core clone) or `Drupal,DrupalPractice`, then `phpstan` if the project has it; with no paths, the files changed against `HEAD` |
| `shell` | a login shell in the project, as `www-data` |

```sh
docker exec drupal-dev drupal-dev.sh test core/modules/node/tests/src/Functional/NodeCreationTest.php
docker exec drupal-dev drupal-dev.sh lint core/modules/node/src/NodeForm.php
docker exec -it drupal-dev drupal-dev.sh shell
```

| variable | default | |
|---|---|---|
| `DRUPAL_AUTO_INSTALL` | `0` | `serve` installs the site first if needed |
| `DRUPAL_PROFILE` | `standard` | install profile |
| `DRUPAL_ADMIN_USER` / `DRUPAL_ADMIN_PASS` | `admin` / `admin` | Drush installs only |
| `DRUPAL_DB_URL` | `sqlite://sites/default/files/.ht.sqlite` | Drush installs only |
| `DRUPAL_BASE` / `DRUPAL_DOCROOT` | `/var/www/drupal` / `…/web` | where the checkout is mounted |
| `SIMPLETEST_BASE_URL` / `SIMPLETEST_DB` | `http://localhost` / `sqlite://localhost//tmp/test.sqlite` | for `test` |
| `XDEBUG` | `0` | `1` loads Xdebug and raises `limits.timeout` |
| `UNIT_APP_TIMEOUT` / `UNIT_APP_TIMEOUT_XDEBUG` | `300` / `3600` | `limits.timeout` of the Drupal app, in seconds |
| `UNITD` | `unitd` | `unitd-debug` for FreeUnit's debug log |

## The FreeUnit configuration

`unit-drupal.json` follows Drupal's `.htaccess`:

| request | action |
|---|---|
| `*/.ht*`, `/sites/*/files/*.php`, `/sites/*/private/*` | `403` |
| `/vendor/*`, dotfiles other than `/.well-known/*`, Drupal source (`*.module`, `*.inc`, `*.yml`, `*.twig`, …), `composer.json/lock`, backup files | `404` |
| `/core/install.php`, `/core/rebuild.php`, `/update.php`, `/core/modules/statistics/statistics.php` | that script |
| any other `*.php` except `/index.php` | `404` |
| everything else | the static file if there is one, else `index.php` |

OPcache revalidates on every request. The file is rendered into
`/docker-entrypoint.d/` at every start; to use your own, mount it over
`/usr/share/unit/drupal/unit-drupal.json`. The configuration as applied:
`docker exec drupal-dev curl -s --unix-socket /var/run/control.unit.sock http://localhost/config`.

## Xdebug

Loaded only with `XDEBUG=1`:

```sh
docker run -d --name drupal-dev -p 8080:80 \
    -e XDEBUG=1 --add-host=host.docker.internal:host-gateway \
    -v "$PWD:/var/www/drupal/web" "$IMAGE"
```

Defaults: `xdebug.mode=debug,develop`, `start_with_request=trigger`,
`client_host=host.docker.internal`, port 9003; override with `XDEBUG_MODE`,
`XDEBUG_START_WITH_REQUEST`, `XDEBUG_CLIENT_HOST` and `XDEBUG_CLIENT_PORT`.
Trigger a session with the browser extension or `?XDEBUG_TRIGGER=1`; for
PHPUnit, `docker exec -e XDEBUG_TRIGGER=1 drupal-dev drupal-dev.sh test …`.
Map `/var/www/drupal/web` (or `/var/www/drupal` for a recommended-project)
to your workspace in the IDE.

A worker paused on a breakpoint is, to the router, a request that does not
answer, so `XDEBUG=1` raises `limits.timeout` to 3600 s. Re-create the
container to switch Xdebug on or off.

## Cron

Set `DRUPAL_CRON_KEY` to Drupal's cron key (`drush state:get
system.cron_key`) to have FreeUnit run cron itself
([schedules](../../../docs/schedules.md)):

```json
"schedules": {
    "drupal-cron": {
        "pass": "applications/drupal/index",
        "uri": "/cron/<DRUPAL_CRON_KEY>",
        "interval": 300,
        "jitter": 15,
        "timeout": 300,
        "overlap": "skip"
    }
}
```

Unset (the default), no `schedules` member is added.

| variable | default | |
|---|---|---|
| `DRUPAL_CRON_KEY` | unset | Drupal's cron key; setting it adds the schedule |
| `DRUPAL_CRON_INTERVAL` | `300` | seconds between runs |
| `DRUPAL_CRON_JITTER` | `15` | up to this many extra seconds |
| `DRUPAL_CRON_HOST` | unset | `Host` header of the run; without it the run uses `localhost` |

A checkout whose `trusted_host_patterns` does not trust `localhost` needs
`DRUPAL_CRON_HOST`, or the run gets a `400`. The schedule `timeout` is
capped at `limits.timeout` but does not stop PHP: an overrunning run keeps a
worker busy until it finishes. To run cron by hand:
`docker exec -u www-data drupal-dev drush cron`.
