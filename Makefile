# json2toon - build the libjson2toon library, the json2toon CLI, and tests.
#
# Quick start (run `make help` for the full list):
#   make build         build the library + CLI into a per-target build dir
#   make test          build and run the test suite
#   make install       copy the built artifacts under $(PREFIX)
#   make clean         remove all build output
#
# Artifacts never land in the source tree. They go under a directory keyed by
# the target triple, the compiler, and the build variant:
#
#   $(BUILDROOT)/<target-triple>/<cc>-<version>/<variant>/
#
# so release/debug/asan builds and different toolchains (native, mingw64,
# emscripten, cross gcc) coexist without ever clobbering one another.
# `make install` is the only step that writes outside the build tree.
#
# Flags: ASAN=1 (AddressSanitizer + UBSan), DEBUG=1 (debug build).
# Environment CC/AR/RANLIB/CFLAGS/LDFLAGS and CONFIGFILE are honoured. The
# config file written by ./configure is included when present; otherwise
# sensible defaults let the targets work standalone.

CONFIGFILE ?= config.mk
-include $(CONFIGFILE)

CC      ?= cc
AR      ?= ar
RANLIB  ?= ranlib
PREFIX  ?= /usr/local

# Executable suffix for the linker output. ./configure writes this into the
# config file (".exe" for Windows/mingw targets, empty elsewhere); default to
# empty so a standalone `make` without ./configure still works.
EXE_EXT ?=

# --------------------------------------------- target / compiler / variant id
# Derived from the compiler's own configuration so it is correct when cross-
# compiling: -dumpmachine prints the *target* triple (native, mingw64,
# emscripten and cross toolchains alike) and -dumpversion the compiler version.
# Neither runs a target binary, matching configure's no-run-probe policy.
TARGET_TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
ifeq ($(TARGET_TRIPLE),)
  TARGET_TRIPLE := unknown-target
endif
CC_NAME := $(notdir $(CC))
CC_VERSION := $(shell $(CC) -dumpversion 2>/dev/null)
ifeq ($(CC_VERSION),)
  CC_VERSION := 0
endif

ifeq ($(ASAN),1)
  VARIANT := asan
else ifeq ($(DEBUG),1)
  VARIANT := debug
else ifeq ($(DEBUG_DEFAULT),1)
  VARIANT := debug
else
  VARIANT := release
endif

# The parent dir is target-platform-specific; override BUILDROOT to relocate
# the whole tree (e.g. to a temp filesystem): make build BUILDROOT=/tmp/j2t
BUILDROOT ?= build
BUILDID   := $(TARGET_TRIPLE)/$(CC_NAME)-$(CC_VERSION)/$(VARIANT)
BUILDDIR  := $(BUILDROOT)/$(BUILDID)
OBJDIR    := $(BUILDDIR)/obj

# ------------------------------------------------ shared-library conventions
UNAME_S := $(shell uname -s 2>/dev/null)

# Library version (single source of truth: the public header). SOVERSION is the
# major component and is the shared-object ABI name. Used for the soname and for
# the versioned file names / install symlinks below.
VERSION := $(shell sed -n 's/.*JSON2TOON_VERSION "\([^"]*\)".*/\1/p' include/json2toon.h | head -1)
VERSION := $(if $(VERSION),$(VERSION),0.0.0)
SOVERSION := $(firstword $(subst ., ,$(VERSION)))

# Default link flags when ./configure did not provide them (standalone make).
# ./configure writes the same versioned forms into the config file.
ifeq ($(SHLIB_EXT),)
  ifeq ($(UNAME_S),Darwin)
    SHLIB_EXT := dylib
    SHLIB_LDFLAGS := -dynamiclib \
        -install_name $(PREFIX)/lib/libjson2toon.$(SOVERSION).dylib \
        -compatibility_version $(SOVERSION) -current_version $(VERSION)
  else
    SHLIB_EXT := so
    SHLIB_LDFLAGS := -shared -Wl,-soname,libjson2toon.so.$(SOVERSION)
  endif
endif

ENABLE_SHARED ?= 1
ifneq ($(STATIC_BUILD),)
  ifneq ($(STATIC_BUILD),0)
    ENABLE_SHARED := 0
  endif
endif

# ------------------------------------------------------------------- flags
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

# Applied only to the library's own translation units (not to consumers such as
# the CLI or the tests): export the annotated public API and hide every other
# symbol from the shared object's dynamic table.
LIB_CFLAGS := -DJSON2TOON_BUILD -fvisibility=hidden

# ----------------------------------------------------- sources / artifacts
LIB_SRCS := src/json2toon.c src/toon2json.c src/dom.c src/format.c src/simd.c \
            src/convenience.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.o)

STATIC_LIB := $(BUILDDIR)/libjson2toon.a

# Shared-library file names, ABI-versioned on ELF/macOS. SHARED_LIB_REAL is the
# real object; SHARED_LIB is the linker/dev name (a symlink to it on ELF/macOS,
# the file itself on Windows). install(1) lays down the runtime symlinks too.
ifeq ($(SHLIB_EXT),dll)
  SHARED_LIB_REAL := $(BUILDDIR)/libjson2toon.dll
  SHARED_LIB := $(SHARED_LIB_REAL)
else ifeq ($(SHLIB_EXT),dylib)
  SHARED_LIB_REAL := $(BUILDDIR)/libjson2toon.$(SOVERSION).dylib
  SHARED_LIB := $(BUILDDIR)/libjson2toon.dylib
else
  SHARED_LIB_REAL := $(BUILDDIR)/libjson2toon.so.$(VERSION)
  SHARED_LIB := $(BUILDDIR)/libjson2toon.so
endif

APP := $(BUILDDIR)/json2toon$(EXE_EXT)
TESTBIN := $(BUILDDIR)/test$(EXE_EXT)

LIBS := $(STATIC_LIB)
ifeq ($(ENABLE_SHARED),1)
  LIBS += $(SHARED_LIB)
endif

.PHONY: help all build lib app test check test-leaks fuzz fuzz-standalone \
        clean distclean install print-builddir
.DEFAULT_GOAL := help

help:
	@echo 'json2toon - available make targets:'
	@echo ''
	@echo '  help          show this list (default target)'
	@echo '  build         build libjson2toon (static + shared) and the json2toon CLI'
	@echo '  lib           build the libraries only'
	@echo '  app           build the json2toon CLI only'
	@echo '  test          build and run the unit/round-trip test suite'
	@echo '  check         alias for test'
	@echo '  test-leaks    run the test suite under a leak checker'
	@echo '  fuzz          build the libFuzzer target (needs an LLVM clang)'
	@echo '  fuzz-standalone  build the portable replay driver (any toolchain)'
	@echo '  install       copy built artifacts under PREFIX (default /usr/local)'
	@echo '  clean         remove the entire build tree (all targets/variants)'
	@echo '  distclean     clean + remove ./configure output and legacy artifacts'
	@echo '  print-builddir  print the resolved build directory for this config'
	@echo ''
	@echo 'Flags:  ASAN=1 (AddressSanitizer+UBSan)   DEBUG=1 (debug build)'
	@echo 'Vars:   CC, AR, RANLIB, CFLAGS, LDFLAGS, PREFIX, BUILDROOT'
	@echo ''
	@echo 'This config builds into: $(BUILDDIR)'

all: build
build: $(LIBS) $(APP)
lib: $(LIBS)
app: $(APP)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) $(LIB_CFLAGS) -c $< -o $@

$(OBJDIR)/main.o: app/main.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(OBJDIR)/test.o: tests/test.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $(LIB_OBJS)
	$(RANLIB) $@ 2>/dev/null || true

$(SHARED_LIB_REAL): $(LIB_OBJS)
	$(CC) $(ALL_LDFLAGS) $(SHLIB_LDFLAGS) -o $@ $(LIB_OBJS)

# Dev/linker symlink -> real versioned object (ELF/macOS). On Windows
# SHARED_LIB == SHARED_LIB_REAL, so this rule is omitted (no ln, no symlinks).
ifneq ($(SHARED_LIB),$(SHARED_LIB_REAL))
$(SHARED_LIB): $(SHARED_LIB_REAL)
	ln -sf $(notdir $(SHARED_LIB_REAL)) $@
endif

$(APP): $(OBJDIR)/main.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/main.o $(STATIC_LIB)

$(TESTBIN): $(OBJDIR)/test.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/test.o $(STATIC_LIB)

test check: $(TESTBIN)
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

# ------------------------------------------------------------------- fuzzing
# libFuzzer build. Compiles the library sources together with the harness in one
# clang invocation so coverage/sanitizer instrumentation reaches the parsers.
# Needs an LLVM clang with libFuzzer (Apple clang lacks it -- use the Linux CI
# or a real clang). Override the compiler with FUZZ_CC=...
FUZZ_CC ?= clang
FUZZBIN := $(BUILDDIR)/fuzz$(EXE_EXT)
FUZZSTANDALONE := $(BUILDDIR)/fuzz-standalone$(EXE_EXT)

$(FUZZBIN): $(LIB_SRCS) tests/fuzz.c | $(OBJDIR)
	$(FUZZ_CC) $(BASE_CFLAGS) -g -O1 -fno-omit-frame-pointer \
	  -fsanitize=fuzzer,address,undefined -o $@ $(LIB_SRCS) tests/fuzz.c

fuzz: $(FUZZBIN)
	@echo "built $(FUZZBIN)"
	@echo "run e.g.: $(FUZZBIN) -max_total_time=60 tests/corpus"

# Portable replay driver: runs each input file (or stdin) through the harness
# once. Buildable/runnable with any toolchain (no libFuzzer) -- used for CI
# smoke coverage and for replaying a crash file found by `make fuzz`.
$(OBJDIR)/fuzz-standalone.o: tests/fuzz.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -DJ2T_FUZZ_STANDALONE -c $< -o $@

$(FUZZSTANDALONE): $(OBJDIR)/fuzz-standalone.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/fuzz-standalone.o $(STATIC_LIB)

fuzz-standalone: $(FUZZSTANDALONE)
	@echo "built $(FUZZSTANDALONE)"

# Runtime symlinks created next to the installed shared object. ELF gets the
# conventional three names (real <- soname <- dev); macOS gets the dev link to
# the major-versioned dylib; Windows (.dll) gets none.
ifeq ($(SHLIB_EXT),dylib)
  INSTALL_SHLIB_LINKS = cd $(PREFIX)/lib && ln -sf libjson2toon.$(SOVERSION).dylib libjson2toon.dylib
else ifeq ($(SHLIB_EXT),so)
  INSTALL_SHLIB_LINKS = cd $(PREFIX)/lib && ln -sf libjson2toon.so.$(VERSION) libjson2toon.so.$(SOVERSION) && ln -sf libjson2toon.so.$(SOVERSION) libjson2toon.so
else
  INSTALL_SHLIB_LINKS = :
endif

install: build
	mkdir -p $(PREFIX)/bin $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/lib/pkgconfig
	cp $(APP) $(PREFIX)/bin/
	cp $(STATIC_LIB) $(PREFIX)/lib/
	cp include/json2toon.h $(PREFIX)/include/
	[ -f json2toon.pc ] && cp json2toon.pc $(PREFIX)/lib/pkgconfig/ || true
ifeq ($(ENABLE_SHARED),1)
	cp $(SHARED_LIB_REAL) $(PREFIX)/lib/
	$(INSTALL_SHLIB_LINKS)
endif

print-builddir:
	@echo $(BUILDDIR)

clean:
	rm -rf $(BUILDROOT)

# Also remove configure output and any legacy in-tree artifacts from older
# (pre per-target build dir) layouts.
distclean: clean
	rm -f config.mk config.sbx json2toon.pc
	rm -rf obj
	rm -f libjson2toon.a libjson2toon.so libjson2toon.dylib libjson2toon.dll \
	      json2toon
