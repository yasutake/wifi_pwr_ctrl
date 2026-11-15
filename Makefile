CC ?= cc
PKGS = libcurl openssl json-c
CPPFLAGS += $(shell pkg-config --cflags $(PKGS))
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
LDLIBS += $(shell pkg-config --libs $(PKGS))

wifi-pwr-ctrl: src/main.c src/klap.c src/legacy.c src/common.c src/common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/klap.c src/legacy.c src/common.c $(LDFLAGS) $(LDLIBS) -o $@

test: wifi-pwr-ctrl
	python3 tests/test_cli.py

clean:
	rm -f wifi-pwr-ctrl

.PHONY: test clean
