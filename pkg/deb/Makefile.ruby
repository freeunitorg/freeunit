MODULES+=		ruby
MODULE_SUFFIX_ruby=	ruby

MODULE_SUMMARY_ruby=	Ruby module for $(BRAND_TITLE)

MODULE_VERSION_ruby=	$(VERSION)
MODULE_RELEASE_ruby=	1

MODULE_CONFARGS_ruby=	ruby
MODULE_MAKEARGS_ruby=	ruby
MODULE_INSTARGS_ruby=	ruby-install

MODULE_SOURCES_ruby=	freeunit.example-ruby-app \
			freeunit.example-ruby-config

BUILD_DEPENDS_ruby=	ruby-dev ruby-rack
BUILD_DEPENDS+=         $(BUILD_DEPENDS_ruby)

MODULE_BUILD_DEPENDS_ruby=,ruby-dev,ruby-rack

MODULE_DEPENDS_ruby=,ruby-rack

define MODULE_PREINSTALL_ruby
	mkdir -p debian/$(BRAND)-ruby/usr/share/doc/$(BRAND)-ruby/examples
	install -m 644 -p debian/freeunit.example-ruby-app debian/$(BRAND)-ruby/usr/share/doc/$(BRAND)-ruby/examples/ruby-app.ru
	install -m 644 -p debian/freeunit.example-ruby-config debian/$(BRAND)-ruby/usr/share/doc/$(BRAND)-ruby/examples/$(BRAND).config
endef
export MODULE_PREINSTALL_ruby

define MODULE_POST_ruby
cat <<BANNER
----------------------------------------------------------------------

The $(MODULE_SUMMARY_ruby) has been installed.

To check out the sample app, run these commands:

 sudo service $(RUNTIME) restart
 cd /usr/share/doc/$(BRAND)-$(MODULE_SUFFIX_ruby)/examples
 $(MODULE_CONFIG_PUT)
 curl http://localhost:8700/

Online documentation is available at $(DOCS_URL)

----------------------------------------------------------------------
BANNER
endef
export MODULE_POST_ruby
