#!/usr/bin/env bash

export CC=clang
export CXX=clang++
export CFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -fsanitize=address,undefined -fsanitize=fuzzer-no-link"
export CXXFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -fsanitize=address,undefined -fsanitize=fuzzer-no-link"
export LIB_FUZZING_ENGINE="-fsanitize=fuzzer"

# fuzz_http_h2p #includes nxt_h2proto.c, so it only builds with --h2 (which
# itself needs --openssl); pass those only when nghttp2 is actually here, so
# this still works on a machine without it -- `make fuzz` then simply leaves
# fuzz_http_h2p out (see auto/h2 and auto/sources), same as CI does.
NXT_FUZZ_H2_CONFIGURE=
if pkgconf --exists libnghttp2 2>/dev/null || pkg-config --exists libnghttp2 2>/dev/null
then
	NXT_FUZZ_H2_CONFIGURE="--openssl --h2"
else
	echo "build-fuzz.sh: libnghttp2 not found; building without fuzz_http_h2p" >&2
fi

# shellcheck disable=SC2086  # deliberate word splitting
./configure --no-regex --no-pcre2 $NXT_FUZZ_H2_CONFIGURE --fuzz=$LIB_FUZZING_ENGINE
make fuzz -j$(nproc)

mkdir -p build/fuzz_basic_seed
mkdir -p build/fuzz_http_controller_seed
mkdir -p build/fuzz_http_h1p_seed
mkdir -p build/fuzz_http_h1p_peer_seed
mkdir -p build/fuzz_json_seed

if [ -x build/fuzz_http_h2p ]; then
	mkdir -p build/fuzz_http_h2p_seed
fi

echo ""
echo "Run: ./build/\${fuzzer} build/\${fuzzer}_seed fuzzing/\${fuzzer}_seed_corpus"
if [ -x build/fuzz_http_h2p ]; then
	echo "     ./build/fuzz_http_h2p build/fuzz_http_h2p_seed fuzzing/fuzz_h2p_seed_corpus"
fi
echo ""
