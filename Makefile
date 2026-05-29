.PHONY: all setup build full test freebsd-test bench-build bench bench-perf bench-perf-report bench-perf-hot bench-perf-duck bench-uring-stat bench-uring-record bench-uring-duck bench-uring-trace spec docs docs-publish clean traces

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

freebsd-test:
	scripts/freebsd-vm test

bench-build:
	scripts/bench build

bench:
	scripts/bench all

bench-perf:
	scripts/bench perf-record

bench-perf-report:
	scripts/bench perf-report

bench-perf-hot:
	scripts/bench perf-hot

bench-perf-duck:
	scripts/bench perf-duck

bench-uring-stat:
	scripts/bench uring-stat

bench-uring-record:
	scripts/bench uring-record

bench-uring-duck:
	scripts/bench uring-duck

bench-uring-trace:
	scripts/bench uring-trace

spec:
	racket nxtrt/model.rkt --run-all

docs:
	rm -rf docs/html
	mkdir -p docs/html
	uvx poxy --output-dir docs docs/poxy.toml
	chmod -R a+rX docs/html

docs-publish: docs
	@if [ "$$(readlink /var/www/nxt 2>/dev/null)" != "$(CURDIR)/docs/html" ]; then \
		sudo ln -sfnT $(CURDIR)/docs/html /var/www/nxt; \
	fi

clean:
	rm -rf build build-bench-release
