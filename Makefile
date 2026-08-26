# lambda — minimal agent harness. `make` builds ./lambda
#
#   make              optimized build
#   make release      optimized, dead-stripped, symbols removed
#   make install      install to $(PREFIX)/bin  (default /usr/local)
#   make uninstall    remove it again
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

clean:
	rm -rf build lambda

.PHONY: clean release install uninstall
