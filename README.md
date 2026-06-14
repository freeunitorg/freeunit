# FreeUnit — Community LTS fork of Unit

[![Project Status: Active – The project has reached a stable, usable state and is being actively developed.](https://www.repostatus.org/badges/latest/active.svg)](https://www.repostatus.org/#active)
[![Build & Test](https://github.com/freeunitorg/freeunit/actions/workflows/build-test.yml/badge.svg)](https://github.com/freeunitorg/freeunit/actions/workflows/build-test.yml "Build & Test")
[![Docker](https://github.com/freeunitorg/freeunit/actions/workflows/release-docker.yml/badge.svg)](https://github.com/freeunitorg/freeunit/actions/workflows/release-docker.yml "Release (Docker Images)")
[![GitHub Discussions](https://img.shields.io/badge/GitHub-discussions-009639)](https://github.com/freeunitorg/freeunit/discussions "GitHub Discussions")

**Free as in freedom.**

Unit application server, continued by the community.

> The original Unit repository was archived in October 2025.
> The maintainers noted: *"A new maintainer is desired."*
> FreeUnit is that maintainer.

Forked from the original Unit project to ensure:
- Long-term security maintenance
- PHP 8.4+ and PHP 8.5+ runtime support
- Predictable release cycle
- Independent, community-driven governance

**In the lineage of:** freenginx · MariaDB · LibreOffice · OpenSSH
— when corporations step back, community takes over.

---

## Universal Web App Server

FreeUnit is a lightweight and versatile open-source server with two primary capabilities:

- serves static media assets
- runs application code in eight languages

Unit compresses several layers of the modern application stack into a potent,
coherent solution with a focus on performance, low latency, and scalability. It
is intended as a universal building block for any web architecture, regardless
of its complexity, from enterprise-scale deployments to your pet's homepage.

Its native [RESTful JSON API](#openapi-specification) enables dynamic
updates with zero interruptions and flexible configuration, while its
out-of-the-box productivity reliably scales to production-grade workloads. We
achieve that with a complex, asynchronous, multithreading architecture
comprising multiple processes to ensure security and robustness while getting
the most out of today's computing platforms.

## Installation

### Docker

Images are published to the GitHub Container Registry (GHCR) on every release
and are available for `linux/amd64` and `linux/arm64`.

| Variant | Image |
|---------|-------|
| minimal | `ghcr.io/freeunitorg/freeunit:latest-minimal` |
| PHP 8.5 | `ghcr.io/freeunitorg/freeunit:latest-php-8.5` |
| PHP 8.4 | `ghcr.io/freeunitorg/freeunit:latest-php-8.4` |
| PHP 8.3 | `ghcr.io/freeunitorg/freeunit:latest-php-8.3` |
| Python 3.14 | `ghcr.io/freeunitorg/freeunit:latest-python-3.14` |
| Python 3.13 | `ghcr.io/freeunitorg/freeunit:latest-python-3.13` |
| Python 3.12 | `ghcr.io/freeunitorg/freeunit:latest-python-3.12` |
| Node.js 26 | `ghcr.io/freeunitorg/freeunit:latest-node-26` |
| Node.js 24 | `ghcr.io/freeunitorg/freeunit:latest-node-24` |
| Node.js 22 | `ghcr.io/freeunitorg/freeunit:latest-node-22` |
| Node.js 20 | `ghcr.io/freeunitorg/freeunit:latest-node-20` |
| Go 1.26 | `ghcr.io/freeunitorg/freeunit:latest-go-1.26` |
| Go 1.25 | `ghcr.io/freeunitorg/freeunit:latest-go-1.25` |
| Ruby 4.0 | `ghcr.io/freeunitorg/freeunit:latest-ruby-4.0` |
| Ruby 3.4 | `ghcr.io/freeunitorg/freeunit:latest-ruby-3.4` |
| WebAssembly | `ghcr.io/freeunitorg/freeunit:latest-wasm` |

Full list of variants (including `python-3.14`, `perl-5.40`, `ruby-3.3`, slim
Python variants, etc.) is in the
[docker workflow](.github/workflows/release-docker.yml).

```console
$ docker pull ghcr.io/freeunitorg/freeunit:latest-minimal
$ mkdir /tmp/unit-control
$ docker run -d \
      --mount type=bind,src=/tmp/unit-control,dst=/var/run \
      --mount type=bind,src=.,dst=/www \
      --network host \
      ghcr.io/freeunitorg/freeunit:latest-minimal
```

### Build from Source

```console
$ git clone https://github.com/freeunitorg/freeunit
$ cd freeunit
$ ./configure --openssl --otel
$ make
$ sudo make unitd-install
```

## Getting Started with `unitctl`

[`unitctl`](tools/README.md) streamlines the management of FreeUnit processes
through an easy-to-use command line interface. Download it from the
[releases page](https://github.com/freeunitorg/freeunit/releases).

```console
$ tar xzvf unitctl-master-x86_64-unknown-linux-gnu.tar.gz
# mv unitctl /usr/local/bin/
```

## Quick Start: PHP

Save `/www/helloworld/index.php`:
```php
<?php echo "Hello, PHP on FreeUnit!"; ?>
```

Configure via Unix socket:
```console
# curl -X PUT --data-binary @config.json \
       --unix-socket /var/run/control.unit.sock \
       http://localhost/config/applications
```

```console
# curl -X PUT -d '{"127.0.0.1:8080": {"pass": "applications/helloworld"}}' \
       --unix-socket /var/run/control.unit.sock \
       http://localhost/config/listeners
```

```console
$ curl 127.0.0.1:8080
Hello, PHP on FreeUnit!
```

## PHP 8.4 and PHP 8.5 Support

FreeUnit provides first-class PHP 8.4 and PHP 8.5 support — the primary motivation for this fork.

```console
$ docker pull ghcr.io/freeunitorg/freeunit:latest-php-8.5
$ docker pull ghcr.io/freeunitorg/freeunit:latest-php-8.4
```

## OpenTelemetry

FreeUnit includes built-in OpenTelemetry support (compiled with `--otel`):

```json
{
  "settings": {
    "telemetry": {
      "endpoint": "http://localhost:4317/v1/traces",
      "protocol": "grpc",
      "sampling_ratio": 1.0,
      "batch_size": 20
    }
  }
}
```

## WebAssembly

FreeUnit supports running WebAssembly Components (WASI 0.2).
For configuration details see the [OpenAPI spec](docs/unit-openapi.yaml).

## OpenAPI Specification

The [OpenAPI specification](docs/unit-openapi.yaml) aims to simplify
configuring and integrating FreeUnit deployments and provides an authoritative
source of knowledge about the control API.

## Community

- **Discussions:** [github.com/freeunitorg/freeunit/discussions](https://github.com/freeunitorg/freeunit/discussions)
- **Issues:** [github.com/freeunitorg/freeunit/issues](https://github.com/freeunitorg/freeunit/issues)
- **Website:** [freeunit.org](https://freeunit.org)
- **Chat:** [t.me/freeunit_support](https://t.me/freeunit_support)
- **Contact:** [team@freeunit.org](mailto:team@freeunit.org)
- **Contributing:** see [CONTRIBUTING.md](CONTRIBUTING.md)
- **Security:** see [SECURITY.md](SECURITY.md)

## License

FreeUnit is distributed under the [Apache 2.0 License](LICENSE),
same as the original Unit project.

---

*Forked from [nginx/unit](https://github.com/nginx/unit) — original authors retain full credit.*
