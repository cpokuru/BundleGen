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
#include <tuple>
#include "nlohmann/json.hpp"

class Utils {
public:
    // Run a command via popen, stream output to LOG_DEBUG, return exit code
    static int runProcess(const std::string& command);

    // Run a command via popen, capture stdout, return {exitcode, output}
    static std::tuple<int, std::string> runProcessAndReturnOutput(const std::string& command);

    // Generate a random hex string of given length
    static std::string getRandomString(size_t length = 32);

    // Write a Debian-style control file using fields from platform/appMetadata JSON
    static void createControlFile(const nlohmann::json& platform,
                                   const nlohmann::json& appMetadata);

    // Create a .tar.gz of srcDir. uid/gid/fileMask are optional (-1 = not set).
    static bool createTgz(const std::string& srcDir, const std::string& destPath,
                           int uid = -1, int gid = -1, int fileMask = -1);

    // Create a .ipk from bundleDir (ar archive of control.tar.gz + data.tar.gz)
    static bool createIpk(const std::string& bundleDir, const std::string& destPath);

    // Parse human-friendly size strings like "128MiB", "1GiB", "512MB" → bytes
    static long long parseSize(const std::string& sizeStr);
};
