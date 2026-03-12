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
#include <vector>
#include "nlohmann/json.hpp"

class STBPlatform {
public:
    STBPlatform(const std::string& name, const std::string& searchPath = "");

    bool foundConfig() const;
    bool validatePlatformConfig();
    const nlohmann::json& getConfig() const;

private:
    void searchConfig();
    void parseConfig();

    std::string name;
    std::string searchPath;
    std::vector<std::string> configFiles;
    nlohmann::json config;
};
