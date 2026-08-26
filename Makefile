# lambda — minimal agent harness. `make` builds ./lambda
#
#   make              optimized build
#   make release      optimized, dead-stripped, symbols removed
#   make install      install to $(PREFIX)/bin  (default /usr/local)
#   make uninstall    remove it again
#   make test         build and run the unit tests (asan/ubsan)
#   make STATIC=1     fully static binary (linux; best with musl)
#   make DEBUG=1      -O0 -g with sanitizers
#   make clean

CC       ?= cc
CFLAGS   ?= -O2
CFLAGS   += -std=c99 -Wall -Wextra
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE \
            -Isrc -Ivendor/bearssl/inc -Ivendor/jsmn -Ivendor/picohttpparser

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
INSTALL ?= install

# the two toolchains spell dead-code stripping differently
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
GC_LDFLAGS := -Wl,-dead_strip
STRIP_ARGS := -S -x
else
GC_LDFLAGS := -Wl,--gc-sections
STRIP_ARGS := -s
endif

ifeq ($(RELEASE),1)
# -ffunction-sections + --gc-sections drops the large parts of bearssl we
# never call (most ciphersuites, the server side, every unused hash)
CFLAGS  := -std=c99 -Wall -Wextra -O2 -DNDEBUG \
           -ffunction-sections -fdata-sections
LDFLAGS += $(GC_LDFLAGS)
endif
ifeq ($(WERROR),1)
CFLAGS += -Werror
endif
ifeq ($(STATIC),1)
LDFLAGS += -static
endif
ifeq ($(DEBUG),1)
CFLAGS := -std=c99 -Wall -Wextra -O0 -g -fsanitize=address,undefined
LDFLAGS += -fsanitize=address,undefined
endif

SRC         := $(wildcard src/*.c) $(wildcard plugins/*.c)
VENDOR_SRC  := vendor/picohttpparser/picohttpparser.c \
               $(wildcard vendor/bearssl/src/*.c) \
               $(wildcard vendor/bearssl/src/*/*.c)

OBJ := $(patsubst %.c,build/%.o,$(SRC) $(VENDOR_SRC))

lambda: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDFLAGS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

# vendored code is built as-is; suppress warnings and give bearssl its
# internal include dir
build/vendor/%.o: CFLAGS += -w
build/vendor/bearssl/%.o: CPPFLAGS += -Ivendor/bearssl/src

# Rebuilt from scratch so the release flags reach every object, including
# anything already sitting in build/ from a plain `make`.
release:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory lambda RELEASE=1
	@strip $(STRIP_ARGS) lambda
	@echo
	@ls -lh lambda | awk '{print "  lambda  " $$5}'
	@./lambda --version | sed 's/^/  /'

install: lambda
	@$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 755 lambda $(DESTDIR)$(BINDIR)/lambda
	@echo "installed $(DESTDIR)$(BINDIR)/lambda"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lambda
	@echo "removed $(DESTDIR)$(BINDIR)/lambda"

# Tests include the translation unit under test so they can reach its static
# state, so they compile their own dependencies rather than linking build/.
# The arena is shrunk right down so compaction is exercised in a short run.
TEST_SRC   := $(wildcard tests/*.c)
TEST_BIN   := $(patsubst tests/%.c,build/tests/%,$(TEST_SRC))
TEST_CFLAGS := -std=c99 -Wall -Wextra -O1 -g -fsanitize=address,undefined
TEST_DEFS   := -DLAMBDA_TRANSCRIPT_ARENA=4096 -DLAMBDA_MAX_ITEMS=8

build/tests/%: tests/%.c $(wildcard src/*.c) $(wildcard src/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(TEST_CFLAGS) $(CPPFLAGS) $(TEST_DEFS) -o $@ $< \
	    src/term.c src/md.c src/util.c

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "== $$t"; $$t || exit 1; done
	@echo "all tests passed"

clean:
	rm -rf build lambda

.PHONY: clean release install uninstall test
