MODULES+=		php
MODULE_SUFFIX_php=	php

MODULE_SUMMARY_php=	PHP module for $(BRAND_TITLE)

MODULE_VERSION_php=	$(VERSION)
MODULE_RELEASE_php=	1

MODULE_CONFARGS_php=	php
MODULE_MAKEARGS_php=	php
MODULE_INSTARGS_php=	php-install

MODULE_SOURCES_php=	freeunit.example-php-app \
			freeunit.example-php-config

ifneq (,$(findstring $(CODENAME),trusty jessie))
BUILD_DEPENDS_php=	php5-dev libphp5-embed
MODULE_BUILD_DEPENDS_php=,php5-dev,libphp5-embed
MODULE_DEPENDS_php=,libphp5-embed
else
BUILD_DEPENDS_php=	php-dev libphp-embed
MODULE_BUILD_DEPENDS_php=,php-dev,libphp-embed
MODULE_DEPENDS_php=,libphp-embed
endif

BUILD_DEPENDS+=		$(BUILD_DEPENDS_php)

define MODULE_PREINSTALL_php
	mkdir -p debian/$(BRAND)-php/usr/share/doc/$(BRAND)-php/examples/phpinfo-app
	install -m 644 -p debian/freeunit.example-php-app debian/$(BRAND)-php/usr/share/doc/$(BRAND)-php/examples/phpinfo-app/index.php
	install -m 644 -p debian/freeunit.example-php-config debian/$(BRAND)-php/usr/share/doc/$(BRAND)-php/examples/$(BRAND).config
endef
export MODULE_PREINSTALL_php

define MODULE_POST_php
cat <<BANNER
----------------------------------------------------------------------

The $(MODULE_SUMMARY_php) has been installed.

To check out the sample app, run these commands:

 sudo service $(RUNTIME) restart
 cd /usr/share/doc/$(BRAND)-$(MODULE_SUFFIX_php)/examples
 $(MODULE_CONFIG_PUT)
 curl http://localhost:8300/

Online documentation is available at $(DOCS_URL)

----------------------------------------------------------------------
BANNER
endef
export MODULE_POST_php
