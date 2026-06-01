This document describes the release process for FreeUnit (ex NGINX Unit) and as such is
likely only of interest to FreeUnit maintainers.

# Create a preparatory branch

You should create a new branch for doing this work. E.g.

    $ git checkout -b x.y[.z]-prep master


# Create a set of commits

## unitctl

Create a commit that updates the version of tools/unitctl. There are a
few places where this needs updating, find them with

    $ grep -rn x.y.z tools/unitctl/

See 3144710fe for an example.

## unit-openapi.yaml

Create a commit that updates the version in docs/unit-openapi.yaml

See 4d627c8f8 for an example.

## Dockerfiles

Create a commit that generates new dockerfiles.

    $ cd pkg/docker
    $ make clean
    $ make dockerfiles

    $ git rm/add as required

See f7771378f for an example.

## changes.xml

Create a commit that updates the docs/changes.xml for this release.

As well as adding the various entries also update the 'date' and 'time'
fields.

## Generate the CHANGES file

    $ make -C docs/ changes && mv build/CHANGES .

See 24ed91f40 for an example.


# Merge it

These should be the last commits into the repository before the release
is tagged.


# Tag the release

Once the above has been merged you can tag it with the new version. For
this we create an annotated tag. E.g. On master

    $ git tag -a -m "FreeUnit 1.33.0 release." 1.33.0

This should create a new tag object pointing to the "CHANGES" commit.

The tag can be pushed just as the branch is. E.g.

    $ git push <upstream> 1.33.0


# A new 'Release'

After a while the new release should show up at
<https://github.com/freeunitorg/freeunit/releases>


# Tarball

We need to publish an archive of the source and a checksum.

    $ cd pkg
    $ make dist
    $ rsync -tv unit-X.Y.Z.tar.* dev:/data/www/freeunit.org/download/


# Docs

The unit-docs repository needs a copy of CHANGES under
source/CHANGES.txt


# Post release

Immediately after release we should bump the version of Unit by editing
the version file and docs/changes.xml to add a new changes header.

See e67d74332 for an example.


# Appendix: the `version` file and what consumes it

The repository-root `version` file is the single source of truth for the
release number:

    # Copyright (C) FreeUnit Community

    NXT_VERSION=1.35.6
    NXT_VERNUM=13506

- **`NXT_VERSION`** — human-readable dotted string (`MAJOR.MINOR.PATCH`).
- **`NXT_VERNUM`** — the same number for compile-time comparisons:
  `MAJOR * 10000 + MINOR * 100 + PATCH` (e.g. `1.35.6` → `13506`). Keep both in
  lockstep; a mismatch is a silent bug.

## Consumers

- **Build:** `configure` sources `. ./version`; `auto/make` generates
  `build/include/nxt_version.h` (`#define NXT_VERSION` / `NXT_VERNUM`). Every
  object lists that header as a prerequisite, so a bump recompiles everything
  that embeds the version (`Server:` header, `unitd --version`, libunit).
- **Packaging:** `pkg/Makefile` (`VERSION ?= $(NXT_VERSION)`) names the source
  tarball; `pkg/{deb,rpm,docker,npm}/Makefile` each `include ../../version` for
  the package version and the docker image tag.
- **Language modules:** `auto/modules/{java,nodejs}` embed `$NXT_VERSION` into
  artifact names (`*.jar`, `unit-http-*.tgz`, `package.json`).

## Files that must move in lockstep with a bump

1. `version` — `NXT_VERSION` **and** `NXT_VERNUM`.
2. `CHANGES` — new `Changes with FreeUnit X.Y.Z   DD Mon YYYY` block at the top.
3. `docs/changes.xml` — the two `<changes>` blocks (the `unit` block and the
   per-module block), with matching `ver=` and `date=`.
4. `docs/unit-openapi.yaml` line 3 — `title: "FreeUnit X.Y.Z (ex NGINX Unit)"`.
   This line is **not** auto-generated and is the one most often forgotten.

Quick consistency check:

    grep -nE 'NXT_VERSION=|NXT_VERNUM=' version
    grep -m1 'Changes with FreeUnit' CHANGES
    grep -m2 'ver=' docs/changes.xml
    sed -n '3p' docs/unit-openapi.yaml
