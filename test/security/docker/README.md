# Docker hardening matrix

`test-hardening.sh` starts the FreeUnit image once per hardening posture with
the same trivial Python application and asserts the outcome.  It is
documentation that runs: the point is not that any of these postures is
required, but that the file cannot go stale about which ones work.

```sh
./test/security/docker/test-hardening.sh                 # published image
./test/security/docker/test-hardening.sh freeunit:local  # a locally built one
```

CI runs it as the last step of `.github/workflows/security-seccomp.yml`,
against the image that workflow has already built from the in-repo Dockerfile.
That workflow also triggers on `src/nxt_capability.*`, `src/nxt_credential.*`,
`src/nxt_isolation.*`, `src/nxt_process.*` and `src/nxt_runtime.*`, since
those are the files that decide what the table below says.

The platform is pinned to `linux/amd64` -- what CI runs, and so that a
multi-arch tag cannot resolve to something else by ambient default.  On an
arm64 host set `UNIT_HARDENING_PLATFORM=linux/arm64`.

## What it establishes

| posture | outcome |
|---|---|
| root, default capabilities | serves 200 |
| root, `--security-opt no-new-privileges` | serves 200 |
| root, `--cap-drop=ALL` | refused, never reaches readiness: `setgid(999) failed (1: Operation not permitted)` |
| `--user 1000:1000` + writable tmpfs on state/run/tmp | serves 200 |
| `--user 1000:1000 --cap-drop=ALL` + tmpfs | serves 200 |
| `--user 1000:1000 --cap-drop=ALL --security-opt no-new-privileges` + tmpfs | serves 200 |

Every cell runs under Docker's **default seccomp profile**, unchanged.

Cells assert a cause, not just an outcome.  The refusal cell greps the
container log for `setgid(999) failed`, because a wholly broken image would
satisfy "no 200" too.  The five serving cells check two things, both anchored to end of line so that
`gid=999` cannot be satisfied by `gid=9990`:

- what the canary reports about **itself** -- `uid`, `gid` *and*
  `os.getgroups()`, since a process that drops its primary gid while keeping
  group 0 has root-group access under another name.  It has to come from the
  application: `docker exec ... id` reports the container's configured user,
  a different process, and would look right even if Unit dropped the
  application to the wrong credentials.
- the **router**'s uid and gid, read from `/proc` *inside* the container,
  because a router still running as root would serve a correctly-dropped
  application perfectly well.  Not `docker top`: that reports host-side uids,
  which under daemon user-namespace remapping are the remapped ones and would
  turn every cell red.  The scan matches the *start* of `cmdline` and skips
  its own pid -- the probing shell's command line contains the string it is
  searching for, and in a non-root cell it runs as 1000, so a substring match
  would have reported the right answer whatever the router did.

Root cells expect `uid=999 gid=999 groups=999` and a router at 999 -- the
image's `unit` user, so the drop is measured rather than assumed.  Non-root
cells expect 1000 throughout.  A 200 on its own says nothing about who served
it, and the flag list and the label describing it did in fact drift apart
once while this was being written.

The wait is two-phase for the same reason.  `docker-entrypoint.sh` runs a
temporary daemon to apply the initial configuration, which activates the
listener, and only then execs the real one; polling immediately can take a
200 from the throwaway and pass while production startup is broken.  Each
cell waits for the entrypoint's "ready for start up" line first.

Three things worth taking away:

- **`--cap-drop=ALL` and root do not mix.**  The daemon's first act is to drop
  the router and applications to the unprivileged `unit` user, and that needs
  CAP_SETGID/CAP_SETUID.  Dropping capabilities is worth doing *after* the
  process is already non-root, not instead of it.
- **`no-new-privileges` is free.**  `setuid` from root is not a privilege gain,
  so the bit never applies to what Unit does; it costs nothing in either
  posture.
- **A bare `--tmpfs /var/lib/unit` is not enough.**  Docker creates a tmpfs
  root-owned and 0755, so a non-root container still gets EACCES on the state,
  run and tmp directories.  The ownership has to be spelled out:
  `--tmpfs /var/lib/unit:uid=1000,gid=1000`.  This is the step people miss.
- **`--user 1000` is not `--user 1000:1000`.**  Uid 1000 has no passwd entry
  in this image, so a bare uid resolves its primary group to GID 0 and the
  container keeps root-group access.  Spell out the group.

## What it does not establish

It does not test the shipped `pkg/docker/seccomp-no-af-alg.json` profile --
that has its own test in `test/security/seccomp/` -- and it makes no claim
that Docker's default seccomp profile blocks `capget` or `mount`.  Measured
against docker 29.7.2 it blocks neither: `capget` is allowed by name in
moby's default profile, and `mount` is gated on CAP_SYS_ADMIN plus the default
AppArmor profile, not on seccomp.  Documentation that says otherwise is wrong.
