#!/bin/sh

# Source: https://github.com/OpenAPITools/openapi-generator/blob/master/bin/utils/openapi-generator-cli.sh
# License: Apache 2.0

####
# Save as openapi-generator-cli on your PATH. chmod u+x. Enjoy.
#
# This script will query github on every invocation to pull the latest released
# version of openapi-generator.
#
# If you want repeatable executions, you can explicitly set a version via
#    OPENAPI_GENERATOR_VERSION
# e.g. (in POSIX sh)
#    export OPENAPI_GENERATOR_VERSION=3.1.0
#    openapi-generator-cli.sh
# or
#    OPENAPI_GENERATOR_VERSION=3.1.0 openapi-generator-cli.sh
#
# This is also helpful, for example, if you want to evaluate a SNAPSHOT version.
#
# NOTE: Jars are downloaded on demand from maven into the same directory as this
# script for every 'latest' version pulled from github. Consider putting this
# under its own directory.
####

# pipefail is not in POSIX; use it when the shell supports it.
# shellcheck disable=SC3040
if (set -o pipefail) 2>/dev/null; then
  set -o pipefail
fi

for cmd in mvn jq curl; do
  if ! command -v "${cmd}" > /dev/null 2>&1; then
    printf "This script requires '%s' to be installed.\n" "${cmd}" >&2
    exit 1
  fi
done

latest_tag() {
  _repo=$1
  _uri="https://api.github.com/repos/${_repo}/releases"
  _ver=$(curl -s "${_uri}" | jq -r 'first(.[]|select(.prerelease==false)).tag_name')
  case "${_ver}" in
    v*) _ver="${_ver#v}" ;;
  esac
  printf '%s\n' "${_ver}"
}

ghrepo=openapitools/openapi-generator
groupid=org.openapitools
artifactid=openapi-generator-cli
ver=${OPENAPI_GENERATOR_VERSION:-$(latest_tag "${ghrepo}")}

printf 'Using OpenAPI Generator version: %s\n' "${ver}"

jar=${artifactid}-${ver}.jar
cachedir=${OPENAPI_GENERATOR_DOWNLOAD_CACHE_DIR}

DIR=${cachedir:-"$( cd "$( dirname "$0" )" && pwd )"}

if [ ! -d "${DIR}" ]; then
  mkdir -p "${DIR}"
fi

if [ ! -f "${DIR}/${jar}" ]; then
  repo="central::default::https://repo1.maven.org/maven2/"
  case "${ver}" in
    *-SNAPSHOT)
      repo="central::default::https://oss.sonatype.org/content/repositories/snapshots"
      ;;
  esac
  mvn org.apache.maven.plugins:maven-dependency-plugin:2.9:get \
    -DremoteRepositories="${repo}" \
    -Dartifact="${groupid}:${artifactid}:${ver}" \
    -Dtransitive=false \
    -Ddest="${DIR}/${jar}"
fi

# JAVA_OPTS may contain multiple space-separated flags
# shellcheck disable=SC2086
exec java -ea                     \
  ${JAVA_OPTS}                    \
  -Xms512M                        \
  -Xmx1024M                       \
  -server                         \
  -jar "${DIR}/${jar}" "$@"
