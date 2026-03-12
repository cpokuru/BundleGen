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

#include "readelf.h"
#include "utils.h"
#include "logger.h"

#include <unistd.h>
#include <string>
#include <vector>
#include <sstream>

std::vector<std::string> ReadElf::retrieveApiVersions(const std::string& libFullPath)
{
    std::vector<std::string> apiVersions;

    // Check if the file exists
    if (access(libFullPath.c_str(), F_OK) != 0) {
        LOG_DEBUG("ReadElf: file not found: %s", libFullPath.c_str());
        return apiVersions;
    }

    std::string command = "readelf -V " + libFullPath;
    auto [exitCode, output] = Utils::runProcessAndReturnOutput(command);

    if (exitCode != 0) {
        LOG_WARNING("readelf failed for %s", libFullPath.c_str());
        return apiVersions;
    }

    // Parse output - look for "Version definition section", then lines starting with "  0x"
    // that contain "Name: <version>". Stop at "Version needs section".
    bool inVersionDefinitionSection = false;
    std::istringstream stream(output);
    std::string line;
    const std::string TAG = "Name: ";

    while (std::getline(stream, line)) {
        if (inVersionDefinitionSection) {
            if (line.find("Version needs section") != std::string::npos) {
                inVersionDefinitionSection = false;
                break;
            }
            // Lines like: "  0x005c: Rev: 1  Flags: none  Index: 4  Cnt: 2  Name: GLIBC_2.6"
            if (line.size() >= 4 && line.substr(0, 4) == "  0x") {
                auto idx = line.find(TAG);
                if (idx != std::string::npos) {
                    std::string version = line.substr(idx + TAG.size());
                    // Trim trailing whitespace
                    while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' '))
                        version.pop_back();
                    apiVersions.push_back(version);
                }
            }
        } else if (line.find("Version definition section") != std::string::npos) {
            inVersionDefinitionSection = true;
        }
    }

    return apiVersions;
}
