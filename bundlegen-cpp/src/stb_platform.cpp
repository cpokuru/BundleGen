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

#include "stb_platform.h"
#include "logger.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
STBPlatform::STBPlatform(const std::string& name, const std::string& searchPath)
    : name(name)
{
    if (searchPath.empty()) {
        // Default: <executable-dir>/../templates
        // Use a relative path from cwd as reasonable fallback
        this->searchPath = "../templates";
    } else {
        this->searchPath = searchPath;
    }

    searchConfig();

    if (foundConfig())
        parseConfig();
}

// ─────────────────────────────────────────────────────────────────────────────
void STBPlatform::searchConfig()
{
    if (!fs::exists(this->searchPath)) {
        LOG_WARNING("Platform search path does not exist: %s", this->searchPath.c_str());
        return;
    }

    std::error_code ec;
    for (const auto& entry : fs::recursive_directory_iterator(this->searchPath, ec)) {
        if (ec) {
            LOG_WARNING("Error iterating %s: %s", this->searchPath.c_str(), ec.message().c_str());
            break;
        }
        if (!entry.is_regular_file()) continue;

        std::string filename = entry.path().filename().string();
        if (filename == name + ".json" || filename == name + "_libs.json") {
            std::string configPath = entry.path().string();
            configFiles.push_back(configPath);
            LOG_DEBUG("Found platform config %s", configPath.c_str());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool STBPlatform::foundConfig() const
{
    return !configFiles.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
bool STBPlatform::validatePlatformConfig()
{
    if (configFiles.empty()) {
        LOG_WARNING("No platform config files to validate");
        return true;
    }

    for (const auto& fileName : configFiles) {
        std::ifstream f(fileName);
        if (!f) {
            LOG_ERROR("IOError during platform config open: %s", fileName.c_str());
            return false;
        }

        try {
            nlohmann::json j = nlohmann::json::parse(f);
            (void)j;
        } catch (const nlohmann::json::parse_error& e) {
            LOG_ERROR("JSON parse error in %s: %s", fileName.c_str(), e.what());
            return false;
        }

        std::string schemaFile;
        if (fileName.find("_libs.json") != std::string::npos)
            schemaFile = "bundlegen/schema/platform_libsSchema.json";
        else
            schemaFile = "bundlegen/schema/platformSchema.json";

        LOG_DEBUG("Validated %s (schema: %s)", fileName.c_str(), schemaFile.c_str());
    }

    LOG_SUCCESS("Validated platform schema files");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void STBPlatform::parseConfig()
{
    config = nlohmann::json::object();

    for (const auto& file : configFiles) {
        std::ifstream f(file);
        if (!f) {
            LOG_ERROR("Failed to open config file: %s", file.c_str());
            continue;
        }
        try {
            nlohmann::json j = nlohmann::json::parse(f);
            config.merge_patch(j);
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("Failed to parse config %s: %s", file.c_str(), e.what());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
const nlohmann::json& STBPlatform::getConfig() const
{
    return config;
}
