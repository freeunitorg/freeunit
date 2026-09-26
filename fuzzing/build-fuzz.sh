#!/usr/bin/env bash

export CC=clang
export CXX=clang++
export CFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -fsanitize=address,undefined -fsanitize=fuzzer-no-link"
export CXXFLAGS="-g -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION -fsanitize=address,undefined -fsanitize=fuzzer-no-link"
export LIB_FUZZING_ENGINE="-fsanitize=fuzzer"

NXT_FUZZ_CONFIGURE="--no-regex --no-pcre2 --fuzz=$LIB_FUZZING_ENGINE"

# fuzz_http_h2p #includes nxt_h2proto.c, so it only builds with --h2 (which
# itself needs --openssl).  libnghttp2.pc alone does not mean that configure
# succeeds: the OpenSSL development files may be missing, or nghttp2 may
# lack the setters auto/h2 probes for.  So try the h2 configure, and if it
# fails, configure again without h2: `make fuzz` then leaves fuzz_http_h2p
# out (see auto/h2 and auto/sources) and still builds the other five.
NXT_FUZZ_H2=no

if pkgconf --exists libnghttp2 2>/dev/null || pkg-config --exists libnghttp2 2>/dev/null
then
	# shellcheck disable=SC2086  # deliberate word splitting
	if ./configure $NXT_FUZZ_CONFIGURE --openssl --h2; then
		NXT_FUZZ_H2=yes
	else
		echo "build-fuzz.sh: configure --openssl --h2 failed (see above);" \
		     "building without fuzz_http_h2p" >&2
	fi
else
	echo "build-fuzz.sh: libnghttp2 not found; building without fuzz_http_h2p" >&2
fi

if [ $NXT_FUZZ_H2 = no ]; then
	# shellcheck disable=SC2086  # deliberate word splitting
	./configure $NXT_FUZZ_CONFIGURE || exit 1

	# Not built by this configuration; an old binary must not look current.
	rm -f build/fuzz_http_h2p
fi

make fuzz -j$(nproc) || exit 1

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
