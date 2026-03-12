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

#pragma once
#include <string>
#include <set>
#include <utility>
#include "nlohmann/json.hpp"
#include "library_matching.h"

class BundleProcessor {
public:
    BundleProcessor(const nlohmann::json& platformCfg,
                    const std::string& bundlePath,
                    const nlohmann::json& appMetadata,
                    bool noDepWalking,
                    const std::string& libMatchingMode,
                    bool createMountPoints,
                    bool crunOnly);

    bool checkCompatibility();
    bool validateAppMetadataConfig();
    bool beginProcessing();

    // Returns {hostUid, hostGid} after resolving user namespace mappings
    std::pair<int,int> getRealUidGid();

    // Needed by main.cpp after processing
    nlohmann::json platformCfg;

private:
    // ── Processing steps (mirrors Python methods 1:1) ──────────────────────
    void createMountPointsUmoci();
    void processOciVersion();
    void processProcess();
    void processRoot();
    void processMounts();
    void processResources();
    void processGpu();
    void processDobbyPluginDependencies();
    void processUsersAndGroups();
    void processCapabilities();
    void processHostname();
    void processApparmorProfile();
    void processSeccomp();
    void addRdkPlugins();
    void processNetwork();
    void processStorage();
    void processLogging();
    void processDynamicDevices();
    void processIpc();
    void processMinidump();
    void processOomCrash();
    void processThunder();
    void processGpuPlugin();
    void processHooks();
    void writeConfigJson();
    void cleanupUmociLeftovers();

    // ── Helpers ─────────────────────────────────────────────────────────────
    bool compatibilityCheck();
    bool shouldGenerateCompliantConfig();
    void addBindMount(const std::string& src, const std::string& dst,
                      bool createMountPoint = false,
                      const nlohmann::json& options = nlohmann::json());
    void addMount(nlohmann::json& mount);
    void createAndWriteFileInRootfs(const std::string& path,
                                     const std::string& contents,
                                     int mode);
    void createEmptyDirInRootfs(const std::string& path);
    void addAnnotation(const std::string& key, const std::string& value);
    bool isMapped(int id, const nlohmann::json& mappings);
    void checkUidGidMappings();
    nlohmann::json loadConfig();

    std::string bundlePath;
    std::string rootfsPath;
    nlohmann::json appMetadata;
    nlohmann::json ociConfig;
    bool createMountPoints;
    bool crunOnly;
    std::set<std::string> handledLibs;
    LibraryMatching libMatcher;
};
