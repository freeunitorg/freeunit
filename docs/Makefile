#!/usr/bin/make

DEST=	../build
ROOT=	..
XSLS?=	xslscript.pl

PACKAGES=	unit 							\
		unit-php 						\
		unit-python unit-python2.7 unit-python3.4		\
		unit-python3.5 unit-python3.6 unit-python3.7		\
		unit-python3.8 unit-python3.9 unit-python3.10		\
		unit-python3.11						\
		unit-go unit-go1.7 unit-go1.8 unit-go1.9 unit-go1.10	\
		unit-go1.12 unit-go1.13					\
		unit-perl						\
		unit-ruby						\
		unit-jsc-common unit-jsc8 unit-jsc10 unit-jsc11		\
		unit-jsc13 unit-jsc14 unit-jsc15 unit-jsc16 unit-jsc17

CURDATE:=$(shell date +"%Y-%m-%d")
CURTIME:=$(shell date +"%H:%M:%S %z")


all:		changes changelogs

# The changelog in the root of the tree is generated from changes.xml.  Edit
# changes.xml, not CHANGES, and run this target again.
changes:	$(DEST)/CHANGES stylesheet-check
	@if [ changes.xsls -nt changes.xslt ]; then \
		echo "docs: changes.xsls is newer than changes.xslt." >&2; \
		echo "docs: the compiled form can be stale; run \"make xslt\"." >&2; \
	fi
	@if cmp -s $(DEST)/CHANGES $(ROOT)/CHANGES; then \
		echo "docs: the CHANGES file is up to date."; \
	else \
		echo "docs: the CHANGES file is updated from $(DEST)/CHANGES."; \
		cp $(DEST)/CHANGES $(ROOT)/CHANGES; \
	fi

# The check target compares the generated changelog with the file in the root
# of the tree.  It fails when the two differ and changes no file in the tree.
# The CI job runs it, so that a hand-edited CHANGES file is caught.
check:	$(DEST)/CHANGES stylesheet-check
	@if cmp -s $(DEST)/CHANGES $(ROOT)/CHANGES; then \
		echo "docs: the CHANGES file matches the generated one."; \
	else \
		echo "docs: the CHANGES file differs from the generated one." >&2; \
		echo "docs: run \"make -C docs changes\" and commit the result." >&2; \
		exit 1; \
	fi

# The stylesheet in the tree is the compiled form of changes.xsls.  The
# compiler, xslscript.pl, is not packaged in most distributions, so the build
# rule is not part of the default target: set XSLS to its path and run
# "make xslt" after a change to changes.xsls.
xslt:
	$(XSLS) -o changes.xslt changes.xsls

# The check target compares the named templates of the two stylesheets.  The
# compiler is not available in most environments, so the check cannot compare
# the body of a template.  It catches the common case of a change to
# changes.xsls that no one compiled into changes.xslt.
stylesheet-check:
	@mkdir -p $(DEST)
	@grep -oE 'X:template [A-Za-z_][A-Za-z0-9_-]*' changes.xsls \
		| awk '{print $$2}' | sort -u > $(DEST)/.templates-xsls
	@grep -oE '<xsl:template name="[A-Za-z_][A-Za-z0-9_-]*"' changes.xslt \
		| sed 's/.*name="//; s/"$$//' | sort -u > $(DEST)/.templates-xslt
	@if diff -u $(DEST)/.templates-xsls $(DEST)/.templates-xslt; then \
		echo "docs: the two stylesheets define the same templates."; \
	else \
		echo "docs: the two stylesheets differ; run \"make xslt\"." >&2; \
		exit 1; \
	fi

changelogs:	$(addsuffix .rpm-changelog, $(addprefix $(DEST)/, $(PACKAGES))) \
		$(addsuffix .deb-changelog, $(addprefix $(DEST)/, $(PACKAGES)))

$(DEST)/CHANGES:		changes.dtd				\
				changes.xml				\
				change_log_conf.xml			\
				changes.xslt

	mkdir -p $(DEST)

	xmllint --noout --valid changes.xml
	xsltproc --stringparam format generic				\
		--stringparam curdate '$(CURDATE)'			\
		--stringparam curtime '$(CURTIME)'			\
		-o $@ changes.xslt changes.xml

$(DEST)/%.rpm-changelog:	changes.dtd 				\
				changes.xml 				\
				change_log_conf.xml 			\
				changes.xslt
	mkdir -p $(DEST)
	xmllint --noout --valid changes.xml
	xsltproc --stringparam pkgname $* --stringparam format rpm	\
		--stringparam curdate '$(CURDATE)'			\
		--stringparam curtime '$(CURTIME)'			\
		-o $@ changes.xslt changes.xml

$(DEST)/%.deb-changelog:	changes.dtd 				\
				changes.xml 				\
				change_log_conf.xml 			\
				changes.xslt
	mkdir -p $(DEST)
	xmllint --noout --valid changes.xml
	xsltproc --stringparam pkgname $* --stringparam format deb	\
		--stringparam curdate '$(CURDATE)'			\
		--stringparam curtime '$(CURTIME)'			\
		-o $@ changes.xslt changes.xml

clean:
	rm -rf $(DEST)

.PHONY: all changes check changelogs xslt stylesheet-check clean
