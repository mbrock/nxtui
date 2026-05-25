.PHONY: all setup build test spec docs docs-publish clean traces

all: build

setup:
	meson setup build

build:
	meson compile -C build

test: build
	meson test -C build

spec:
	racket nxtrt/model.rkt --run-all

docs:
	rm -rf docs/html
	mkdir -p docs/html
	uvx poxy --output-dir docs docs/poxy.toml

docs-publish: docs
	rsync -a --delete docs/html/ /var/www/nxtui/

clean:
	rm -rf build
