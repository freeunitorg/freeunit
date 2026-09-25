# Changelog

All notable changes to `unitctl` will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.37.0] - 2026-10-01

### Removed
- JSON5 input. A `.json5` configuration file is refused by name, as hjson and YAML already
  were, and the message names a converter. `edit` reads its temporary file as JSON, so a
  comment written in the editor is now a syntax error.

### Fixed
- `execute -m PUT -p /js_modules/<name>.js -f <file>.js` sends the module
  (`Content-Type: application/javascript`, body verbatim) instead of panicking with
  "Unknown input file type". A file whose type cannot be placed is refused with an error in
  both `execute` and `import` rather than aborting.
- A JSON configuration file is sent as it was written. It was parsed into a map and
  re-serialized, which silently merged duplicate member names and re-spelled numbers; the
  server now sees the file and reports a duplicate member with its line and column.
- `save` no longer tells the operator to restore a configuration that holds non-UTF-8 bytes
  with curl; the control API refuses such a configuration whichever client sends it, so the
  message now names the value that has to be corrected.

### Security
- rustls 0.23.42 → 0.23.45 (RUSTSEC-2026-0285, GHSA-2mjx-qc3c-rqvc), with aws-lc-rs 1.18.1,
  aws-lc-sys 0.45.0 and rustls-webpki 0.103.15.

### Changed
- Version bump to track the FreeUnit 1.37.0 release.

## [1.36.1] - 2026-08-28

### Fixed
- Control API error responses that carry a `location` object (byte offset, line and column for
  parse errors; the JSON Pointer `path` for validation errors) no longer fail to deserialise;
  `unitctl` now prints the server's `detail`, `suggestion` and location instead of a generic JSON
  decoding error.

### Changed
- Version bump to track the FreeUnit 1.36.1 release.
- The `/status` counters in the OpenAPI spec are declared `format: int64`, so the generated
  client models them as 64-bit instead of `i32`.

## [1.36.0] - 2026-07-16

### Added
- `KnownSize::into_full_body()` fallible API for materializing request bodies with proper error handling and actual Content-Length derivation.
- Unit tests for `KnownSize` body conversion, including error handling and short-read scenarios.
- Comprehensive test coverage for binary name lookup optimization.

### Changed
- **Dependencies updated to latest compatible versions:**
  - `base64`: 0.21 → 0.22
  - `bollard`: 0.17 → 0.21 (significant API refactoring)
  - `nu-json`: 0.112 → 0.113
  - `rand`: 0.8 → 0.9 (dev-dependency in unit-client-rs)
  - `sysinfo`: 0.30 → 0.39 (significant API changes)
  - `which`: 5.0 → 8.0
  - `tokio`: added `rt-multi-thread` feature

- **Request body handling**: `streaming_upload_deserialize_response` now uses `KnownSize::into_full_body()` to compute Content-Length from the materialized body, ensuring accuracy when declared sizes differ from actual bytes produced. I/O errors during body materialization are now surfaced instead of silently ignored.

- **Process filtering**: Optimized `UNITD_BINARY_NAMES` lookup in `find_unitd_processes()` by precomputing `OsString` wrappers once before the hot filter loop, reducing per-process allocations.

- **bollard v0.21 migration**:
  - Updated imports from `bollard::container::*` and `bollard::image::*` to `bollard::models::*` and `bollard::query_parameters::*`
  - Migrated `bollard::container::Config` → `bollard::models::ContainerCreateBody`
  - Converted to builder pattern for container/image options (`ListContainersOptionsBuilder`, `CreateImageOptionsBuilder`, etc.)
  - Updated enum types: `MountTypeEnum::BIND` → `MountType::BIND`

- **sysinfo v0.39 migration**:
  - `ProcessRefreshKind::new()` → `ProcessRefreshKind::nothing()`
  - Fixed process name handling (`process.name()` now returns `&OsStr`)
  - Fixed environment and command handling (`process.environ()` and `process.cmd()` return `&[OsString]`)

- **rand v0.9 migration**:
  - Updated imports: `rand::distributions` → `rand::distr`, `DistString` → `SampleString`
  - Replaced deprecated `rand::thread_rng()` with `rand::rng()`

### Fixed
- Non-exhaustive pattern match for `bollard::Error::LegacyClient` variant in error handling.
- Missing `bytes::Buf` trait import for the `.reader()` method in `unit_client.rs`.
- Silent truncation of request bodies on I/O errors in `KnownSize::Read` (now using `expect()` and fallible API).
- Incorrect Content-Length header when request body size differs from declared estimate.
- Compilation errors from major dependency version updates.

## [1.35.5] - 2026

### Previous releases
See earlier versions for additional change history.

[Unreleased]: https://github.com/freeunitorg/freeunit/compare/unitctl/1.37.0...HEAD
[1.37.0]: https://github.com/freeunitorg/freeunit/releases/tag/unitctl/1.37.0
[1.36.1]: https://github.com/freeunitorg/freeunit/releases/tag/unitctl/1.36.1
[1.36.0]: https://github.com/freeunitorg/freeunit/releases/tag/unitctl/1.36.0
[1.35.5]: https://github.com/freeunitorg/freeunit/releases/tag/unitctl/1.35.5
