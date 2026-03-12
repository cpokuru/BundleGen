// If not stated otherwise in this file or this component's license file the
// following copyright and licenses apply:
//
// Copyright 2024 Liberty Global B.V.
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

#include "library_matching.h"
#include "readelf.h"
#include "logger.h"

#include <unistd.h>
#include <regex>
#include <set>

// ─────────────────────────────────────────────────────────────────────────────
LibraryMatching::LibraryMatching(const nlohmann::json& platformCfg,
                                  const std::string& bundlePath,
                                  MountFunc addMountFunc,
                                  bool noDepWalking,
                                  const std::string& libMatchingMode,
                                  bool createMountPoints)
    : platformCfg(platformCfg)
    , bundlePath(bundlePath)
    , rootfsPath(bundlePath + "/rootfs")
    , libMatchingMode(libMatchingMode)
    , noDepWalking(noDepWalking)
    , createMountPoints(createMountPoints)
    , addMountFunc(addMountFunc)
{
    determineSublibs();

    if (noDepWalking) {
        LOG_INFO("Library dependency walking is DISABLED!");
    } else if (!platformCfg.contains("libs") || platformCfg["libs"].is_null()) {
        LOG_WARNING("Library dependency walking DISABLED because no _libs.json file available!");
    }
    LOG_DEBUG("Libmatching mode: %s", libMatchingMode.c_str());
}

// ─────────────────────────────────────────────────────────────────────────────
void LibraryMatching::removeFromRootfs(const std::string& rootfsFilepath)
{
    char realBuf[4096] = {};
    if (::realpath(rootfsFilepath.c_str(), realBuf) != nullptr) {
        std::string realLink(realBuf);
        ::unlink(rootfsFilepath.c_str());
        if (realLink != rootfsFilepath)
            ::unlink(realLink.c_str());
    } else {
        ::unlink(rootfsFilepath.c_str());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void LibraryMatching::takeHostLib(const std::string& srcLib,
                                   const std::string& dstLib,
                                   const nlohmann::json& apiInfo)
{
    LOG_TRACE("HOST version chosen: %s", srcLib.c_str());
    handledLibs.insert(dstLib);

    std::string rootfsFilepath = rootfsPath + "/" + [&]{
        std::string s = dstLib;
        while (!s.empty() && s.front() == '/') s.erase(s.begin());
        return s;
    }();

    if (access(rootfsFilepath.c_str(), F_OK) == 0) {
        LOG_TRACE("Removing from rootfs: %s", dstLib.c_str());
        removeFromRootfs(rootfsFilepath);
    }

    addMountFunc(srcLib, dstLib, createMountPoints);

    if (!apiInfo.is_null() && apiInfo.contains("sublibs")) {
        for (const auto& sublib : apiInfo["sublibs"]) {
            std::string sublibStr = sublib.get<std::string>();
            LOG_TRACE("HOST version chosen (sublib): %s", sublibStr.c_str());
            handledLibs.insert(sublibStr);

            std::string sublibRootfs = rootfsPath + "/" + [&]{
                std::string s = sublibStr;
                while (!s.empty() && s.front() == '/') s.erase(s.begin());
                return s;
            }();

            if (access(sublibRootfs.c_str(), F_OK) == 0) {
                LOG_TRACE("Removing from rootfs: %s", sublibStr.c_str());
                removeFromRootfs(sublibRootfs);
            }
            addMountFunc(sublibStr, sublibStr, createMountPoints);
        }
    }

    if (!apiInfo.is_null() && apiInfo.contains("deps")) {
        for (const auto& neededLib : apiInfo["deps"]) {
            std::string neededStr = neededLib.get<std::string>();
            mountOrUseRootfs(neededStr, neededStr);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void LibraryMatching::takeRootfsLib(const std::string& dstLib,
                                     const nlohmann::json& apiInfo)
{
    LOG_TRACE("OCI IMAGE version chosen: %s", dstLib.c_str());
    handledLibs.insert(dstLib);

    if (!apiInfo.is_null() && apiInfo.contains("sublibs")) {
        for (const auto& sublib : apiInfo["sublibs"]) {
            LOG_TRACE("OCI IMAGE version chosen (sublib): %s", sublib.get<std::string>().c_str());
            handledLibs.insert(sublib.get<std::string>());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void LibraryMatching::mountOrUseRootfs(const std::string& src, const std::string& dst)
{
    LOG_TRACE("Explicitly adding for host mount or from OCI: %s", dst.c_str());

    if (handledLibs.count(dst)) {
        LOG_TRACE("No need to add explicitly (already handled): %s", dst.c_str());
        return;
    }

    // Find API info for this lib in platform_cfg['libs']
    nlohmann::json apiInfo = nlohmann::json(nullptr);
    if (!noDepWalking && platformCfg.contains("libs") && platformCfg["libs"].is_array()) {
        for (const auto& lib : platformCfg["libs"]) {
            if (lib.value("name", "") == src) {
                apiInfo = lib;
                break;
            }
        }
        if (apiInfo.is_null())
            LOG_TRACE("No api info found for %s", dst.c_str());
    }

    if (apiInfo.is_null()) {
        if (libMatchingMode == "image") {
            std::string rootfsFilepath = rootfsPath + "/" + [&]{
                std::string s = dst;
                while (!s.empty() && s.front() == '/') s.erase(s.begin());
                return s;
            }();
            if (access(rootfsFilepath.c_str(), F_OK) != 0) {
                LOG_TRACE("Lib not inside OCI image rootfs: %s", dst.c_str());
                takeHostLib(src, dst, nlohmann::json(nullptr));
            } else {
                LOG_TRACE("OCI Image %s forcibly chosen.", dst.c_str());
                takeRootfsLib(dst, nlohmann::json(nullptr));
            }
        } else {
            // normal and host modes
            takeHostLib(src, dst, nlohmann::json(nullptr));
        }
        return;
    }

    // If this is a sublib in normal mode, switch to parent lib logic
    if (libMatchingMode == "normal" && apiInfo.contains("parentlib")) {
        std::string parentLib = apiInfo["parentlib"].get<std::string>();
        mountOrUseRootfs(parentLib, parentLib);
        return;
    }

    std::string rootfsFilepath = rootfsPath + "/" + [&]{
        std::string s = dst;
        while (!s.empty() && s.front() == '/') s.erase(s.begin());
        return s;
    }();

    if (access(rootfsFilepath.c_str(), F_OK) != 0) {
        LOG_TRACE("Lib not inside OCI image rootfs: %s", dst.c_str());
        takeHostLib(src, dst, apiInfo);
        return;
    }

    if (libMatchingMode == "image") {
        LOG_TRACE("OCI Image %s forcibly chosen.", dst.c_str());
        takeRootfsLib(dst, apiInfo);
        return;
    } else if (libMatchingMode == "host") {
        LOG_TRACE("Host %s forcibly chosen.", dst.c_str());
        takeHostLib(src, dst, apiInfo);
        return;
    }

    // Normal mode: compare API versions
    if (apiInfo.contains("apiversions") && !apiInfo["apiversions"].empty()) {
        std::set<std::string> hostVersions;
        for (const auto& v : apiInfo["apiversions"])
            hostVersions.insert(v.get<std::string>());

        auto rootfsVersionsVec = ReadElf::retrieveApiVersions(rootfsFilepath);
        std::set<std::string> rootfsVersions(rootfsVersionsVec.begin(), rootfsVersionsVec.end());

        // Host has more or same versions → use host
        bool hostIsSupersetOrEqual = std::includes(
            hostVersions.begin(), hostVersions.end(),
            rootfsVersions.begin(), rootfsVersions.end());

        bool rootfsIsStrictlyMore = std::includes(
            rootfsVersions.begin(), rootfsVersions.end(),
            hostVersions.begin(), hostVersions.end()) && (rootfsVersions != hostVersions);

        if (hostIsSupersetOrEqual) {
            takeHostLib(src, dst, apiInfo);
        } else if (rootfsIsStrictlyMore) {
            LOG_TRACE("OCI Image %s has more API versions", dst.c_str());
            takeRootfsLib(dst, apiInfo);
        } else {
            LOG_ERROR("Cannot decide which library to choose for %s - defaulting to OCI image", dst.c_str());
            takeRootfsLib(dst, apiInfo);
        }
    } else {
        takeHostLib(src, dst, apiInfo);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void LibraryMatching::mount(const std::string& src, const std::string& dst)
{
    LOG_TRACE("Explicitly adding for host mount: %s", dst.c_str());
    if (handledLibs.count(dst))
        LOG_TRACE("No need to add explicitly: %s", dst.c_str());

    nlohmann::json apiInfo = nlohmann::json(nullptr);
    if (!noDepWalking && platformCfg.contains("libs") && platformCfg["libs"].is_array()) {
        for (const auto& lib : platformCfg["libs"]) {
            if (lib.value("name", "") == src) {
                apiInfo = lib;
                break;
            }
        }
        if (apiInfo.is_null())
            LOG_TRACE("No api info found for %s", dst.c_str());
    }

    takeHostLib(src, dst, apiInfo);
}

// ─────────────────────────────────────────────────────────────────────────────
void LibraryMatching::determineSublibs()
{
    if (noDepWalking || !platformCfg.contains("libs") || !platformCfg["libs"].is_array())
        return;

    const auto& libs = platformCfg["libs"];
    size_t maxCnt = 0;

    // We need a mutable copy since we patch it
    nlohmann::json libsCopy = libs;

    std::regex libRe("^/(?:usr/)?lib/lib\\S+\\.so\\.\\d+$");
    std::regex ldRe("^/(?:usr/)?lib/ld-linux\\S*\\.so\\.\\d+$");

    std::vector<size_t> sublibIndices;
    size_t libcIdx = SIZE_MAX;

    for (size_t i = 0; i < libsCopy.size(); ++i) {
        auto& lib = libsCopy[i];
        std::string libname = lib.value("name", "");

        bool match = std::regex_match(libname, libRe) || std::regex_match(libname, ldRe);
        if (!match) continue;

        size_t cnt = 0;
        if (lib.contains("apiversions") && lib["apiversions"].is_array()) {
            for (const auto& v : lib["apiversions"]) {
                if (v.get<std::string>().rfind("GLIBC_", 0) == 0)
                    ++cnt;
            }
        }

        if (cnt == 0) continue;
        size_t total = lib.contains("apiversions") ? lib["apiversions"].size() : 0;
        if (cnt != total) continue;

        sublibIndices.push_back(i);
        if (cnt > maxCnt) {
            libcIdx = i;
            maxCnt = cnt;
        }
    }

    if (libcIdx == SIZE_MAX) return;

    LOG_TRACE("Found libc: %s", libsCopy[libcIdx].value("name", "").c_str());

    // Tag sublibs with parentlib pointer
    for (size_t i : sublibIndices) {
        if (i == libcIdx) continue;
        libsCopy[libcIdx]["sublibs"].push_back(libsCopy[i]["name"]);
        libsCopy[i]["parentlib"] = libsCopy[libcIdx]["name"];
        libsCopy[i]["apiversions"] = nlohmann::json::array();
    }

    // Patch back into platformCfg (platformCfg is a value member, not const)
    platformCfg["libs"] = libsCopy;
}
