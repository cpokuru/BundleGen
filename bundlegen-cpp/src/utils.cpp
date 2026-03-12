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

#include "utils.h"
#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <tuple>
#include <random>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <sys/wait.h>

// ─────────────────────────────────────────────────────────────────────────────
int Utils::runProcess(const std::string& command)
{
    LOG_DEBUG("Running: %s", command.c_str());
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        LOG_ERROR("popen failed for: %s", command.c_str());
        return -1;
    }

    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        // Strip trailing newline for cleaner log output
        size_t len = strlen(buf);
        if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
        LOG_DEBUG("%s", buf);
    }

    int status = pclose(pipe);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

// ─────────────────────────────────────────────────────────────────────────────
std::tuple<int, std::string> Utils::runProcessAndReturnOutput(const std::string& command)
{
    LOG_DEBUG("Running (capture): %s", command.c_str());
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        LOG_ERROR("popen failed for: %s", command.c_str());
        return {-1, ""};
    }

    std::string output;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe)) {
        output += buf;
    }

    int status = pclose(pipe);
    int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exitCode, output};
}

// ─────────────────────────────────────────────────────────────────────────────
std::string Utils::getRandomString(size_t length)
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<unsigned int> dis(0, 255);

    std::ostringstream oss;
    for (size_t i = 0; i < (length + 1) / 2; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << dis(gen);
    }

    std::string result = oss.str();
    if (result.size() > length)
        result.resize(length);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
void Utils::createControlFile(const nlohmann::json& platform,
                                const nlohmann::json& appMetadata)
{
    std::string packageName = appMetadata.value("id", "test_package");
    std::string version     = appMetadata.value("version", "1.0.0");
    std::string description = appMetadata.value("description", "some package");
    std::string priority    = appMetadata.value("priority", "optional");

    std::string architecture;
    if (platform.contains("arch")) {
        auto& arch = platform["arch"];
        architecture = arch.value("arch", "") + arch.value("variant", "");
    }

    std::ofstream f("control");
    if (!f) {
        LOG_ERROR("Failed to open 'control' file for writing");
        return;
    }
    f << "Package: "     << packageName  << "\n";
    f << "Version: "     << version      << "\n";
    f << "Architecture: "<< architecture << "\n";
    f << "Description: " << description  << "\n";
    f << "Priority: "    << priority     << "\n";
    f << "Depends: "     << ""           << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
bool Utils::createTgz(const std::string& srcDir, const std::string& destPath,
                       int uid, int gid, int fileMask)
{
    std::string output = destPath;
    if (output.size() < 7 || output.substr(output.size()-7) != ".tar.gz")
        output += ".tar.gz";

    LOG_INFO("Creating tgz of %s as %s", srcDir.c_str(), output.c_str());

    std::string cmd = "tar czf \"" + output + "\" -C \"" + srcDir + "\" .";

    if (uid >= 0)
        cmd += " --owner=" + std::to_string(uid);
    if (gid >= 0)
        cmd += " --group=" + std::to_string(gid);
    if (fileMask >= 0) {
        // Convert numeric mode mask to octal string for chmod-style use inside tar
        // (not directly supported by GNU tar flags, so we skip—kept for future)
        (void)fileMask;
    }

    int rc = runProcess(cmd);
    if (rc != 0) {
        LOG_ERROR("tar failed with code %d", rc);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool Utils::createIpk(const std::string& bundleDir, const std::string& destPath)
{
    const std::string DATA_NAME        = "data.tar.gz";
    const std::string CONTROL_NAME     = "control.tar.gz";
    const std::string DEBIAN_BIN_NAME  = "debian-binary";

    std::string output = destPath;
    if (output.size() < 4 || output.substr(output.size()-4) != ".ipk")
        output += ".ipk";

    // Create data tarball
    if (!createTgz(bundleDir, DATA_NAME))
        return false;

    // Create control tarball (control file must exist already)
    int rc = runProcess("tar czf " + CONTROL_NAME + " control");
    if (rc != 0) {
        LOG_ERROR("Failed to create control.tar.gz");
        return false;
    }

    // Create debian-binary
    {
        std::ofstream db(DEBIAN_BIN_NAME);
        if (!db) {
            LOG_ERROR("Failed to create debian-binary");
            return false;
        }
        db << "2.0";
    }

    // Bundle into .ipk (ar archive)
    std::string arCmd = "ar r \"" + output + "\" "
                       + DEBIAN_BIN_NAME + " "
                       + CONTROL_NAME + " "
                       + DATA_NAME;
    rc = runProcess(arCmd);

    // Cleanup temp files
    ::remove(DEBIAN_BIN_NAME.c_str());
    ::remove(CONTROL_NAME.c_str());
    ::remove(DATA_NAME.c_str());
    ::remove("control");

    if (rc != 0) {
        LOG_ERROR("ar failed with code %d", rc);
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
long long Utils::parseSize(const std::string& sizeStr)
{
    if (sizeStr.empty()) return 0LL;

    // Find where the numeric part ends
    size_t i = 0;
    while (i < sizeStr.size() && (std::isdigit(sizeStr[i]) || sizeStr[i] == '.'))
        ++i;

    double value = 0.0;
    try {
        value = std::stod(sizeStr.substr(0, i));
    } catch (const std::exception& e) {
        LOG_WARNING("parseSize: invalid numeric value in '%s': %s", sizeStr.c_str(), e.what());
        return 0LL;
    }
    std::string unit = sizeStr.substr(i);

    // Trim leading spaces in unit
    while (!unit.empty() && unit.front() == ' ')
        unit.erase(unit.begin());

    // Normalise to upper-case
    for (char& c : unit) c = (char)std::toupper((unsigned char)c);

    // Binary (IEC) units
    if (unit == "KIB")  return (long long)(value * 1024LL);
    if (unit == "MIB")  return (long long)(value * 1024LL * 1024LL);
    if (unit == "GIB")  return (long long)(value * 1024LL * 1024LL * 1024LL);
    if (unit == "TIB")  return (long long)(value * 1024LL * 1024LL * 1024LL * 1024LL);

    // Decimal (SI) units
    if (unit == "KB" || unit == "K")  return (long long)(value * 1000LL);
    if (unit == "MB" || unit == "M")  return (long long)(value * 1000LL * 1000LL);
    if (unit == "GB" || unit == "G")  return (long long)(value * 1000LL * 1000LL * 1000LL);
    if (unit == "TB" || unit == "T")  return (long long)(value * 1000LL * 1000LL * 1000LL * 1000LL);

    // Plain bytes
    if (unit.empty() || unit == "B")  return (long long)value;

    LOG_WARNING("parseSize: unknown unit '%s' in '%s', treating as bytes",
                unit.c_str(), sizeStr.c_str());
    return (long long)value;
}
