# wasmtime

include $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/version
WASMTIME_URL := https://packages.freeunit.org/wasmtime/wasmtime-v$(WASMTIME_VERSION)-src.tar.gz

PKGS += wasmtime

ifeq ($(shell which cargo >/dev/null 2>&1 || echo FAIL),)
CARGO = cargo
else
CARGO = $(error Cargo (Rust package manager) not found)
endif

$(TARBALLS)/wasmtime-v$(WASMTIME_VERSION)-src.tar.gz:
	$(call download_pkg,$(WASMTIME_URL),wasmtime)

.sum-wasmtime: wasmtime-v$(WASMTIME_VERSION)-src.tar.gz

wasmtime: wasmtime-v$(WASMTIME_VERSION)-src.tar.gz .sum-wasmtime
	$(UNPACK)
	$(MOVE)

# The wasm module loads core modules only (wasmtime_module_new() in
# src/wasm/nxt_rt_wasmtime.c) and calls no wasmtime_component_*() function.
# So the component model is off.  This removes the wasmtime_component_*() C
# API, and the code that only that API reaches, from libwasmtime.so.
# cmake does not check that component-model-async needs component-model, so
# turn the two off together.  check-lock-audit.py reads every -DWASMTIME_*
# flag below and accepts only =ON and =OFF.
.wasmtime: wasmtime
	cd $< && cmake \
				-DCMAKE_INSTALL_LIBDIR=lib \
				-DWASMTIME_FEATURE_WASI_HTTP=OFF \
				-DWASMTIME_FEATURE_COMPONENT_MODEL_ASYNC=OFF \
				-DWASMTIME_FEATURE_COMPONENT_MODEL=OFF \
				-S crates/c-api \
				-B target/c-api \
				--install-prefix "$(TOPSRC)/wasmtime/artifacts"
	cd $< && cmake --build target/c-api
	cd $< && cmake --install target/c-api
	touch $@
