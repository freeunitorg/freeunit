#!/bin/bash -eu

# fuzz_http_h2p #includes nxt_h2proto.c, so it only builds with --h2 (which
# itself needs --openssl); pass those only when nghttp2 is actually here,
# same as build-fuzz.sh and the CI workflow.
NXT_FUZZ_H2_CONFIGURE=
if pkgconf --exists libnghttp2 2>/dev/null || pkg-config --exists libnghttp2 2>/dev/null
then
	NXT_FUZZ_H2_CONFIGURE="--openssl --h2"
fi

# Build unit
# shellcheck disable=SC2086  # deliberate word splitting
./configure --no-regex --no-pcre2 $NXT_FUZZ_H2_CONFIGURE --fuzz="$LIB_FUZZING_ENGINE"
make fuzz -j"$(nproc)"

# Copy all fuzzers.
cp build/fuzz_* $OUT/

# cd into fuzzing dir
pushd fuzzing/
cp fuzz_http.dict $OUT/fuzz_http_controller.dict
cp fuzz_http.dict $OUT/fuzz_http_h1p.dict
cp fuzz_http.dict $OUT/fuzz_http_h1p_peer.dict

# Create temporary directories.
cp -r fuzz_http_seed_corpus/ fuzz_http_controller_seed_corpus/
cp -r fuzz_http_seed_corpus/ fuzz_http_h1p_seed_corpus/
cp -r fuzz_http_seed_corpus/ fuzz_http_h1p_peer_seed_corpus/

zip -r $OUT/fuzz_basic_seed_corpus.zip fuzz_basic_seed_corpus/
zip -r $OUT/fuzz_http_controller_seed_corpus.zip  fuzz_http_controller_seed_corpus/
zip -r $OUT/fuzz_http_h1p_seed_corpus.zip  fuzz_http_h1p_seed_corpus/
zip -r $OUT/fuzz_http_h1p_peer_seed_corpus.zip  fuzz_http_h1p_peer_seed_corpus/
zip -r $OUT/fuzz_json_seed_corpus.zip fuzz_json_seed_corpus/

# Delete temporary directories.
rm -r fuzz_http_controller_seed_corpus/ fuzz_http_h1p_seed_corpus/ fuzz_http_h1p_peer_seed_corpus/

# fuzz_http_h2p's own corpus/dict, named fuzz_h2p_seed_corpus (not
# fuzz_http_h2p_seed_corpus) since nghttp2 frame/HPACK bytes are not useful
# to the h1 targets or vice versa (see run-ci.sh's corpus_for()); only
# present, and only copied, when --h2 was actually configured in above.
if [ -f "$OUT/fuzz_http_h2p" ]; then
	cp fuzz_h2.dict "$OUT/fuzz_http_h2p.dict"
	zip -r "$OUT/fuzz_http_h2p_seed_corpus.zip" fuzz_h2p_seed_corpus/
fi

popd
