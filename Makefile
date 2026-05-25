.PHONY: all setup build test docs docs-publish clean traces

all: build

setup:
	meson setup build

build:
	meson compile -C build

test: build
	meson test -C build

docs:
	rm -rf docs/html
	mkdir -p docs/html
	cd docs2 && doxygen Doxyfile
	racket docs2/doxygen-to-nice.rkt \
		--input docs2/out/doxygen/xml/rt_overview.xml \
		--output docs2/out/rt-overview.xml
	bun docs/forge_graphs.ts prepare
	bun docs/forge_graphs.ts install
	cp docs/forge-doc-graphs.js docs/html/forge-doc-graphs.js
	xsltproc docs2/nice-html.xsl docs2/out/rt-overview.xml > docs/html/index.html

docs-publish: docs
	rsync -a --delete docs/html/ /var/www/nxtui/

clean:
	rm -rf build
