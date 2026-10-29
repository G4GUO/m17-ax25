CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -Isrc

BINS := tests/test_kiss tests/test_m17_packet tests/test_m17_bridge
TOOL := tools/m17bridge

.PHONY: all test itest san clean tool

all: test tool

test: $(BINS)
	@./tests/test_kiss
	@./tests/test_m17_packet
	@./tests/test_m17_bridge

tests/test_kiss: src/kiss.c tests/test_kiss.c src/kiss.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/kiss.c tests/test_kiss.c

tests/test_m17_packet: src/m17_packet.c tests/test_m17_packet.c src/m17_packet.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/m17_packet.c tests/test_m17_packet.c

tests/test_m17_bridge: src/m17_bridge.c tests/test_m17_bridge.c src/m17_bridge.h src/m17_packet.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ src/m17_bridge.c tests/test_m17_bridge.c

tool: $(TOOL)

$(TOOL): tools/m17bridge.c src/kiss.c src/m17_packet.c src/m17_bridge.c src/kiss.h src/m17_packet.h src/m17_bridge.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ tools/m17bridge.c src/kiss.c src/m17_packet.c src/m17_bridge.c

# The loopback test starts the tool, connects two KISS clients and checks that
# frames cross intact and that the console commands work.
itest: $(TOOL)
	@python3 tests/test_loopback.py

# Same tests under ASan/UBSan, to catch buffer arithmetic mistakes in the
# escape, chunking and reassembly paths that a passing assertion would hide.
san: CFLAGS := -std=c11 -g -O1 -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer
san: clean test

clean:
	rm -f $(BINS) $(TOOL)
