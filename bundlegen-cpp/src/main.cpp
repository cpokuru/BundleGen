// If not stated otherwise in this file or this component's license file the
// following copyright and licenses apply:
//
// Copyright 2024 Consult Red
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "logger.h"
#include "stb_platform.h"
#include "image_downloader.h"
#include "image_unpacker.h"
#include "bundle_processor.h"
#include "utils.h"

#include <getopt.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// Global log level – default to SUCCESS (quiet) until -v flags are parsed
LogLevel g_logLevel = LogLevel::SUCCESS;

// ─────────────────────────────────────────────────────────────────────────────
static void printUsage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s [OPTIONS] IMAGE OUTPUTDIR\n"
        "\n"
        "Generate an OCI Bundle for a specified platform\n"
        "\n"
        "Positional arguments:\n"
        "  IMAGE               OCI image URL (e.g. docker://hello-world:latest)\n"
        "  OUTPUTDIR           Output directory path\n"
        "\n"
        "Options:\n"
        "  -p, --platform        PLATFORM  Platform name [env: RDK_PLATFORM] (required)\n"
        "  -s, --searchpath      PATH      Platform template search path [env: RDK_PLATFORM_SEARCHPATH]\n"
        "  -c, --creds           CREDS     Registry credentials user:pass [env: RDK_OCI_REGISTRY_CREDS]\n"
        "  -i, --ipk                       Output as .ipk instead of .tar.gz\n"
        "  -a, --appmetadata     PATH      Path to app metadata JSON file\n"
        "  -y, --yes                       Auto-confirm overwrite of existing output dir\n"
        "  -n, --nodepwalking              Disable library dependency walking\n"
        "  -m, --libmatchingmode MODE      Library matching mode: normal|image|host (default: normal)\n"
        "  -r, --createmountpoints         Create mount points in rootfs\n"
        "  -x, --appid           ID        Override app id from metadata\n"
        "  -u, --crun                      Generate crun-compatible bundle (no Dobby)\n"
        "  -v, --verbose                   Increase verbosity (repeat up to 3 times)\n"
        "  -h, --help                      Show this help message\n"
        "\n",
        prog);
}

// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    // ── Default values ────────────────────────────────────────────────────────
    std::string platform;
    std::string searchPath;
    std::string creds;
    std::string appMetadataPath;
    std::string libMatchingMode = "normal";
    std::string appIdOverride;
    bool ipk               = false;
    bool autoYes           = false;
    bool noDepWalking      = false;
    bool createMountPoints = false;
    bool crunOnly          = false;
    int  verbose           = 0;

    // Pick up env vars as defaults (CLI flags override them)
    if (const char* e = std::getenv("RDK_PLATFORM"))            platform      = e;
    if (const char* e = std::getenv("RDK_PLATFORM_SEARCHPATH")) searchPath    = e;
    if (const char* e = std::getenv("RDK_OCI_REGISTRY_CREDS")) creds         = e;

    // ── Parse arguments ──────────────────────────────────────────────────────
    static const struct option longOpts[] = {
        {"platform",         required_argument, nullptr, 'p'},
        {"searchpath",       required_argument, nullptr, 's'},
        {"creds",            required_argument, nullptr, 'c'},
        {"ipk",              no_argument,       nullptr, 'i'},
        {"appmetadata",      required_argument, nullptr, 'a'},
        {"yes",              no_argument,       nullptr, 'y'},
        {"nodepwalking",     no_argument,       nullptr, 'n'},
        {"libmatchingmode",  required_argument, nullptr, 'm'},
        {"createmountpoints",no_argument,       nullptr, 'r'},
        {"appid",            required_argument, nullptr, 'x'},
        {"crun",             no_argument,       nullptr, 'u'},
        {"verbose",          no_argument,       nullptr, 'v'},
        {"help",             no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:s:c:ia:ynm:rx:uvh", longOpts, nullptr)) != -1) {
        switch (opt) {
            case 'p': platform        = optarg; break;
            case 's': searchPath      = optarg; break;
            case 'c': creds           = optarg; break;
            case 'i': ipk             = true;   break;
            case 'a': appMetadataPath = optarg; break;
            case 'y': autoYes         = true;   break;
            case 'n': noDepWalking    = true;   break;
            case 'm': libMatchingMode = optarg; break;
            case 'r': createMountPoints = true; break;
            case 'x': appIdOverride   = optarg; break;
            case 'u': crunOnly        = true;   break;
            case 'v': ++verbose;                break;
            case 'h': printUsage(argv[0]); return 0;
            default:  printUsage(argv[0]); return 1;
        }
    }

    // ── Positional arguments ──────────────────────────────────────────────────
    if (optind + 2 != argc) {
        fprintf(stderr, "Error: expected IMAGE and OUTPUTDIR positional arguments.\n\n");
        printUsage(argv[0]);
        return 1;
    }

    std::string image     = argv[optind];
    std::string outputDir = argv[optind + 1];

    // Validate required options
    if (platform.empty()) {
        fprintf(stderr, "Error: --platform (-p) is required (or set RDK_PLATFORM).\n\n");
        printUsage(argv[0]);
        return 1;
    }

    // Validate libmatchingmode
    if (libMatchingMode != "normal" && libMatchingMode != "image" && libMatchingMode != "host") {
        fprintf(stderr, "Error: --libmatchingmode must be one of: normal, image, host\n\n");
        return 1;
    }

    // ── Set log level from verbosity count ────────────────────────────────────
    if (verbose > 3) verbose = 3;
    switch (verbose) {
        case 0: g_logLevel = LogLevel::SUCCESS; break;
        case 1: g_logLevel = LogLevel::INFO;    break;
        case 2: g_logLevel = LogLevel::DEBUG;   break;
        case 3: g_logLevel = LogLevel::TRACE;   break;
    }

    LOG_INFO("Generating new OCI bundle from %s for %s", image.c_str(), platform.c_str());

    // ── Make outputDir absolute ───────────────────────────────────────────────
    {
        std::error_code ec;
        outputDir = fs::absolute(outputDir, ec).string();
        if (ec) {
            LOG_ERROR("Failed to resolve output directory path: %s", ec.message().c_str());
            return 1;
        }
    }

    // ── Handle existing output directory ─────────────────────────────────────
    if (fs::exists(outputDir)) {
        if (!autoYes) {
            fprintf(stdout,
                "The directory %s already exists. Continue and delete it? [y/N] ",
                outputDir.c_str());
            fflush(stdout);
            int ch = getchar();
            if (ch != 'y' && ch != 'Y') {
                fprintf(stderr, "Aborted.\n");
                return 1;
            }
        }
        std::error_code ec;
        fs::remove_all(outputDir, ec);
        if (ec) {
            LOG_ERROR("Failed to remove existing output directory: %s", ec.message().c_str());
            return 1;
        }
    }

    // ── Load platform config ──────────────────────────────────────────────────
    STBPlatform selectedPlatform(platform, searchPath);
    if (!selectedPlatform.foundConfig()) {
        LOG_ERROR("Could not find config for platform %s", platform.c_str());
        return 1;
    }

    if (!selectedPlatform.validatePlatformConfig()) {
        LOG_ERROR("Validation of platform config FAILED with schema");
        return 1;
    }

    // ── Download image ────────────────────────────────────────────────────────
    ImageDownloader imgDownloader;
    std::string imgPath = imgDownloader.downloadImage(image, creds, selectedPlatform.getConfig());
    if (imgPath.empty()) {
        LOG_ERROR("Image download failed");
        return 1;
    }

    // ── Unpack image ──────────────────────────────────────────────────────────
    std::string imageTag = ImageDownloader::getImageTag(image);
    ImageUnpacker imgUnpacker(imgPath, outputDir);
    if (!imgUnpacker.unpackImage(imageTag, /*deleteAfter=*/true)) {
        LOG_ERROR("Image unpack failed");
        return 1;
    }

    // ── Load app metadata ─────────────────────────────────────────────────────
    nlohmann::json appMetadataDict;

    if (!appMetadataPath.empty()) {
        // User provided metadata path
        std::error_code ec;
        std::string absPath = fs::absolute(appMetadataPath, ec).string();

        LOG_INFO("Selecting User Provided appmetadata path %s", absPath.c_str());
        if (!fs::exists(absPath)) {
            LOG_ERROR("App metadata file %s does not exist", absPath.c_str());
            return 1;
        }
        std::ifstream f(absPath);
        if (!f) {
            LOG_ERROR("Failed to open app metadata file: %s", absPath.c_str());
            return 1;
        }
        try {
            appMetadataDict = nlohmann::json::parse(f);
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("Failed to parse app metadata: %s", e.what());
            return 1;
        }
    } else {
        nlohmann::json metaFromImg = imgUnpacker.getAppMetadataFromImg();
        if (!metaFromImg.empty()) {
            LOG_INFO("Selecting appmetadata from OCI image");
            appMetadataDict = metaFromImg;
            imgUnpacker.deleteImgAppMetadata();
        } else {
            LOG_ERROR("Cannot find app metadata file in OCI image and none provided to BundleGen");
            return 1;
        }
    }

    // Override app id if requested
    if (!appIdOverride.empty())
        appMetadataDict["id"] = appIdOverride;

    // ── Process bundle ────────────────────────────────────────────────────────
    BundleProcessor processor(
        selectedPlatform.getConfig(),
        outputDir,
        appMetadataDict,
        noDepWalking,
        libMatchingMode,
        createMountPoints,
        crunOnly);

    if (!processor.checkCompatibility()) {
        std::error_code ec;
        fs::remove_all(outputDir, ec);
        return 2;
    }

    if (!processor.validateAppMetadataConfig()) {
        LOG_ERROR("Validation of app metadata FAILED with schema");
        return 1;
    }

    if (!processor.beginProcessing()) {
        LOG_WARNING("Failed to produce bundle");
        return 3;
    }

    // ── Create output archive ─────────────────────────────────────────────────
    if (ipk) {
        Utils::createControlFile(selectedPlatform.getConfig(), appMetadataDict);
        Utils::createIpk(outputDir, outputDir);
        LOG_SUCCESS("Successfully generated bundle at %s.ipk", outputDir.c_str());
    } else {
        // Determine uid/gid/fileMask from tarball settings
        int uid = -1, gid = -1, fileMask = -1;
        if (selectedPlatform.getConfig().contains("tarball")) {
            const auto& tball = selectedPlatform.getConfig()["tarball"];
            bool useOwnership = tball.value("fileOwnershipSameAsUser", false);
            std::string maskStr = tball.value("fileMask", "");

            if (!maskStr.empty()) {
                try { fileMask = std::stoi(maskStr, nullptr, 8); } catch (...) {}
            }

            if (useOwnership) {
                auto [realUid, realGid] = processor.getRealUidGid();
                uid = realUid;
                gid = realGid;
            }
        }

        Utils::createTgz(outputDir, outputDir, uid, gid, fileMask);
        LOG_SUCCESS("Successfully generated bundle at %s.tar.gz", outputDir.c_str());
    }

    return 0;
}
