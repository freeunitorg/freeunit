# The image variants of pkg/eol.json.  release-docker.yml and build-local.sh
# include this file, so the list has one definition.
#
# A runtime version gives "<runtime>-<version>".  A null version (wasm,
# minimal) gives "<runtime>".  "slim": true adds a "-slim" sibling, and
# "zts_variant": true adds a thread-safe "-zts" sibling.  A variant name is
# the suffix of its Dockerfile, pkg/docker/Dockerfile.<variant>.

def variants:
    .runtimes | to_entries[] | .key as $rt | .value[] |
    if .version == null then $rt
    else $rt + "-" + .version
         + ("", (select(.slim) | "-slim"), (select(.zts_variant) | "-zts"))
    end;
