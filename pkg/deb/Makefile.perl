MODULES+=		perl
MODULE_SUFFIX_perl=	perl

MODULE_SUMMARY_perl=	Perl module for $(BRAND_TITLE)

MODULE_VERSION_perl=	$(VERSION)
MODULE_RELEASE_perl=	1

MODULE_CONFARGS_perl=	perl
MODULE_MAKEARGS_perl=	perl
MODULE_INSTARGS_perl=	perl-install

MODULE_SOURCES_perl=	freeunit.example-perl-app \
			freeunit.example-perl-config

BUILD_DEPENDS_perl=	libperl-dev
BUILD_DEPENDS+=         $(BUILD_DEPENDS_perl)

MODULE_BUILD_DEPENDS_perl=,libperl-dev

define MODULE_PREINSTALL_perl
	mkdir -p debian/$(BRAND)-perl/usr/share/doc/$(BRAND)-perl/examples/perl-app
	install -m 644 -p debian/freeunit.example-perl-app debian/$(BRAND)-perl/usr/share/doc/$(BRAND)-perl/examples/perl-app/index.pl
	install -m 644 -p debian/freeunit.example-perl-config debian/$(BRAND)-perl/usr/share/doc/$(BRAND)-perl/examples/$(BRAND).config
endef
export MODULE_PREINSTALL_perl

define MODULE_POST_perl
cat <<BANNER
----------------------------------------------------------------------

The $(MODULE_SUMMARY_perl) has been installed.

To check out the sample app, run these commands:

 sudo service $(RUNTIME) restart
 cd /usr/share/doc/$(BRAND)-$(MODULE_SUFFIX_perl)/examples
 $(MODULE_CONFIG_PUT)
 curl http://localhost:8600/

Online documentation is available at $(DOCS_URL)

----------------------------------------------------------------------
BANNER
endef
export MODULE_POST_perl
