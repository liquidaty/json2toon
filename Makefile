# json2toon - build the libjson2toon library, the json2toon CLI, and tests.
#
# Targets used by the build protocol:
#   make build         build the library and application
#   make test          build and run the test suite
#   make test-leaks    run the test suite under a leak checker
#   make clean         remove build artifacts
#
# Flags: ASAN=1 (AddressSanitizer + UBSan), DEBUG=1 (debug build).
# Environment CC/AR/RANLIB/CFLAGS/LDFLAGS and CONFIGFILE are honoured. The
# config file written by ./configure is included when present; otherwise
# sensible defaults let "make build" work standalone.

CONFIGFILE ?= config.mk
-include $(CONFIGFILE)

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
PREFIX  ?= /usr/local

UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(SHLIB_EXT),)
  ifeq ($(UNAME_S),Darwin)
    SHLIB_EXT := dylib
    SHLIB_LDFLAGS := -dynamiclib -install_name $(PREFIX)/lib/libjson2toon.dylib
  else
    SHLIB_EXT := so
    SHLIB_LDFLAGS := -shared -Wl,-soname,libjson2toon.so
  endif
endif

# Executable suffix. ./configure sets this (".exe" for Windows targets, empty
# otherwise); when building standalone without configure, detect a native
# Windows shell (MSYS/MinGW/Cygwin) from uname so $(APP) matches the file the
# linker actually produces.
ifeq ($(origin EXE_EXT),undefined)
  ifneq (,$(filter MINGW% MSYS% CYGWIN% Windows%,$(UNAME_S)))
    EXE_EXT := .exe
  else
    EXE_EXT :=
  endif
endif

ENABLE_SHARED ?= 1
ifneq ($(STATIC_BUILD),)
  ifneq ($(STATIC_BUILD),0)
    ENABLE_SHARED := 0
  endif
endif

WARN := -Wall -Wextra -Wno-unused-parameter
BASE_CFLAGS := -std=c11 $(WARN) -Iinclude -Isrc -fPIC
OPT_CFLAGS := -O2

ifeq ($(DEBUG),1)
  OPT_CFLAGS := -O0 -g -DDEBUG
endif
ifeq ($(DEBUG_DEFAULT),1)
  OPT_CFLAGS := -O0 -g -DDEBUG
endif

SAN_CFLAGS :=
SAN_LDFLAGS :=
ifeq ($(ASAN),1)
  SAN_CFLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer -g
  SAN_LDFLAGS := -fsanitize=address,undefined
  OPT_CFLAGS := -O1
endif

ALL_CFLAGS := $(BASE_CFLAGS) $(OPT_CFLAGS) $(SAN_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)
ALL_LDFLAGS := $(SAN_LDFLAGS) $(EXTRA_LDFLAGS) $(LDFLAGS)

OBJDIR := obj
LIB_SRCS := src/json2toon.c src/dom.c src/format.c src/simd.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.o)

STATIC_LIB := libjson2toon.a
SHARED_LIB := libjson2toon.$(SHLIB_EXT)
APP := json2toon$(EXE_EXT)
TESTBIN := $(OBJDIR)/test$(EXE_EXT)

LIBS := $(STATIC_LIB)
ifeq ($(ENABLE_SHARED),1)
  LIBS += $(SHARED_LIB)
endif

.PHONY: all build lib app test test-leaks clean install
.DEFAULT_GOAL := build

all: build
build: $(LIBS) $(APP)
lib: $(LIBS)
app: $(APP)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(OBJDIR)/main.o: app/main.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(OBJDIR)/test.o: tests/test.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)
	$(RANLIB) $@ 2>/dev/null || true

$(SHARED_LIB): $(LIB_OBJS)
	$(CC) $(ALL_LDFLAGS) $(SHLIB_LDFLAGS) -o $@ $(LIB_OBJS)

$(APP): $(OBJDIR)/main.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/main.o $(STATIC_LIB)

$(TESTBIN): $(OBJDIR)/test.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/test.o $(STATIC_LIB)

test: $(TESTBIN)
	$(TESTBIN)

# Leak check: use macOS 'leaks' when available, else fall back to running the
# suite directly (sanitizer/Valgrind targets cover leaks on other platforms).
test-leaks: $(TESTBIN)
	@if command -v leaks >/dev/null 2>&1; then \
	  echo "leaks: $(TESTBIN)"; \
	  MallocStackLogging=1 leaks --atExit -- $(TESTBIN); \
	else \
	  echo "leaks(1) not found; running suite directly"; \
	  $(TESTBIN); \
	fi

install: build
	mkdir -p $(PREFIX)/bin $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/lib/pkgconfig
	cp $(APP) $(PREFIX)/bin/
	cp $(STATIC_LIB) $(PREFIX)/lib/
	[ "$(ENABLE_SHARED)" = "1" ] && cp $(SHARED_LIB) $(PREFIX)/lib/ || true
	cp include/json2toon.h $(PREFIX)/include/
	[ -f json2toon.pc ] && cp json2toon.pc $(PREFIX)/lib/pkgconfig/ || true

clean:
	rm -rf $(OBJDIR) $(STATIC_LIB) libjson2toon.so libjson2toon.dylib \
	       libjson2toon.dll json2toon json2toon.exe
