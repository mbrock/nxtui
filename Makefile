.PHONY: all setup build test clean

all: build

setup:
	meson setup build

build:
	meson compile -C build

test: build
	meson test -C build

clean:
	rm -rf build
