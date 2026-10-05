"""Smallest application that proves the whole request path works.

It reports the credentials of the process that served the request.  That is
the only way the hardening matrix can check *who* answered: `docker exec ...
id` would report the container's configured user, which is a different
process and would look right even if Unit dropped to the wrong credentials.

Supplementary groups are part of that: a process can drop its primary gid and
still hold group 0 if setgroups() was skipped, which is root-group access by
another name.
"""

import os


def application(environ, start_response):
    body = ("docker hardening canary uid=%d gid=%d groups=%s\n"
            % (os.getuid(), os.getgid(),
               ",".join(str(g) for g in sorted(os.getgroups())))).encode()
    start_response(
        "200 OK",
        [("Content-Type", "text/plain"), ("Content-Length", str(len(body)))],
    )
    return [body]
