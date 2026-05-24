# Changelog

All notable changes to `unitctl` will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.35.6] - 2026-05-24

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

[Unreleased]: https://github.com/nginx/unit/compare/1.35.6...HEAD
[1.35.6]: https://github.com/nginx/unit/releases/tag/1.35.6
[1.35.5]: https://github.com/nginx/unit/releases/tag/1.35.5
