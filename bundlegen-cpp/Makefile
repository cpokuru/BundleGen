# BundleGen C++ - Makefile for native and RDK-B cross-compilation
#
# Usage:
#   Native build:
#     make deps   (first time only - downloads nlohmann/json header)
#     make
#
#   Cross-compile for RDK-B (ARM):
#     make deps
#     make CROSS_COMPILE=arm-rdk-linux-gnueabi- \
#          SYSROOT=/path/to/rdk/sysroot
#
#   Install:
#     make install PREFIX=/usr/local
#
#   Clean:
#     make clean
#
#   Legacy skopeo/umoci fallback (subprocess-based):
#     make USE_SKOPEO_UMOCI=1

# ──────────────────────────────────────────────
# Toolchain
# ──────────────────────────────────────────────
CROSS_COMPILE   ?=
CC              := $(CROSS_COMPILE)gcc
CXX             := $(CROSS_COMPILE)g++
AR              := $(CROSS_COMPILE)ar
STRIP           := $(CROSS_COMPILE)strip

# ──────────────────────────────────────────────
# Sysroot (for cross-compilation)
# ──────────────────────────────────────────────
SYSROOT         ?=
ifneq ($(SYSROOT),)
  SYSROOT_FLAGS := --sysroot=$(SYSROOT)
else
  SYSROOT_FLAGS :=
endif

# ──────────────────────────────────────────────
# Paths
# ──────────────────────────────────────────────
PREFIX          ?= /usr/local
BINDIR          := $(PREFIX)/bin
SRCDIR          := src
INCDIR          := include
OBJDIR          := obj
TARGET          := bundlegen

# nlohmann/json header (single-header, vendored)
JSON_INCLUDE    ?= $(INCDIR)

# ──────────────────────────────────────────────
# Compiler flags
# ──────────────────────────────────────────────
CXXSTD          := -std=c++17
OPTFLAGS        := -O2
WARNFLAGS       := -Wall -Wextra -Wno-unused-parameter
DEBUGFLAGS      ?=

# Set USE_SKOPEO_UMOCI=1 to build with subprocess-based skopeo/umoci fallback
USE_SKOPEO_UMOCI ?= 0

CXXFLAGS        := $(CXXSTD) $(OPTFLAGS) $(WARNFLAGS) $(DEBUGFLAGS) \
                   $(SYSROOT_FLAGS) \
                   -I$(INCDIR) \
                   -I$(JSON_INCLUDE)

ifeq ($(USE_SKOPEO_UMOCI),1)
  CXXFLAGS += -DUSE_SKOPEO_UMOCI
endif

# -lstdc++fs required for GCC < 9; harmless on GCC >= 9
# -larchive required for native OCI layer extraction (libarchive-dev)
LDFLAGS         := $(SYSROOT_FLAGS) -lcrypto -lssl -larchive

# ──────────────────────────────────────────────
# Sources & Objects
# ──────────────────────────────────────────────
SRCS := $(wildcard $(SRCDIR)/*.cpp)
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))

# ──────────────────────────────────────────────
# Build targets
# ──────────────────────────────────────────────
.PHONY: all clean install uninstall strip deps deps-check help

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build successful: $(TARGET)"

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJDIR):
	mkdir -p $(OBJDIR)

clean:
	rm -rf $(OBJDIR) $(TARGET)
	@echo "Cleaned."

strip: $(TARGET)
	$(STRIP) $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	@echo "Installed to $(DESTDIR)$(BINDIR)/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

deps:
	@echo "Downloading nlohmann/json header..."
	mkdir -p $(INCDIR)/nlohmann
	curl -fsSL https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp \
	     -o $(INCDIR)/nlohmann/json.hpp
	@echo "Done. You can now run 'make'."

deps-check:
	@# Note: this check uses dpkg/pkg-config and is oriented towards Debian/Ubuntu.
	@# On other distributions, check manually that libarchive-dev and libssl-dev are installed.
	@echo "Checking build dependencies..."
	@pkg-config --exists libarchive 2>/dev/null || \
	  dpkg -s libarchive-dev >/dev/null 2>&1 || \
	  { echo "ERROR: libarchive-dev is not installed."; \
	    echo "  Install with: sudo apt-get install -y libarchive-dev"; exit 1; }
	@echo "  libarchive-dev ... OK"
	@pkg-config --exists openssl 2>/dev/null || \
	  dpkg -s libssl-dev >/dev/null 2>&1 || \
	  { echo "ERROR: libssl-dev is not installed."; \
	    echo "  Install with: sudo apt-get install -y libssl-dev"; exit 1; }
	@echo "  libssl-dev ....... OK"
	@echo "All required build dependencies are present."

help:
	@echo "BundleGen C++ Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all         - Build bundlegen (default)"
	@echo "  deps        - Download nlohmann/json single-header dependency"
	@echo "  deps-check  - Check that libarchive-dev and libssl-dev are installed"
	@echo "  clean       - Remove build artifacts"
	@echo "  strip       - Strip debug symbols from binary"
	@echo "  install     - Install to PREFIX (default: /usr/local)"
	@echo "  uninstall   - Remove installed binary"
	@echo "  help        - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  CROSS_COMPILE    - Toolchain prefix (e.g. arm-rdk-linux-gnueabi-)"
	@echo "  SYSROOT          - Sysroot for cross-compilation"
	@echo "  PREFIX           - Install prefix (default: /usr/local)"
	@echo "  DEBUGFLAGS       - Extra debug flags (e.g. -g -DDEBUG)"
	@echo "  USE_SKOPEO_UMOCI - Set to 1 to use skopeo/umoci subprocesses instead"
	@echo "                     of the native libarchive implementation (default: 0)"
	@echo ""
	@echo "Build dependencies:"
	@echo "  libarchive-dev   - Required for native OCI layer extraction"
	@echo "  libssl-dev       - Required for SHA256 (storage paths)"
	@echo ""
	@echo "Examples:"
	@echo "  make deps"
	@echo "  make deps-check"
	@echo "  make"
	@echo "  make CROSS_COMPILE=arm-rdk-linux-gnueabi- SYSROOT=/opt/rdk/sysroot"
	@echo "  make install PREFIX=/usr"
	@echo "  make DEBUGFLAGS='-g -DDEBUG'"
	@echo "  make USE_SKOPEO_UMOCI=1   # legacy skopeo/umoci subprocess mode"
