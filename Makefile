CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -Isrc

BINS := tests/test_kiss tests/test_m17_packet tests/test_m17_bridge

.PHONY: all test san clean

all: test

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

# Same tests under ASan/UBSan, to catch buffer arithmetic mistakes in the
# escape, chunking and reassembly paths that a passing assertion would hide.
san: CFLAGS := -std=c11 -g -O1 -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer
san: clean test

clean:
	rm -f $(BINS)
