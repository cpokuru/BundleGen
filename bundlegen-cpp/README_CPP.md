# BundleGen C++ — OCI Bundle Generator for RDK-B

A C++17 port of the BundleGen Python tool for native deployment on RDK-B
(broadband/gateway) devices that do not ship a Python runtime.

---

## Overview

BundleGen natively unpacks OCI images using **libarchive** and processes
`config.json` for the **Dobby** container manager on RDK devices.

This C++ version:
- Compiles to a single, self-contained binary
- Requires **no Python runtime** and **no skopeo/umoci** on the target device
- Cross-compiles cleanly with a Yocto/RDK-B toolchain
- Natively handles `oci:` local image directories (OCI Image Layout)
- Uses libarchive for tar/tar.gz/tar.zst/tar.xz layer extraction

---

## Prerequisites

| Package | Purpose | Notes |
|---------|---------|-------|
| `g++ >= 7` | C++17 compiler | GCC 7+ or Clang 5+ |
| `libssl-dev` | SHA256 for storage paths | OpenSSL dev headers |
| `libarchive-dev` | OCI layer extraction | **Build-time dep; `libarchive.so` at runtime** |
| `curl` | Downloading nlohmann/json | For `make deps` only |

> **Runtime dependencies**: only `libarchive.so` (already present on RDK-B images).
> `skopeo` and `umoci` are **not** needed on the device.

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
sudo apt-get install -y g++ libssl-dev libarchive-dev curl

# Clone the repo and enter the C++ directory
git clone https://github.com/cpokuru/BundleGen
cd BundleGen/bundlegen-cpp

# Check that required libraries are available
make deps-check

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

DEPENDS = "openssl libarchive"
RDEPENDS:${PN} = ""

do_compile() {
    oe_runmake CROSS_COMPILE="${TARGET_PREFIX}" \
               SYSROOT="${STAGING_DIR_TARGET}" \
               CXXFLAGS="${CXXFLAGS}" \
               LDFLAGS="${LDFLAGS} -larchive"
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
│   ├── image_downloader.h     # OCI image copy (native) or skopeo wrapper
│   ├── image_unpacker.h       # OCI unpacker (libarchive) or umoci wrapper
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
| `subprocess` | `popen()/pclose()` | POSIX standard (used for readelf/tar only) |
| `os.walk` | `std::filesystem` | C++17 standard |
| `skopeo` (subprocess) | `std::filesystem::copy` | Native OCI dir copy |
| `umoci` (subprocess) | `libarchive` | Native OCI layer extraction |

---

## Troubleshooting

### `json.hpp: No such file or directory`
Run `make deps` first to download the nlohmann/json header.

### `archive.h: No such file or directory` or `cannot find -larchive`
Install the libarchive development package:
```bash
sudo apt-get install -y libarchive-dev
```
In Yocto, add `DEPENDS = "libarchive"` to your recipe.

### Cross-compilation: `cannot find -lstdc++fs`
On GCC < 9, `libstdc++fs` may be a separate library in the sysroot.  
On GCC >= 9, `-lstdc++fs` is a no-op (the functionality is in `libstdc++`).  
Add it explicitly to sysroot if missing:
```
SYSROOT/usr/lib/libstdc++fs.a
```

### Cross-compilation: `cannot find -lcrypto`
Ensure OpenSSL is part of your sysroot. In Yocto, add `DEPENDS = "openssl"`.

### Remote image pull not supported
The native build only supports local `oci:` image sources.  
To pull from a remote Docker registry, build with the legacy subprocess mode:
```bash
make USE_SKOPEO_UMOCI=1
```
This requires `skopeo` and `umoci` to be installed on the target device.

### Filesystem errors during build
Ensure your build host has GCC 7+ and the `<filesystem>` header is available:
```bash
g++ --version
echo '#include <filesystem>' | g++ -std=c++17 -x c++ - -c -o /dev/null
```
