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

#pragma once
#include <string>
#include <set>
#include <functional>
#include "nlohmann/json.hpp"

class LibraryMatching {
public:
    using MountFunc = std::function<void(const std::string& src,
                                          const std::string& dst,
                                          bool createMountPoint)>;

    LibraryMatching(const nlohmann::json& platformCfg,
                    const std::string& bundlePath,
                    MountFunc addMountFunc,
                    bool noDepWalking,
                    const std::string& libMatchingMode,
                    bool createMountPoints);

    // Unconditionally bind-mount src → dst
    void mount(const std::string& src, const std::string& dst);

    // Decide whether to mount lib from host or use the one in rootfs
    void mountOrUseRootfs(const std::string& src, const std::string& dst);

private:
    void determineSublibs();
    void removeFromRootfs(const std::string& rootfsFilepath);
    void takeHostLib(const std::string& srcLib, const std::string& dstLib,
                      const nlohmann::json& apiInfo);
    void takeRootfsLib(const std::string& dstLib, const nlohmann::json& apiInfo);

    nlohmann::json platformCfg;
    std::string bundlePath;
    std::string rootfsPath;
    std::string libMatchingMode;
    bool noDepWalking;
    bool createMountPoints;
    std::set<std::string> handledLibs;
    MountFunc addMountFunc;
    nlohmann::json subLibs;  // unused placeholder kept for future use
};
