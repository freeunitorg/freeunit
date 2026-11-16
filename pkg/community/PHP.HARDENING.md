# PHP Hardening for FreeUnit

**Checked:** 2026-11-19, against master `708df2a46` and PHP 8.6.0.
**Scope:** PHP applications that run in the `php` module of FreeUnit.

This document tells you how to limit what one HTTP request can do to a PHP
worker. It does not replace a patched PHP.

## 1. Use a patched PHP

A setting does not close a memory corruption bug. Install a PHP version that
has the fix first.

| PHP branch | First safe version |
|------------|--------------------|
| 8.3 | 8.3.37 |
| 8.4 | 8.4.28 |
| 8.5 | 8.5.13 |
| 8.6 | 8.6.0 |

The security release of 2026-11-19 (8.3.37, 8.4.28, 8.5.13, and 8.6.0,
which has the same fixes) closes 18 CVEs. Two of them are remote code
execution through the session extension: a partly overwritten session file
(CVE-2026-126714) and a mismatch of session serializers (CVE-2026-115560).
The NEWS file of each release lists all of them. The earlier fixes, for
example CVE-2026-109618 and CVE-2026-109619, are in [PHP.CVE.md](PHP.CVE.md).
A version in the table has all of them.

To see the PHP version of the module, read the log of `unitd` at start:

```
[notice] ... module: php 8.5.12 "/usr/lib/unit/modules/php.unit.so"
```

The module uses the PHP library that it was built with. A new PHP package
needs a new build of the module, or a module package that matches it.

## 2. Who parses the request body

Unit does not parse `multipart/form-data` and does not parse
`application/x-www-form-urlencoded`. Unit gives the raw body to PHP through
the SAPI `read_post` callback (`src/nxt_php_sapi.c`, `nxt_php_read_post()`).
PHP parses the body when the request starts. It creates `$_POST`, `$_FILES`,
the upload temporary files, and the upload progress data.

So the limits on fields, parts, and files are PHP limits. Set them as PHP ini
values. Unit cannot set them for you.

## 3. Set PHP limits with `options.admin`

Unit applies `options.admin` with `ZEND_INI_SYSTEM` when the PHP worker
starts (`src/nxt_php_sapi.c`, `nxt_php_setup()`). A script cannot change an
`admin` value with `ini_set()`. Use `admin`, not `user`, for limits:
`options.user` applies with `ZEND_INI_USER`, and a script can change those
values.

Example:

```json
{
    "applications": {
        "app": {
            "type": "php",
            "root": "/var/www/app",
            "options": {
                "admin": {
                    "session.upload_progress.enabled": "0",
                    "max_multipart_body_parts": "200",
                    "max_file_uploads": "20",
                    "max_input_vars": "1000",
                    "post_max_size": "8M",
                    "upload_max_filesize": "8M",
                    "memory_limit": "128M"
                }
            }
        }
    }
}
```

| Setting | What it limits | Notes |
|---------|----------------|-------|
| `session.upload_progress.enabled` | The upload progress data that PHP writes to the session while it reads a multipart body. | Set `0` if the application does not show upload progress. The default is `0` from 8.3.37, 8.4.28, 8.5.13 and 8.6.0 on (GHSA-658j-r8rw-q6mx); older versions default to `1`. The FreeUnit Docker images set `0` in `zz-freeunit-security.ini`. |
| `max_multipart_body_parts` | The number of parts in one multipart body. | Exists in PHP 8.0.28, 8.1.16, 8.2.3 and newer. The default `-1` means `max_input_vars` + `max_file_uploads`. |
| `max_file_uploads` | The number of files in one request. | |
| `max_input_vars` | The number of input variables in one request. | PHP stops at the limit and writes a warning. |
| `post_max_size` | The size of the body that PHP accepts for `$_POST` and `$_FILES`. | Keep it at or below `settings.http.max_body_size` (section 4). |
| `upload_max_filesize` | The size of one uploaded file. | Keep it at or below `post_max_size`. |
| `memory_limit` | The memory of one request in the worker. | |

The values above are an example. Select values for your application.

`options.file` can load a separate `php.ini`. `admin` values apply after that
file, so `admin` is the place for the limits that must not change.

### Session defaults of PHP 8.6

PHP 8.6.0 changes the defaults of three session settings. Set them on 8.3,
8.4 and 8.5 too, so that every version behaves in the same way:

| Setting | Default from 8.6.0 | Where to set it |
|---------|--------------------|-----------------|
| `session.use_strict_mode` | `1`: an unknown session ID is replaced, which stops session fixation | `admin`, unless a custom session handler needs IDs that it did not make |
| `session.cookie_httponly` | `1`: JavaScript cannot read the session cookie | `admin`, or `user` if a script must read the cookie |
| `session.cookie_samesite` | `Lax`: the cookie is not sent on most cross-site requests | `user`, so that an application can still select `Strict` or `None` |

```json
"options": {
    "admin": {
        "session.use_strict_mode": "1",
        "session.cookie_httponly": "1"
    },
    "user": {
        "session.cookie_samesite": "Lax"
    }
}
```

PHP 8.6.0 also adds `error_include_args`, off by default. When it is on, an
error message shows the arguments of the function. Keep it off where errors
are logged or shown outside development.

### Check the values

Put a script in the application root and request it:

```php
<?php
foreach (['session.upload_progress.enabled', 'max_multipart_body_parts',
          'max_file_uploads', 'max_input_vars', 'post_max_size',
          'upload_max_filesize', 'memory_limit'] as $k) {
    echo $k, ' = ', var_export(ini_get($k), true), "\n";
}
```

On PHP 8.6 and later, `ini_get_all()` also returns `builtin_default_value`
for each directive, so a check can show which values differ from the PHP
default.

Remove the script after the check. The test
`test_php_application_ini_admin_hardening` in
`test/test_php_application.py` does the same check. It also shows that
`ini_set()` cannot raise an `admin` value.

## 4. What `settings.http.max_body_size` does

`settings.http.max_body_size` is a limit of Unit, in bytes. The default is
8 MB (`src/nxt_router.c`).

It does:

- Return `413` when the `Content-Length` of a request is larger than the
  limit (`src/nxt_http_request.c`).
- Return `413` when a chunked body becomes larger than the limit while Unit
  reads it (`src/nxt_h1proto.c`).

It does not:

- Limit the number of fields, parts, or files in the body.
- Limit the memory that PHP uses to parse the body.
- Apply per application. It applies to all listeners.

A small body with many parts or many variables can still be expensive for
PHP. Use the PHP limits of section 3 for that.

## 5. `limits.requests`: the trade-off

`limits.requests` stops a worker process after it handles that number of
requests (`src/nxt_unit.c`). Unit then starts a new process. The default `0`
means no limit.

```json
{
    "applications": {
        "app": {
            "type": "php",
            "root": "/var/www/app",
            "limits": {
                "requests": 1000
            }
        }
    }
}
```

What you get:

- State that a request leaves in the process does not live forever: memory
  leaks, global state in extensions, and state from a bug of the
  CVE-2026-109619 class.

What it costs:

- A new worker process for every N requests. Unit forks it from the
  application prototype, where PHP is already initialized.
- Per-process resources, for example persistent database connections, open
  again in the new process.

The OPcache is not lost: it is in shared memory, and the new worker uses the
same cache. The test `test_php_application_shared_opcache` in
`test/test_php_application.py` shows this with `limits.requests` set to 1.

What it does not do:

- It does not fix a vulnerability. A crafted request can still corrupt the
  process before the limit. Install a patched PHP (section 1).

A low value costs more process starts. Start with a value in the thousands
and lower it if the state of a process grows over time.

## 6. Checklist

- [ ] PHP is at a safe version (section 1).
- [ ] `options.admin` sets the PHP limits (section 3).
- [ ] `post_max_size` is at or below `settings.http.max_body_size`
      (section 4).
- [ ] `session.upload_progress.enabled` is `0` if the application does not
      use upload progress.
- [ ] `session.use_strict_mode` and `session.cookie_httponly` are `1`, also
      on PHP 8.5 and older (section 3).
- [ ] `limits.requests` is set if the application or its extensions keep
      state between requests (section 5).

## Sources

- [PHP.CVE.md](PHP.CVE.md)
- [PHP ini directives](https://www.php.net/manual/en/ini.list.php)
- [PHP 8.6 UPGRADING, session defaults](https://github.com/php/php-src/blob/php-8.6.0/UPGRADING)
- [PHP file upload ini settings](https://www.php.net/manual/en/ini.core.php#ini.sect.file-uploads)
