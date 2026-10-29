CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -pedantic
CPPFLAGS += -Isrc

SRC   := src/kiss.c
TESTS := tests/test_kiss.c
BIN   := tests/test_kiss

.PHONY: all test san clean

all: test

test: $(BIN)
	@./$(BIN)

$(BIN): $(SRC) $(TESTS) src/kiss.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SRC) $(TESTS)

# Same tests under ASan/UBSan, to catch buffer arithmetic mistakes in the
# escape and overrun paths that a passing assertion would hide.
san: CFLAGS := -std=c11 -g -O1 -Wall -Wextra -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer
san: clean test

clean:
	rm -f $(BIN)
