.PHONY: all setup build test docs docs-publish clean

all: build

setup:
	meson setup build

build:
	meson compile -C build

test: build
	meson test -C build

docs:
	cd docs && uv run --with poxy poxy poxy.toml

docs-publish: docs
	rsync -a --delete docs/html/ /var/www/nxtui/

clean:
	rm -rf build
