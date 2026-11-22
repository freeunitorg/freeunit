# Ruby and Java CVE Map: FreeUnit Docker Images

**Checked:** 2026-11-19
**Scope:** the Ruby and Java images of this repository: `ruby-3.3`,
`ruby-3.4`, `ruby-4.0`, `java-17`, `java-21`, `java-25`, `java-26` and
`java-27`. The list of versions comes from `pkg/eol.json`.

The defects are in the runtimes, not in FreeUnit. An application that
passes request data to the vulnerable code is the trigger. The fix is the
runtime update.

## Ruby: regexp and case conversion, 2026-11-18

Seven advisories in the regexp engine and in String case conversion: heap
overflows, a double free, an integer overflow and out-of-bounds reads.
`upcase`, `downcase` and regexp matching on request data are ordinary
application code, so a crafted parameter can reach them.

| Advisory | Defect |
|----------|--------|
| GHSA-34m4-m9fh-gqp5 | heap buffer overflow in case conversion |
| GHSA-7x2q-68qv-6p8x | double free in regexp compilation |
| GHSA-87fm-7w84-j2px | integer overflow in case conversion |
| GHSA-wpqw-5qxg-m68r | out-of-bounds read in case-insensitive matching |
| GHSA-3p8q-wmhx-vm2w | heap overflow in regexp compilation |
| GHSA-7whj-cfh7-xgm4 | heap overflow in `Regexp#to_s` |
| GHSA-pvgc-g7gj-pv6h | out-of-bounds read in backreference matching |

| Image | Vulnerable | First safe version |
|-------|------------|--------------------|
| `ruby-3.3` | 3.3.13 and older | 3.3.14 |
| `ruby-3.4` | 3.4.0 to 3.4.12 | 3.4.13 |
| `ruby-4.0` | 4.0.0 to 4.0.9 | 4.0.10 |

The base tags `ruby:3.3-trixie`, `ruby:3.4-trixie` and `ruby:4.0-trixie`
carry the fixed releases since 2026-11-19. The next image release is
built on them.

## Java: CVE-2026-114938, 2026-11-17

A heap buffer overflow in the Base64 decoder of OpenJDK, in the intrinsic
that HotSpot uses on x86-64 with AVX-512 VBMI (Intel Ice Lake and later,
AMD Zen 4 and later). arm64 is not affected. The affected builds are
OpenJDK 17.0.6 and later on the 17, 21, 25 and 27 lines, from every vendor.

| Image | First safe version | State on 2026-11-19 |
|-------|--------------------|---------------------|
| `java-17` | 17.0.21.1 | no fixed Temurin image yet |
| `java-21` | 21.0.13.1 | no fixed Temurin image yet |
| `java-25` | 25.0.5.1 | no fixed Temurin image yet |
| `java-27` | 27.0.1.1 | no fixed Temurin image yet |
| `java-26` | none | Java 26 has no fix; move to `java-27` |

The Java images use `eclipse-temurin:<version>-jdk-resolute`. On
2026-11-19 the newest Temurin images on Docker Hub are 17.0.21+9,
21.0.13+9, 25.0.5+8 and 27.0.1, all without the fix.

## What the release does

`pkg/eol.json` has a `min_patch` floor for each Ruby and Java version
above. The image release runs `pkg/docker/verify-runtime-floor.sh` in each
built image and does not publish a variant whose runtime is below its
floor. So:

- The Ruby images of the next release have the fixed Ruby.
- The Java 17, 21, 25 and 27 images are not published again until Temurin
  publishes the fixed builds. Until then the tags `latest-java-<version>`
  stay on the previous image, which is vulnerable on x86-64 with AVX-512
  VBMI.
- Java 26 has no fixed release, so it has no floor and the check does not
  stop it. Its image is removed from the release in a separate change
  (https://github.com/freeunitorg/freeunit/pull/1001); until that change
  is in, a release still publishes a vulnerable `java-26` image.

To check an image that you run:

    $ pkg/docker/verify-runtime-floor.sh <image> ruby-3.4

## What to do now

- Ruby: pull the image of the next FreeUnit release, or rebuild your image on
  `ruby:<version>-trixie`.
- Java on arm64: no action.
- Java on x86-64: until a fixed image exists, turn off the Base64 intrinsic
  in the JVM options of the application:

  ```json
  {
      "applications": {
          "app": {
              "type": "java",
              "webapp": "/www/app.war",
              "options": ["-XX:-UseBASE64Intrinsics"]
          }
      }
  }
  ```

  `UseBASE64Intrinsics` exists on every architecture, so the same
  configuration starts on x86-64 and on arm64. `-XX:UseAVX=2` also turns
  the path off, and all AVX-512 code with it, but it exists only on
  x86-64: FreeUnit creates the JVM with unrecognized options refused, so an
  arm64 application with `-XX:UseAVX=2` does not start.
- Java 26: move to Java 27. Java 26 has no fix.

## Sources

- Ruby advisory: https://www.ruby-lang.org/en/news/2026/11/18/multiple-vulnerabilities-in-regexp-and-string/
- Ruby security announcements: https://www.ruby-lang.org/en/security/
- CVE-2026-114938: https://www.tenable.com/blog/java4shell-cve-2026-114938-frequently-asked-questions-about-openjdk-rce-vulnerability
- Eclipse Temurin releases: https://adoptium.net/temurin/releases/
- [PHP.CVE.md](PHP.CVE.md), for the PHP images
