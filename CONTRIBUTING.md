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

## Code Style

Follow the existing C code style in the project.
Run the test suite before submitting:

```console
$ sudo pytest-3 --print-log test/
```

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
