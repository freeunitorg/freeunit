#!/usr/bin/make

include ../../version
include ../shasum.mak

DEFAULT_VERSION := $(NXT_VERSION)

VERSION ?= $(DEFAULT_VERSION)
PATCHLEVEL ?= 1

MODULES ?= go java node perl php python ruby wasm
MODULES_SLIM ?= python

VARIANT ?= trixie

VERSIONS_minimal ?=
CONTAINER_minimal ?= debian:$(VARIANT)-slim
CONFIGURE_minimal ?=
INSTALL_minimal ?=	version
RUN_minimal ?=		/bin/true
MODULE_PREBUILD_minimal ?= /bin/true

VERSIONS_go ?=		1.25 1.26
VARIANT_go ?=		$(VARIANT)
$(foreach goversion, $(VERSIONS_go), $(eval CONTAINER_go-$(goversion) = golang:$(goversion)-$(VARIANT_go)))
CONFIGURE_go ?=		go --go-path=$$GOPATH
INSTALL_go ?=		go-install-src libunit-install
RUN_go ?=			/bin/true
MODULE_PREBUILD_go ?= /bin/true

VERSIONS_java ?=	17 21
VARIANT_java ?=		noble
$(foreach javaversion, $(VERSIONS_java), $(eval CONTAINER_java-$(javaversion) = eclipse-temurin:$(javaversion)-jdk-$(VARIANT_java)))
CONFIGURE_java ?=	java --jars=/usr/share/unit-jsc-common/
INSTALL_java ?=		java-shared-install java-install
RUN_java ?=	 		rm -rf /root/.m2
MODULE_PREBUILD_java ?= /bin/true

VERSIONS_node ?=	20 22 24
VARIANT_node ?=		$(VARIANT)
$(foreach nodeversion, $(VERSIONS_node), $(eval CONTAINER_node-$(nodeversion) = node:$(nodeversion)-$(VARIANT_node)))
CONFIGURE_node ?=	nodejs --node-gyp=/usr/local/bin/node-gyp
INSTALL_node ?=		node node-install libunit-install
RUN_node ?=			rm -rf /root/.cache/ \&\& rm -rf /root/.npm
MODULE_PREBUILD_node ?=	npm -g install node-gyp

VERSIONS_perl ?=	5.38 5.40
VARIANT_perl ?=		$(VARIANT)
$(foreach perlversion, $(VERSIONS_perl), $(eval CONTAINER_perl-$(perlversion) = perl:$(perlversion)-$(VARIANT_perl)))
CONFIGURE_perl ?=	perl
INSTALL_perl ?=		perl-install
RUN_perl ?=			/bin/true
MODULE_PREBUILD_perl ?=	/bin/true

VERSIONS_php ?=		8.3 8.4 8.5
VARIANT_php ?=		cli-$(VARIANT)
$(foreach phpversion, $(VERSIONS_php), $(eval CONTAINER_php-$(phpversion) = php:$(phpversion)-$(VARIANT_php)))
CONFIGURE_php ?=	php
INSTALL_php ?=		php-install
RUN_php ?=			ldconfig
MODULE_PREBUILD_php ?= /bin/true

VERSIONS_python ?=  3.12 3.13 3.14
VARIANT_python ?=	$(VARIANT)
$(foreach pythonversion, $(VERSIONS_python), $(eval CONTAINER_python-$(pythonversion) = python:$(pythonversion)-$(VARIANT_python)))
$(foreach pythonversion, $(VERSIONS_python), $(eval CONTAINER_python-$(pythonversion)-slim = python:$(pythonversion)-slim-$(VARIANT_python)))
CONFIGURE_python ?=	python --config=/usr/local/bin/python3-config
INSTALL_python ?=	python3-install
RUN_python ?=		/bin/true
MODULE_PREBUILD_python ?= /bin/true

VERSIONS_ruby ?=	3.3 3.4
VARIANT_ruby ?=		$(VARIANT)
$(foreach rubyversion, $(VERSIONS_ruby), $(eval CONTAINER_ruby-$(rubyversion) = ruby:$(rubyversion)-$(VARIANT_ruby)))
CONFIGURE_ruby ?=	ruby
INSTALL_ruby ?=		ruby-install
RUN_ruby ?=			gem install rack \&\& rm -rf /root/.local
MODULE_PREBUILD_ruby ?=	/bin/true

VERSIONS_wasm ?=
CONTAINER_wasm ?= debian:$(VARIANT)-slim
CONFIGURE_wasm ?= wasm --include-path=\`pwd\`/pkg/contrib/wasmtime/artifacts/include --lib-path=/usr/lib/\$$(dpkg-architecture -q DEB_HOST_MULTIARCH)/ \&\& ./configure wasm-wasi-component
INSTALL_wasm ?=   wasm-install wasm-wasi-component-install
RUN_wasm ?=       /bin/true

define MODULE_PREBUILD_wasm
make -C pkg/contrib .wasmtime \\\n \
\ \ \ \&\& install -pm 755 pkg/contrib/wasmtime/artifacts/lib/libwasmtime.so /usr/lib/\$$\(dpkg-architecture -q DEB_HOST_MULTIARCH\)/
endef

default:
	@echo "valid targets: all build dockerfiles library clean"

MODVERSIONS = $(foreach module, $(MODULES), $(foreach modversion, $(shell for v in $(VERSIONS_$(module)); do echo $$v; done | sort -r), $(module)-$(modversion)))
MODVERSIONS += $(foreach module, $(MODULES_SLIM), $(foreach modversion, $(shell for v in $(VERSIONS_$(module)); do echo $$v; done | sort -r), $(module)-$(modversion)-slim))
MODVERSIONS += wasm minimal

modname = $(shell echo $1 | /usr/bin/tr -d '.01234567890-' | sed 's/slim//')

dockerfiles: $(addprefix Dockerfile., $(MODVERSIONS))
build: $(addprefix build-, $(MODVERSIONS))

Dockerfile.%: ../../version template.Dockerfile
	@echo "===> Building $@"
	cat template.Dockerfile | sed \
			-e 's,@@VERSION@@,$(VERSION),g' \
			-e 's,@@PATCHLEVEL@@,$(PATCHLEVEL),g' \
			-e 's,@@CONTAINER@@,$(CONTAINER_$*),g' \
			-e 's,@@CONFIGURE@@,$(CONFIGURE_$(call modname, $*)),g' \
			-e 's,@@INSTALL@@,$(INSTALL_$(call modname, $*)),g' \
			-e 's,@@RUN@@,$(RUN_$(call modname, $*)),g' \
			-e 's,@@MODULE_PREBUILD@@,$(MODULE_PREBUILD_$(call modname, $*)),g' \
			-e 's,@@MODULE@@,$*,g' \
			> $@

build-%: Dockerfile.%
	docker pull $(CONTAINER_$*)
	docker build --no-cache -t unit:$(VERSION)-$* -f Dockerfile.$* .
	touch $@

# TODO(#milestone-1.35.8): uncomment and update GitFetch when publishing to
# Docker Hub Official Images.  Target date: 2026-08-28 (1.35.8 release).
#library:
#	@echo "# this file is generated via https://github.com/freeunitorg/freeunit/blob/$(shell git describe --always --abbrev=0 HEAD)/pkg/docker/Makefile"
#	@echo ""
#	@echo "Maintainers: FreeUnit Community <team@freeunit.org> (@freeunitorg)"
#	@echo "GitRepo: https://github.com/freeunitorg/freeunit.git"
#	@previous=""; \
#	 for mod in $(MODVERSIONS); do \
#		echo ""; \
#		modname="$$( echo $$mod | tr -d '.0123456789')"; \
#		modmajor="$${mod%%.*}"; \
#		if test "$${mod#*slim}" != "$$mod"; then \
#			modmajor="$${modmajor}-slim"; \
#		fi; \
#		TAGS="$$mod $$modmajor $$modname"; \
#		TAGS="$$(echo $$TAGS | tr " " "\n" | sort -u -r | tr "\n" "," | sed "s/,/, /g")"; \
#		if test "$${previous#*"$$modname"}" != "$$previous"; then \
#			echo "Tags: $(VERSION)-$$mod, $$mod"; \
#		else \
#			if [ "$$mod" = "minimal" ]; then \
#				echo "Tags: $(VERSION)-$$mod, $${TAGS%, }, latest"; \
#			else \
#				echo "Tags: $(VERSION)-$$mod, $${TAGS%, }"; \
#			fi; \
#		fi; \
#		echo "Architectures: amd64, arm64v8"; \
#		echo "GitFetch: refs/heads/master"; \
#		echo "GitCommit: $(shell git describe --always --abbrev=0 HEAD)"; \
#		echo "Directory: pkg/docker"; \
#		echo "File: Dockerfile.$$mod"; \
#		previous="$$previous $$modname"; \
#	done

diff: $(addprefix diff-, $(MODVERSIONS))

diff-%:
	@echo container-diff diff --type file daemon://$(CONTAINER_$*) daemon://unit:$(VERSION)-$*

all: $(addprefix Dockerfile., $(MODVERSIONS))

clean:
	rm -f $(filter-out $(wildcard Dockerfile.builder-*), $(wildcard Dockerfile.*))
	rm -f $(addprefix build-, $(MODVERSIONS))
	rm -rf build-logs

.PHONY: default build dockerfiles clean
