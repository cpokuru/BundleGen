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

#include "image_downloader.h"
#include "utils.h"
#include "logger.h"

#include <ctime>
#include <cstdlib>
#include <sys/stat.h>
#include <filesystem>

// ─────────────────────────────────────────────────────────────────────────────
// getImageTag is shared by both native and legacy paths
// ─────────────────────────────────────────────────────────────────────────────
std::string ImageDownloader::getImageTag(const std::string& url)
{
    // rsplit on ':', return last token
    auto pos = url.rfind(':');
    if (pos == std::string::npos)
        return "latest";

    std::string tag = url.substr(pos + 1);

    // If we found "//", there probably wasn't a tag and we just found the protocol
    if (tag.find("//") != std::string::npos)
        return "latest";

    return tag;
}

// ─────────────────────────────────────────────────────────────────────────────
#ifdef USE_SKOPEO_UMOCI
// ── Legacy skopeo-based implementation ────────────────────────────────────────

ImageDownloader::ImageDownloader() : skopeoFound(true)
{
    auto [rc, out] = Utils::runProcessAndReturnOutput("which skopeo 2>/dev/null");
    if (rc != 0 || out.empty()) {
        LOG_ERROR("Failed to find skopeo binary to download images");
        skopeoFound = false;
    } else {
        // Trim trailing newline
        std::string path = out;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        LOG_DEBUG("Using skopeo: %s", path.c_str());
    }
}

std::string ImageDownloader::downloadImage(const std::string& url,
                                            const std::string& creds,
                                            const nlohmann::json& platformCfg)
{
    if (!skopeoFound) {
        LOG_ERROR("Cannot download image as cannot find skopeo");
        return "";
    }

    if (!platformCfg.contains("arch")) {
        LOG_ERROR("Platform architecture is not defined");
        return "";
    }

    if (!platformCfg.contains("os")) {
        LOG_ERROR("Platform OS is not defined");
        return "";
    }

    std::string arch    = platformCfg["arch"].value("arch", "");
    std::string variant = platformCfg["arch"].value("variant", "");
    std::string os      = platformCfg.value("os", "linux");

    // Ensure /tmp/bundlegen exists
    ::mkdir("/tmp/bundlegen", 0755);

    // Generate timestamped destination path
    time_t t = std::time(nullptr);
    struct tm* tm_info = std::localtime(&t);
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d-%H%M%S", tm_info);

    std::string destination = std::string("/tmp/bundlegen/") + timebuf + "_" + Utils::getRandomString(8);
    LOG_INFO("Downloading image to %s...", destination.c_str());

    // Build skopeo command
    std::string cmd = "skopeo ";

    // Check for policy.json
    const char* home = std::getenv("HOME");
    std::string policyPath1 = home ? (std::string(home) + "/.config/containers/policy.json") : "";
    std::string policyPath2 = "/etc/containers/policy.json";

    struct stat st;
    bool hasPolicyJson = (!policyPath1.empty() && ::stat(policyPath1.c_str(), &st) == 0)
                      || (::stat(policyPath2.c_str(), &st) == 0);
    if (hasPolicyJson) {
        LOG_DEBUG("Found a policy.json file for skopeo");
    } else {
        LOG_DEBUG("Did not find a policy.json file for skopeo. Will use insecure-policy flag!");
        cmd += "--insecure-policy ";
    }

    if (!creds.empty())
        cmd += "--src-creds " + creds + " ";

    cmd += "--override-os " + os + " --override-arch " + arch + " ";
    if (!variant.empty())
        cmd += "--override-variant " + variant + " ";

    std::string imageTag = getImageTag(url);
    cmd += "copy " + url + " oci:" + destination + ":" + imageTag;

    LOG_DEBUG("%s", cmd.c_str());

    int rc = Utils::runProcess(cmd);
    if (rc == 0) {
        LOG_SUCCESS("Downloaded image from %s successfully to %s", url.c_str(), destination.c_str());
        return destination;
    }

    LOG_WARNING("Skopeo failed to download the image");
    return "";
}

#else  // USE_SKOPEO_UMOCI
// ── Native filesystem-based implementation ────────────────────────────────────

ImageDownloader::ImageDownloader()
{
    // No external tools required for native OCI directory copy
}

std::string ImageDownloader::downloadImage(const std::string& url,
                                            const std::string& creds,
                                            const nlohmann::json& platformCfg)
{
    (void)creds; // credentials are not needed for local oci: copies

    if (!platformCfg.contains("arch")) {
        LOG_ERROR("Platform architecture is not defined");
        return "";
    }

    if (!platformCfg.contains("os")) {
        LOG_ERROR("Platform OS is not defined");
        return "";
    }

    // Only local oci: URLs are supported natively
    if (url.size() < 4 || url.substr(0, 4) != "oci:") {
        LOG_WARNING("Remote image pull (non-oci: URL '%s') is not supported natively. "
                    "Build with USE_SKOPEO_UMOCI=1 to enable skopeo support.",
                    url.c_str());
        return "";
    }

    // Parse "oci:SRC:TAG" – strip leading "oci:" then strip trailing ":TAG"
    std::string srcWithTag = url.substr(4);
    std::string imageTag   = getImageTag(url);

    std::string srcPath = srcWithTag;
    if (!imageTag.empty() && srcPath.size() > imageTag.size() + 1) {
        srcPath = srcPath.substr(0, srcPath.size() - imageTag.size() - 1);
    }

    // Ensure /tmp/bundlegen exists
    std::error_code mkec;
    std::filesystem::create_directories("/tmp/bundlegen", mkec);

    // Generate timestamped destination path
    time_t t = std::time(nullptr);
    struct tm* tm_info = std::localtime(&t);
    char timebuf[32];
    std::strftime(timebuf, sizeof(timebuf), "%Y%m%d-%H%M%S", tm_info);

    std::string destination = std::string("/tmp/bundlegen/") + timebuf + "_" + Utils::getRandomString(8);
    LOG_INFO("Downloading image to %s...", destination.c_str());

    // Native recursive copy of the OCI layout directory
    std::error_code ec;
    std::filesystem::copy(srcPath, destination,
        std::filesystem::copy_options::recursive |
        std::filesystem::copy_options::overwrite_existing, ec);

    if (ec) {
        LOG_WARNING("Failed to copy OCI image from %s to %s: %s",
                    srcPath.c_str(), destination.c_str(), ec.message().c_str());
        return "";
    }

    LOG_SUCCESS("Downloaded image from %s successfully to %s", url.c_str(), destination.c_str());
    return destination;
}

#endif  // USE_SKOPEO_UMOCI

