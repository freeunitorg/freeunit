# Contributing to FreeUnit

Thank you for helping keep Unit alive.

## Ways to Contribute

- **Bug reports** — open an issue with reproduction steps
- **Security fixes** — see [SECURITY.md](SECURITY.md)
- **PHP 8.5+ support** — our primary focus
- **Documentation** — fixes and improvements always welcome
- **CI/CD** — help improve our build pipeline

## Getting Started

```console
$ git clone https://github.com/freeunitorg/freeunit
$ cd freeunit
$ ./configure --openssl --otel
$ make
```

## Pull Request Process

1. Fork the repository
2. Create a branch: `git checkout -b fix/your-fix`
3. Make your changes
4. Test your changes
5. Submit a pull request against `master`

## Labels

Every issue and PR must carry labels so triage and release notes stay
accurate. Maintainers (or the contributor, if able) apply at least:

- **One type** — `z-bug 🐞`, `z-enhancement ⬆️`, `z-question`, or the
  upstream `T-Defect` / `T-Enhancement` / `T-Other`.
- **One area** — the language module (`z-php`, `z-python`, `z-rust`, …)
  or `z-c` for core C, `z-infrastructure`, `z-packages`, `z-toolchain`.
- **Severity, when it applies** — `z-crasher` for a segfault/abort,
  `X-Release-Blocker` for anything that must ship in the next release.

Run `gh label list` to see the full set. To apply labels to a PR:

```console
$ gh pr edit <num> --add-label "z-bug 🐞" --add-label "z-c"
```

If `gh pr edit` fails with a Projects-classic deprecation error, use
the REST API instead:

```console
$ gh api repos/freeunitorg/freeunit/issues/<num>/labels -X POST \
      -f "labels[]=z-bug 🐞" -f "labels[]=z-c"
```

PR titles must follow Conventional Commits (see below), not the branch
name — rename a `feature/foo` PR to `feat(scope): …` before merge.

## Code Style

Follow the existing C code style in the project.
Run the test suite before submitting:

```console
$ sudo pytest-3 --print-log test/
```

If you changed core C code or libunit, also run the C test suite:

```console
$ ./configure --tests --openssl
$ make
$ make build/lib/libunit.a
$ make tests
$ ./build/tests
```

`--tests` builds the test programs (`build/tests`,
`build/unit_port_recv_test`, and the rest). It does not change `unitd`,
`libnxt.a`, `libunit.a`, or any language module: those come from the
same objects with or without `--tests`.

If you touch a source file under `#if (NXT_TESTS)`, run the gate script
after `make tests`. It fails if a test hook reaches a shipped artifact:

```console
$ .github/scripts/check-test-hooks.sh
```

Build once with `./configure --hardening=strict` before you submit.
New code must compile warning-free under it: no variable-length
arrays, and `nxt_fallthrough;` instead of a fall-through comment. For
length arithmetic on untrusted input, use `nxt_size_add()` and
`nxt_size_mul()` from `src/nxt_checked.h`. For record parsing, use
`nxt_span_t` from `src/nxt_span.h`.

## Commit Messages

Use conventional commits format:

```
fix: correct PHP 8.5 SAPI initialization
feat: add otel trace_id to access log
docs: update installation instructions
```

## Community

- **Discussions:** github.com/freeunitorg/freeunit/discussions
- **Chat:** [t.me/freeunit_support](https://t.me/freeunit_support)
- **Contact:** team@freeunit.org

## License

By contributing, you agree your contributions will be licensed
under the [Apache 2.0 License](LICENSE).
