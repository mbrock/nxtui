.PHONY: all setup build full test spec docs docs-publish clean traces

all: build

setup:
	meson setup build

build:
	meson compile -C build nxtllm

full:
	meson compile -C build

test:
	meson compile -C build nxt-tests
	meson test -C build

spec:
	racket nxtrt/model.rkt --run-all

docs:
	rm -rf docs/html
	mkdir -p docs/html
	uvx poxy --output-dir docs docs/poxy.toml
	chmod -R a+rX docs/html

docs-publish: docs
	rsync -a --delete docs/html/ /var/www/nxtui/

clean:
	rm -rf build
