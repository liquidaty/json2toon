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

# Restrict the shared object's exports to the public API, keeping vendored yajl
# internal (its headers force default visibility, which -fvisibility=hidden can't
# undo). Done at link time: symbol glob on macOS, version script on ELF, nothing
# on Windows (yajl is not dllexport'd there).
ifeq ($(SHLIB_EXT),dylib)
  SHLIB_EXPORT := -Wl,-exported_symbol,_json2toon_* -Wl,-exported_symbol,_toon2json_*
else ifeq ($(SHLIB_EXT),so)
  SHLIB_EXPORT := -Wl,--version-script=json2toon.map
else
  SHLIB_EXPORT :=
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

# --------------------------------------------------------- vendored yajl
# The forward path (json2toon) parses JSON with the YAJL streaming parser, whose
# SAX callbacks drive the toonwriter library. A parser-only subset is vendored
# under third_party/yajl by default (self-contained cross builds);
# ./configure --with-yajl=DIR sets YAJL_VENDORED=0 / YAJL_LIBS=-lyajl instead. The
# defaults below let a standalone `make` (no ./configure) use the vendored copy.
YAJL_DIR := third_party/yajl
YAJL_VENDORED ?= 1
YAJL_CFLAGS ?= -I$(YAJL_DIR)
YAJL_LIBS ?=

ifeq ($(YAJL_VENDORED),1)
  YAJL_SRCS := $(YAJL_DIR)/yajl.c $(YAJL_DIR)/yajl_alloc.c $(YAJL_DIR)/yajl_buf.c \
               $(YAJL_DIR)/yajl_encode.c $(YAJL_DIR)/yajl_lex.c \
               $(YAJL_DIR)/yajl_parser.c $(YAJL_DIR)/yajl_version.c
  YAJL_OBJS := $(YAJL_SRCS:$(YAJL_DIR)/%.c=$(OBJDIR)/yajl/%.o)
else
  YAJL_SRCS :=
  YAJL_OBJS :=
endif

# --------------------------------------------------- vendored jsonwriter
# The reverse path (toon2json) serializes JSON through the vendored jsonwriter
# library. It is a single translation unit (jsonwriter.c #includes utils.c and
# json_numeric.c); -DINCLUDE_UTILS pulls in its escaping helper. Always vendored
# and always built (no system-library alternative), so cross builds stay self-
# contained, mirroring the yajl handling above.
JSONWRITER_DIR := third_party/jsonwriter
JSONWRITER_CFLAGS := -I$(JSONWRITER_DIR)
JSONWRITER_SRCS := $(JSONWRITER_DIR)/jsonwriter.c
JSONWRITER_OBJS := $(JSONWRITER_SRCS:$(JSONWRITER_DIR)/%.c=$(OBJDIR)/jsonwriter/%.o)
# jsonwriter.c is one TU that #includes utils.c and json_numeric.c; these are not
# compiled directly, so list them (plus the header) as extra prerequisites
# wherever jsonwriter.c is compiled -- otherwise editing them skips the rebuild.
JSONWRITER_TU_DEPS := $(JSONWRITER_DIR)/utils.c $(JSONWRITER_DIR)/json_numeric.c \
                      $(JSONWRITER_DIR)/jsonwriter.h

# --------------------------------------------------- vendored toonwriter
# The forward path (json2toon) serializes TOON through the vendored toonwriter
# library, which owns array buffering, inline/tabular/list shaping, quoting and
# the bounded RAM-then-temp-file store. It is a single translation unit
# (toonwriter.c #includes toon_numeric.c). Always vendored and always built, so
# cross builds stay self-contained, mirroring the jsonwriter handling above.
TOONWRITER_DIR := third_party/toonwriter
TOONWRITER_CFLAGS := -I$(TOONWRITER_DIR)
TOONWRITER_SRCS := $(TOONWRITER_DIR)/toonwriter.c
TOONWRITER_OBJS := $(TOONWRITER_SRCS:$(TOONWRITER_DIR)/%.c=$(OBJDIR)/toonwriter/%.o)
# toonwriter.c is one TU that #includes toon_numeric.c; same as JSONWRITER_TU_DEPS.
TOONWRITER_TU_DEPS := $(TOONWRITER_DIR)/toon_numeric.c $(TOONWRITER_DIR)/toon_numeric.h \
                      $(TOONWRITER_DIR)/toonwriter.h

# --------------------------------------------------------- vendored yatl
# The reverse path (toon2json) parses TOON with the yatl streaming SAX parser,
# whose callbacks drive the jsonwriter library -- the sole TOON parser, exactly
# as vendored yajl is the sole JSON parser for the forward path. Always vendored
# and always built (no system-library alternative), so cross builds stay self-
# contained, mirroring the yajl handling above. Build with -I$(YATL_DIR) so the
# sources' `#include <yatl/...>` resolves through third_party/yatl/yatl/.
YATL_DIR := third_party/yatl
YATL_CFLAGS := -I$(YATL_DIR)
YATL_SRCS := $(YATL_DIR)/yatl.c $(YATL_DIR)/yatl_parser.c $(YATL_DIR)/yatl_buf.c \
             $(YATL_DIR)/yatl_encode.c $(YATL_DIR)/yatl_alloc.c $(YATL_DIR)/yatl_version.c
YATL_OBJS := $(YATL_SRCS:$(YATL_DIR)/%.c=$(OBJDIR)/yatl/%.o)

ALL_CFLAGS := $(BASE_CFLAGS) $(OPT_CFLAGS) $(SAN_CFLAGS) $(YAJL_CFLAGS) $(JSONWRITER_CFLAGS) $(TOONWRITER_CFLAGS) $(YATL_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)
ALL_LDFLAGS := $(SAN_LDFLAGS) $(EXTRA_LDFLAGS) $(LDFLAGS)

# Vendored yajl is third-party: suppress its warnings (-w) and hide its symbols,
# but keep the opt/sanitiser/PIC flags (so ASan instruments it; links cleanly).
YAJL_OBJ_CFLAGS := -std=c11 -w $(OPT_CFLAGS) $(SAN_CFLAGS) -fPIC -fvisibility=hidden \
                   $(YAJL_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)

# Vendored jsonwriter is third-party: suppress its warnings (-w) and hide its
# symbols, but keep the opt/sanitiser/PIC flags; -DINCLUDE_UTILS compiles in its
# string-escaping helper (utils.c).
JSONWRITER_OBJ_CFLAGS := -std=c11 -w $(OPT_CFLAGS) $(SAN_CFLAGS) -fPIC -fvisibility=hidden \
                         -DINCLUDE_UTILS $(JSONWRITER_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)

# Vendored toonwriter: hide its symbols (the export filter keeps only json2toon_*
# / toon2json_*), keep opt/sanitiser/PIC flags so ASan instruments it.
TOONWRITER_OBJ_CFLAGS := -std=c11 -w $(OPT_CFLAGS) $(SAN_CFLAGS) -fPIC -fvisibility=hidden \
                         $(TOONWRITER_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)

# Vendored yatl is third-party: suppress its warnings (-w) and hide its symbols,
# but keep the opt/sanitiser/PIC flags (so ASan instruments it; links cleanly).
YATL_OBJ_CFLAGS := -std=c11 -w $(OPT_CFLAGS) $(SAN_CFLAGS) -fPIC -fvisibility=hidden \
                   $(YATL_CFLAGS) $(EXTRA_CFLAGS) $(CFLAGS)

# Applied only to the library's own translation units (not to consumers such as
# the CLI or the tests): export the annotated public API and hide every other
# symbol from the shared object's dynamic table.
LIB_CFLAGS := -DJSON2TOON_BUILD -fvisibility=hidden

# ----------------------------------------------------- sources / artifacts
LIB_SRCS := src/json2toon.c src/toon2json.c \
            src/format.c src/convenience.c
LIB_OBJS := $(LIB_SRCS:src/%.c=$(OBJDIR)/%.o)

STATIC_LIB := $(BUILDDIR)/libjson2toon.a

# ABI-versioned shared-library names. SHARED_LIB_REAL is the real object;
# SHARED_LIB is the dev/link name (a symlink to it, or the file itself on
# Windows); SHLIB_NAMES lists every installed file (real + symlinks).
ifeq ($(SHLIB_EXT),dll)
  SHARED_LIB_REAL := $(BUILDDIR)/libjson2toon.dll
  SHARED_LIB := $(SHARED_LIB_REAL)
  SHLIB_NAMES := libjson2toon.dll
else ifeq ($(SHLIB_EXT),dylib)
  SHARED_LIB_REAL := $(BUILDDIR)/libjson2toon.$(SOVERSION).dylib
  SHARED_LIB := $(BUILDDIR)/libjson2toon.dylib
  SHLIB_NAMES := libjson2toon.$(SOVERSION).dylib libjson2toon.dylib
else
  SHARED_LIB_REAL := $(BUILDDIR)/libjson2toon.so.$(VERSION)
  SHARED_LIB := $(BUILDDIR)/libjson2toon.so
  SHLIB_NAMES := libjson2toon.so.$(VERSION) libjson2toon.so.$(SOVERSION) libjson2toon.so
endif

APP := $(BUILDDIR)/json2toon$(EXE_EXT)
TESTBIN := $(BUILDDIR)/test$(EXE_EXT)

LIBS := $(STATIC_LIB)
ifeq ($(ENABLE_SHARED),1)
  LIBS += $(SHARED_LIB)
endif

.PHONY: help all build lib app test check test-leaks fuzz fuzz-standalone \
        strict install install-lib install-app uninstall uninstall-lib uninstall-app \
        clean distclean print-builddir
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
	@echo '  strict        strict-warnings compile of the library TUs (-Werror +)'
	@echo '  fuzz          build the libFuzzer target (needs an LLVM clang)'
	@echo '  fuzz-standalone  build the portable replay driver (any toolchain)'
	@echo '  install       install lib + CLI under PREFIX (default /usr/local)'
	@echo '  install-lib   install only the library, header and .pc'
	@echo '  install-app   install only the CLI'
	@echo '  uninstall     remove everything install put under PREFIX'
	@echo '  uninstall-lib / uninstall-app  remove only the lib / only the CLI'
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

$(OBJDIR)/yajl:
	mkdir -p $(OBJDIR)/yajl

$(OBJDIR)/jsonwriter:
	mkdir -p $(OBJDIR)/jsonwriter

$(OBJDIR)/toonwriter:
	mkdir -p $(OBJDIR)/toonwriter

$(OBJDIR)/yatl:
	mkdir -p $(OBJDIR)/yatl

$(OBJDIR)/%.o: src/%.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) $(LIB_CFLAGS) -c $< -o $@

# Vendored yajl translation units (only when YAJL_VENDORED=1).
$(OBJDIR)/yajl/%.o: $(YAJL_DIR)/%.c | $(OBJDIR)/yajl
	$(CC) $(YAJL_OBJ_CFLAGS) -c $< -o $@

# Vendored jsonwriter translation unit.
$(OBJDIR)/jsonwriter/%.o: $(JSONWRITER_DIR)/%.c | $(OBJDIR)/jsonwriter
	$(CC) $(JSONWRITER_OBJ_CFLAGS) -c $< -o $@
# The pattern rule only sees jsonwriter.c; name its #included sources explicitly
# (prerequisites-only, recipe stays the pattern rule's) so they force a rebuild.
$(OBJDIR)/jsonwriter/jsonwriter.o: $(JSONWRITER_TU_DEPS)

# Vendored toonwriter translation unit.
$(OBJDIR)/toonwriter/%.o: $(TOONWRITER_DIR)/%.c | $(OBJDIR)/toonwriter
	$(CC) $(TOONWRITER_OBJ_CFLAGS) -c $< -o $@
$(OBJDIR)/toonwriter/toonwriter.o: $(TOONWRITER_TU_DEPS)

# Vendored yatl translation units.
$(OBJDIR)/yatl/%.o: $(YATL_DIR)/%.c | $(OBJDIR)/yatl
	$(CC) $(YATL_OBJ_CFLAGS) -c $< -o $@

$(OBJDIR)/main.o: app/main.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(OBJDIR)/test.o: tests/test.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -c $< -o $@

$(STATIC_LIB): $(LIB_OBJS) $(YAJL_OBJS) $(JSONWRITER_OBJS) $(TOONWRITER_OBJS) $(YATL_OBJS)
	$(AR) rcs $@ $(LIB_OBJS) $(YAJL_OBJS) $(JSONWRITER_OBJS) $(TOONWRITER_OBJS) $(YATL_OBJS)
	$(RANLIB) $@ 2>/dev/null || true

$(SHARED_LIB_REAL): $(LIB_OBJS) $(YAJL_OBJS) $(JSONWRITER_OBJS) $(TOONWRITER_OBJS) $(YATL_OBJS)
	$(CC) $(ALL_LDFLAGS) $(SHLIB_LDFLAGS) $(SHLIB_EXPORT) -o $@ $(LIB_OBJS) $(YAJL_OBJS) $(JSONWRITER_OBJS) $(TOONWRITER_OBJS) $(YATL_OBJS) $(YAJL_LIBS)

# Dev/linker symlink -> real versioned object (ELF/macOS). On Windows
# SHARED_LIB == SHARED_LIB_REAL, so this rule is omitted (no ln, no symlinks).
ifneq ($(SHARED_LIB),$(SHARED_LIB_REAL))
$(SHARED_LIB): $(SHARED_LIB_REAL)
	ln -sf $(notdir $(SHARED_LIB_REAL)) $@
endif

$(APP): $(OBJDIR)/main.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/main.o $(STATIC_LIB) $(YAJL_LIBS)

$(TESTBIN): $(OBJDIR)/test.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/test.o $(STATIC_LIB) $(YAJL_LIBS)

test check: $(TESTBIN)
	$(TESTBIN)

# Strict-warnings lane (not part of the default build): syntax-check the
# library's own translation units under an explicit standard with extra
# diagnostics promoted to errors. Vendored yajl is third-party and excluded
# (it builds under -w). Run across compilers, e.g. `make strict CC=gcc`.
STRICT_WARN := -std=c11 -pedantic -Wall -Wextra -Werror \
               -Wconversion -Wshadow -Wstrict-prototypes -Wvla
strict:
	@for f in $(LIB_SRCS); do \
	  echo "  STRICT $$f"; \
	  $(CC) $(STRICT_WARN) -Iinclude -Isrc $(YAJL_CFLAGS) $(JSONWRITER_CFLAGS) $(TOONWRITER_CFLAGS) $(YATL_CFLAGS) -DJSON2TOON_BUILD \
	    -fsyntax-only $$f || exit 1; \
	done
	@echo "strict: clean ($(CC))"

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
# libFuzzer needs an LLVM clang built with libFuzzer; Apple's clang lacks it. On
# macOS, default FUZZ_CC to Homebrew's LLVM clang when the chosen clang is
# Apple's, and link with the classic linker -- the default ld rejects that newer
# toolchain's Mach-O objects ("invalid r_symbolnum"). Both stay unset on Linux,
# so CI (plain clang + libFuzzer) is unaffected. Override FUZZ_CC to force one.
FUZZ_LDFLAGS ?=
ifeq ($(UNAME_S),Darwin)
  ifeq ($(FUZZ_CC),clang)
    BREW_LLVM_PREFIX := $(shell brew --prefix llvm 2>/dev/null)
    ifneq ($(BREW_LLVM_PREFIX),)
      FUZZ_CC := $(BREW_LLVM_PREFIX)/bin/clang
    endif
  endif
  FUZZ_LDFLAGS += -Wl,-ld_classic
endif
FUZZBIN := $(BUILDDIR)/fuzz$(EXE_EXT)
FUZZSTANDALONE := $(BUILDDIR)/fuzz-standalone$(EXE_EXT)

$(FUZZBIN): $(LIB_SRCS) $(YAJL_SRCS) $(JSONWRITER_SRCS) $(JSONWRITER_TU_DEPS) $(TOONWRITER_SRCS) $(TOONWRITER_TU_DEPS) $(YATL_SRCS) tests/fuzz.c | $(OBJDIR)
	$(FUZZ_CC) $(BASE_CFLAGS) $(YAJL_CFLAGS) $(JSONWRITER_CFLAGS) $(TOONWRITER_CFLAGS) $(YATL_CFLAGS) -DINCLUDE_UTILS \
	  -g -O1 -fno-omit-frame-pointer \
	  -fsanitize=fuzzer,address,undefined $(FUZZ_LDFLAGS) -o $@ $(LIB_SRCS) $(YAJL_SRCS) $(JSONWRITER_SRCS) $(TOONWRITER_SRCS) $(YATL_SRCS) tests/fuzz.c $(YAJL_LIBS)

fuzz: $(FUZZBIN)
	@echo "built $(FUZZBIN)"
	@echo "run e.g.: $(FUZZBIN) -max_total_time=60 tests/corpus"

# Portable replay driver: runs each input file (or stdin) through the harness
# once. Buildable/runnable with any toolchain (no libFuzzer) -- used for CI
# smoke coverage and for replaying a crash file found by `make fuzz`.
$(OBJDIR)/fuzz-standalone.o: tests/fuzz.c | $(OBJDIR)
	$(CC) $(ALL_CFLAGS) -DJ2T_FUZZ_STANDALONE -c $< -o $@

$(FUZZSTANDALONE): $(OBJDIR)/fuzz-standalone.o $(STATIC_LIB)
	$(CC) $(ALL_LDFLAGS) -o $@ $(OBJDIR)/fuzz-standalone.o $(STATIC_LIB) $(YAJL_LIBS)

fuzz-standalone: $(FUZZSTANDALONE)
	@echo "built $(FUZZSTANDALONE)"

# Runtime symlinks beside the installed shared object: ELF wants the
# conventional real <- soname <- dev chain, macOS just the dev link, Windows none.
ifeq ($(SHLIB_EXT),dylib)
  INSTALL_SHLIB_LINKS = cd $(PREFIX)/lib && ln -sf libjson2toon.$(SOVERSION).dylib libjson2toon.dylib
else ifeq ($(SHLIB_EXT),so)
  INSTALL_SHLIB_LINKS = cd $(PREFIX)/lib && ln -sf libjson2toon.so.$(VERSION) libjson2toon.so.$(SOVERSION) && ln -sf libjson2toon.so.$(SOVERSION) libjson2toon.so
else
  INSTALL_SHLIB_LINKS = :
endif

install: install-lib install-app

install-lib: lib
	mkdir -p $(PREFIX)/lib $(PREFIX)/include $(PREFIX)/lib/pkgconfig
	cp $(STATIC_LIB) $(PREFIX)/lib/
	cp include/json2toon.h $(PREFIX)/include/
	[ -f json2toon.pc ] && cp json2toon.pc $(PREFIX)/lib/pkgconfig/ || true
ifeq ($(ENABLE_SHARED),1)
	cp $(SHARED_LIB_REAL) $(PREFIX)/lib/
	$(INSTALL_SHLIB_LINKS)
endif

install-app: app
	mkdir -p $(PREFIX)/bin
	cp $(APP) $(PREFIX)/bin/

uninstall: uninstall-lib uninstall-app

uninstall-lib:
	rm -f $(PREFIX)/lib/libjson2toon.a $(PREFIX)/include/json2toon.h \
	      $(PREFIX)/lib/pkgconfig/json2toon.pc \
	      $(addprefix $(PREFIX)/lib/,$(SHLIB_NAMES))

uninstall-app:
	rm -f $(PREFIX)/bin/json2toon$(EXE_EXT)

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
