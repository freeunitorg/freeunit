# Syscall drift gate

A CI gate that notices when Unit starts using a kernel interface it did not use
before.  It exists because Unit's portability story is a list of syscalls: the
kernel minimum, what a container's seccomp profile has to allow, and what a
`musl` build does differently from a `glibc` one are all statements about this
list, and nothing kept them honest.

    test/syscalls/capture.sh          run unitd under strace, print its syscalls
    test/syscalls/check.sh            diff a capture against a baseline
    test/syscalls/run-in-docker.sh    do both inside a pinned container
    test/syscalls/baseline-linux-glibc.txt
    test/syscalls/baseline-linux-musl.txt

`.github/workflows/security-syscalls.yml` runs the docker driver for both libcs
on every pull request that touches the build or `src/`.

## The job is advisory for now

The workflow is marked `continue-on-error: true` and its jobs are named
"(advisory)": a difference shows up in the checks list and in the log, but it
cannot block a merge.

That is not timidity, it is honesty about where the baselines came from.  They
were captured on a developer machine, and while the container pins the libc by
digest, the syscalls glibc picks still depend partly on the **host kernel** --
so a runner on an older kernel can legitimately add a fallback syscall the
baseline has never seen.  Until a baseline has been captured from a CI run and
committed, a red gate here would be more likely to be wrong than right.

**Making it blocking** is one commit, and this is the sequence:

1. Let the workflow run on a pull request (or dispatch it).
2. Download the `syscalls-glibc` and `syscalls-musl` artifacts -- they are
   uploaded on every run, including failures, for exactly this.
3. Commit them as the baselines if they differ from what is here, noting in
   the commit message which syscalls moved and why.
4. Drop `continue-on-error` and the "(advisory)" suffix.

## The rule

**A new syscall fails the job.  A disappeared syscall only warns.**

Reaching for a new kernel interface is a decision -- it moves the kernel
minimum, or needs a seccomp allowance, or does not exist on some platform -- so
it should be visible in review, with the baseline updated in the same commit.
Dropping one is never a hazard, and a libc or runner update drops one now and
then without anybody meaning it to.

## Updating a baseline

```sh
test/syscalls/run-in-docker.sh glibc --update
test/syscalls/run-in-docker.sh musl  --update
git add test/syscalls/baseline-linux-*.txt
```

Commit the refreshed baseline together with the change that caused it, and say
in the commit message which syscall appeared and why.  If the capture differs
from what CI sees, take CI's: the workflow uploads its captured list as the
`syscalls-<libc>` artifact on every run, exactly so a baseline can be
reconciled without guessing.

Bumping either pinned image digest in `run-in-docker.sh` is also a baseline
refresh.

## What is exercised, and what is not

`capture.sh` starts `unitd`, PUTs a configuration with one TCP listener and one
`share` route, fetches a static file over it, asserts HTTP 200 and the expected
body, then stops the daemon with `SIGQUIT` -- all inside `strace -f`, so
startup, configuration, request handling and teardown are in one trace.  The
served request is a static `share` route and nothing else: no application, no
language module, no prototype process.  Run as root (which is how it runs in
the container) the router drops to the unprivileged default user, so `setgid`
and `setuid` are in the trace.

`capget` is **not**, and it is worth saying why, because the audit that
prompted this gate assumed otherwise.  `nxt_capability_set()` returns at
`src/nxt_capability.c:51` as soon as `geteuid() == 0` -- root already has what
it needs -- so the `capget` call below it is only reached by a non-root daemon.
Neither baseline contains it.  A non-root capture leg would, and is listed as
a follow-up.

No language application is started, on purpose.  An app process is forked from
Unit and never `exec`s, so its syscalls are indistinguishable in the trace from
Unit's own -- and a CPython or PHP interpreter contributes far more of them
than Unit does, all of it moving with the runtime's own version.  The gate
would then fail on a language patch release, which is noise, not drift.
Covering the module path is a separate job with a separate baseline; see the
follow-ups in the pull request that added this.

The two lists are not directly comparable with each other: 11 syscalls appear
only in the glibc baseline and 6 only in the musl one, because the libcs pick
different syscalls for the same operation (`epoll_create` vs `epoll_create1`,
`clone3` vs `fork`, `openat` vs `open`, and `memfd_create` only on glibc).
That is the reason both are captured.

## Guards

Both scripts refuse a capture with fewer than 20 syscalls, and `capture.sh`
additionally requires a handful of syscalls Unit cannot possibly avoid
(`epoll_ctl`, `socket`, `bind`, `listen`, `read`, `write`, `close`).  A missing
`strace`, a changed output format or a parse that quietly matched nothing all
produce an empty list, and an empty list compares clean against any baseline --
a green check that verified nothing.

`capture.sh` also fails on a nonzero exit from the traced daemon.  `strace`
exits with its tracee's status, so a daemon that crashes during its `SIGQUIT`
teardown would otherwise still hand over a large, plausible list, and every
syscall the teardown never reached would be filed as a harmless warning.

## Requirements

`capture.sh` needs `strace` and `curl` and a built tree.  `run-in-docker.sh`
needs only docker; it installs a toolchain, builds Unit and captures inside a
digest-pinned container.  The digest pins the base image and therefore the
libc, but not the packages installed on top of it -- both are stable releases,
so that moves only on a point release, but it is a real residual.  It passes `--cap-add=SYS_PTRACE`: on a stock daemon strace can
already trace its own descendants without it (verified on docker 29.7.2 with
the default seccomp and AppArmor profiles), but a host with a tightened
`ptrace_scope` or a custom profile needs it, and it costs nothing here.  The
image is always pulled `--platform linux/amd64`: the baselines are per
architecture and an arm64 pull would silently produce a different list.
