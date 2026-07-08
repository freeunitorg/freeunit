# njs

include $(dir $(abspath $(lastword $(MAKEFILE_LIST))))/version
NJS_URL := https://packages.freeunit.org/njs/njs-$(NJS_VERSION).tar.gz

PKGS += njs

$(TARBALLS)/njs-$(NJS_VERSION).tar.gz:
	$(call download_pkg,$(NJS_URL),njs)

.sum-njs: njs-$(NJS_VERSION).tar.gz

njs: njs-$(NJS_VERSION).tar.gz .sum-njs
	$(UNPACK)
	$(MOVE)

# njs 1.0.0 recommends QuickJS, but that requires an external QuickJS /
# QuickJS-NG library, so build --no-quickjs to keep the baseline deterministic.
.njs: njs
	cd $< && ./configure \
		--no-quickjs \
		--no-libxml2 \
		--no-zlib \
		&& $(MAKE) libnjs
	touch $@
