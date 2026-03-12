# BundleGen C++ — OCI Bundle Generator for RDK-B

A C++17 port of the BundleGen Python tool for native deployment on RDK-B
(broadband/gateway) devices that do not ship a Python runtime.

---

## Overview

BundleGen downloads OCI images using **skopeo**, unpacks them using **umoci**,
then processes and modifies `config.json` for the **Dobby** container manager
on RDK devices.

This C++ version:
- Compiles to a single static-linkable binary
- Requires no Python runtime on the target device
- Cross-compiles cleanly with a Yocto/RDK-B toolchain
- Uses only one external C++ dependency: **nlohmann/json** (header-only)

---

## Prerequisites

| Package | Purpose | Notes |
|---------|---------|-------|
| `g++ >= 7` | C++17 compiler | GCC 7+ or Clang 5+ |
| `libssl-dev` | SHA256 for storage paths | OpenSSL dev headers |
| `curl` | Downloading nlohmann/json | For `make deps` only |
| `skopeo` | Downloading OCI images | Runtime dependency |
| `umoci` | Unpacking OCI images | Runtime dependency |

---

## Setup

The only C++ header dependency is [nlohmann/json](https://github.com/nlohmann/json)
(single-header, MIT licensed). Download it with:

```bash
cd bundlegen-cpp
make deps
```

This fetches `json.hpp v3.11.3` into `include/nlohmann/json.hpp`.

---

## Native Build

```bash
# Install build dependencies (Debian/Ubuntu)
sudo apt-get install -y g++ libssl-dev curl

# Clone the repo and enter the C++ directory
git clone https://github.com/cpokuru/BundleGen
cd BundleGen/bundlegen-cpp

# Download nlohmann/json header
make deps

# Build
make

# Run
./bundlegen --help
```

---

## RDK-B Cross-Compilation

### Using Yocto SDK environment script

```bash
# Source the RDK-B toolchain environment
source /opt/rdk/toolchain/environment-setup-cortexa9hf-neon-rdk-linux-gnueabi

# Enter the C++ directory
cd BundleGen/bundlegen-cpp

# Download dependency (done once on the build host)
make deps

# Cross-compile
make CROSS_COMPILE="${CROSS_COMPILE}" SYSROOT="${SDKTARGETSYSROOT}"

# Optional: strip debug symbols to reduce binary size
make strip
```

### Manual toolchain setup

```bash
export CROSS_COMPILE=arm-rdk-linux-gnueabi-
export SYSROOT=/opt/rdk/sysroot

cd bundlegen-cpp
make deps
make CROSS_COMPILE=${CROSS_COMPILE} SYSROOT=${SYSROOT}
make strip
```

### aarch64 example

```bash
export CROSS_COMPILE=aarch64-rdk-linux-
export SYSROOT=/opt/rdk/sysroot-aarch64
make CROSS_COMPILE=${CROSS_COMPILE} SYSROOT=${SYSROOT}
```

---

## Yocto / BitBake Integration

Create a recipe (e.g. `bundlegen_git.bb`) in your layer:

```bitbake
SUMMARY = "BundleGen C++ - OCI Bundle generator for Dobby/RDK"
DESCRIPTION = "C++17 port of BundleGen - generates OCI bundles for Dobby containers"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

SRC_URI = "git://github.com/cpokuru/BundleGen;branch=master;protocol=https \
           https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp;name=jsonhpp;subdir=bundlegen-cpp/include/nlohmann"

SRC_URI[jsonhpp.sha256sum] = "9bea4c8066ef4a1c206b2be5a35a7e7b32b17f58cd94c84e42e09a3c83ebc3e3"

SRCREV = "${AUTOREV}"
S = "${WORKDIR}/git/bundlegen-cpp"

DEPENDS = "openssl"
RDEPENDS:${PN} = "skopeo umoci"

do_compile() {
    oe_runmake CROSS_COMPILE="${TARGET_PREFIX}" \
               SYSROOT="${STAGING_DIR_TARGET}" \
               CXXFLAGS="${CXXFLAGS}" \
               LDFLAGS="${LDFLAGS}"
}

do_install() {
    install -d ${D}${bindir}
    install -m 0755 ${B}/bundlegen ${D}${bindir}/bundlegen
}
```

---

## Usage

The CLI is identical to the Python version:

```bash
# Basic usage
./bundlegen docker://hello-world:latest /tmp/output -p rpi3_reference

# With credentials
./bundlegen docker://registry.example.com/myapp:latest /tmp/output \
    -p rpi4_reference \
    -c username:password

# Generate .ipk instead of .tar.gz
./bundlegen docker://myapp:1.0 /tmp/output -p rpi3_reference -i

# Verbose output
./bundlegen -vvv docker://hello-world:latest /tmp/output -p rpi3_reference

# Custom app metadata
./bundlegen docker://myapp:latest /tmp/output \
    -p rpi3_reference \
    -a /path/to/appmetadata.json

# crun-only bundle (no Dobby)
./bundlegen docker://myapp:latest /tmp/output -p rpi3_reference -u

# Override app ID
./bundlegen docker://myapp:latest /tmp/output -p rpi3_reference -x myapp-v2

# Use platform from custom search path
./bundlegen docker://myapp:latest /tmp/output \
    -p myplatform \
    -s /path/to/platform/templates
```

### Environment Variables

| Variable | Flag | Description |
|----------|------|-------------|
| `RDK_PLATFORM` | `-p` | Platform name |
| `RDK_PLATFORM_SEARCHPATH` | `-s` | Template search path |
| `RDK_OCI_REGISTRY_CREDS` | `-c` | Registry credentials |

---

## Directory Structure

```
bundlegen-cpp/
├── Makefile                   # Build system
├── README_CPP.md              # This file
├── include/
│   ├── logger.h               # Logging macros (ERROR/WARNING/INFO/DEBUG/TRACE)
│   ├── utils.h                # Utility functions
│   ├── capabilities.h         # Default Linux capabilities
│   ├── readelf.h              # readelf wrapper for API version detection
│   ├── stb_platform.h         # Platform template loader
│   ├── image_downloader.h     # skopeo wrapper
│   ├── image_unpacker.h       # umoci wrapper
│   ├── library_matching.h     # Host/rootfs library decision engine
│   ├── bundle_processor.h     # Main OCI config processor
│   └── nlohmann/
│       └── json.hpp           # Downloaded by `make deps`
└── src/
    ├── main.cpp               # CLI entry point
    ├── utils.cpp
    ├── capabilities.cpp
    ├── readelf.cpp
    ├── stb_platform.cpp
    ├── image_downloader.cpp
    ├── image_unpacker.cpp
    ├── library_matching.cpp
    └── bundle_processor.cpp
```

---

## Library Dependency Map

| Python library | C++ equivalent | Notes |
|----------------|----------------|-------|
| `loguru` | `logger.h` macros | Custom, no external dep |
| `jsonschema` | JSON parse check | Schema validation is best-effort |
| `humanfriendly` | `Utils::parseSize()` | Supports KiB/MiB/GiB/MB/GB |
| `click` | `getopt_long` | Standard POSIX |
| `hashlib.sha256` | `openssl/sha.h` | Links with `-lcrypto` |
| `json` | `nlohmann/json` | Single header, MIT license |
| `subprocess` | `popen()/pclose()` | POSIX standard |
| `os.walk` | `std::filesystem` | C++17 standard |

---

## Troubleshooting

### `json.hpp: No such file or directory`
Run `make deps` first to download the nlohmann/json header.

### Cross-compilation: `cannot find -lstdc++fs`
On GCC < 9, `libstdc++fs` may be a separate library in the sysroot.  
On GCC >= 9, `-lstdc++fs` is a no-op (the functionality is in `libstdc++`).  
Add it explicitly to sysroot if missing:
```
SYSROOT/usr/lib/libstdc++fs.a
```

### Cross-compilation: `cannot find -lcrypto`
Ensure OpenSSL is part of your sysroot. In Yocto, add `DEPENDS = "openssl"`.

### `skopeo: command not found`
skopeo must be installed on the **target** device.  
Yocto: add `RDEPENDS:${PN} = "skopeo umoci"` to your recipe.

### `umoci: command not found`
Same as above — umoci must be installed on the target device.

### Filesystem errors during build
Ensure your build host has GCC 7+ and the `<filesystem>` header is available:
```bash
g++ --version
echo '#include <filesystem>' | g++ -std=c++17 -x c++ - -c -o /dev/null
```
