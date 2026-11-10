CC ?= cc
CFLAGS ?= -O2
CPPFLAGS += -Ivendor -Igenerated
WARNINGS = -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean test
all: build/click-helper build/LICENSE.cJSON

build/click-helper: src/helper.c vendor/cJSON.c vendor/cJSON.h generated/config.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) src/helper.c vendor/cJSON.c $(LDFLAGS) -s -o $@

build/LICENSE.cJSON: vendor/LICENSE.cJSON
	mkdir -p build
	cp $< $@

generated/config.h:
	@echo 'Run ./setup.sh first to record your program and clicks.' >&2
	@exit 1

clean:
	rm -rf build

test:
	python3 -m unittest discover -s tests -v
