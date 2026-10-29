This document describes the release process for FreeUnit (ex NGINX Unit) and as such is
likely only of interest to FreeUnit maintainers.

# Create a preparatory branch

You should create a new branch for doing this work. E.g.

    $ git checkout -b x.y[.z]-prep master


# Create a set of commits

## Set the version

    $ tools/bump-version.sh X.Y.Z

This is the one way to set the version.  The files that carry it are
listed in `.github/scripts/version-sites`, and the script reads that list.
It sets the version in the `version` file (`NXT_VERSION` and
`NXT_VERNUM`), the title of `docs/unit-openapi.yaml`, the three unitctl
crates and their `Cargo.lock` entries, `tools/unitctl/openapi-config.json`
and `tools/unitctl/unit-openapi/README.md`.  It regenerates the
Dockerfiles in pkg/docker with `make -B dockerfiles`, and sets the default
of `UNIT_VERSION` in the three hand-written files in pkg/docker/local/.

It then runs `.github/scripts/check-version.sh`, which names what still
needs a person: docs/changes.xml, CHANGES, the unitctl CHANGELOG and
SECURITY.md (see below).  Edit those, run the check again until it
passes, and commit.  The script does not commit.  The same check runs on
every pull request, on every push to master and on the release tag.

Earlier releases made one commit per file by hand (3144710fe for
unitctl, 4d627c8f8 for unit-openapi.yaml, f7771378f for the Dockerfiles);
do not edit those files by hand any more.

If pkg/eol.json dropped a runtime version, `make dockerfiles` no longer
writes its Dockerfile, and check-version.sh names the file that is left
over; `git rm` it.

## unitctl CHANGELOG

Add a `## [X.Y.Z]` section and its `[X.Y.Z]:` link to
tools/unitctl/CHANGELOG.md.

## SECURITY.md

For a new minor release, make its `X.Y.x` row `✅ Active` and mark the
previous line EOL, or `✅ LTS` if it becomes a long-term line; one row
says Active.  A patch release of a line that is already Active or LTS
needs no change.

## changes.xml

Create a commit that updates the docs/changes.xml for this release.

As well as adding the various entries also update the 'date' and 'time'
fields.

Each entry is a `<change>` with a `type` of bugfix, feature, change,
security or workaround, and one or more `<para>` elements.  A paragraph
becomes a paragraph of the changelog, so use a second `<para>` for the
paragraphs of one entry that stand apart.

## The CHANGES file

The CHANGES file in the root of the tree is generated from
docs/changes.xml:

    $ make -C docs changes

It writes build/CHANGES and copies it over CHANGES.  Never edit CHANGES by
hand: edit docs/changes.xml and run the command again.  A hand edit is lost
the next time the file is generated, and the two files diverge.

The CI job "Check the generated files" runs "make -C docs check" and compares
the generated file with the committed one.  The job fails when the two differ.
Run the target before the release commit.

The packager of a stanza names the product in the title.  A stanza packed
by "FreeUnit Packaging" is titled "Changes with FreeUnit X.Y.Z".  Any other
packager is titled "Changes with Unit X.Y.Z".
The date of a block comes from the 'date' and 'time' fields of its stanza.

The stylesheet that renders the file, docs/changes.xslt, is the compiled
form of docs/changes.xsls.  xslscript.pl, which compiles it, is not
packaged in most distributions, so the compiled file is committed and the
two must be changed together.  If xslscript.pl is at hand, "make -C docs
xslt XSLS=/path/to/xslscript.pl" rebuilds it.

The target "make -C docs stylesheet-check" compares the named templates of
the two files.  It fails when they differ.  The compiler is not available in
most environments, so the target cannot compare the body of a template.
Run it after a change to either file.


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

Do not create the `unitctl/X.Y.Z` tag by hand.  Pushing `X.Y.Z` runs
.github/workflows/unitctl.yml, whose release job creates `unitctl/X.Y.Z`
and its release itself; a tag that already exists makes that step fail.
The same push runs build-deb.yml and release-docker.yml; the latter logs
in to Docker Hub, so the DOCKERHUB_USERNAME and DOCKERHUB_TOKEN secrets
have to be set before the tag is pushed.


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

Immediately after release we should open the next version: add a new
changes header to docs/changes.xml and CHANGES.  Run
`tools/bump-version.sh` for the new version only when the release is
prepared, as above.

See 47d2f933a for an example.


# Appendix: the `version` file and what consumes it

The repository-root `version` file is the single source of truth for the
release number:

    # Copyright (C) FreeUnit Community

    NXT_VERSION=1.36.1
    NXT_VERNUM=13601

- **`NXT_VERSION`** — human-readable dotted string (`MAJOR.MINOR.PATCH`).
- **`NXT_VERNUM`** — the same number for compile-time comparisons:
  `MAJOR * 10000 + MINOR * 100 + PATCH` (e.g. `1.36.1` → `13601`). Keep both in
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

The list lives in `.github/scripts/version-sites`, with the reason each
file carries the version.  Run `.github/scripts/check-version.sh` to
check them all.
