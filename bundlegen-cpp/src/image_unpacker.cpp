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

#include "image_unpacker.h"
#include "utils.h"
#include "logger.h"

#include <unistd.h>
#include <fstream>

// ─────────────────────────────────────────────────────────────────────────────
ImageUnpacker::ImageUnpacker(const std::string& src, const std::string& dst)
    : umociFound(true), src(src), dst(dst)
{
    auto [rc, out] = Utils::runProcessAndReturnOutput("which umoci 2>/dev/null");
    if (rc != 0 || out.empty()) {
        LOG_ERROR("Failed to find umoci binary to unpack images");
        umociFound = false;
    } else {
        std::string path = out;
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
            path.pop_back();
        LOG_DEBUG("Using umoci: %s", path.c_str());
    }

    appMetadataImagePath = dst + "/rootfs/appmetadata.json";
}

// ─────────────────────────────────────────────────────────────────────────────
bool ImageUnpacker::unpackImage(const std::string& tag, bool deleteAfter)
{
    if (!umociFound) {
        LOG_ERROR("Cannot unpack image as cannot find umoci");
        return false;
    }

    std::string cmd = "umoci unpack --rootless --image " + src + ":" + tag + " " + dst;
    LOG_DEBUG("%s", cmd.c_str());

    int rc = Utils::runProcess(cmd);
    if (rc == 0) {
        LOG_INFO("Unpacked image successfully to %s", dst.c_str());
        if (deleteAfter) {
            LOG_DEBUG("Deleting downloaded image");
            Utils::runProcess("rm -rf \"" + src + "\"");
        }
        return true;
    }

    LOG_WARNING("Umoci failed to unpack the image");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
bool ImageUnpacker::imageContainsMetadata() const
{
    return access(appMetadataImagePath.c_str(), F_OK) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
void ImageUnpacker::deleteImgAppMetadata()
{
    if (imageContainsMetadata()) {
        LOG_DEBUG("Deleting app metadata file from unpacked image");
        ::unlink(appMetadataImagePath.c_str());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
nlohmann::json ImageUnpacker::getAppMetadataFromImg() const
{
    if (!imageContainsMetadata())
        return nlohmann::json();

    std::ifstream f(appMetadataImagePath);
    if (!f) {
        LOG_ERROR("Failed to open appmetadata.json: %s", appMetadataImagePath.c_str());
        return nlohmann::json();
    }

    try {
        return nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse appmetadata.json: %s", e.what());
        return nlohmann::json();
    }
}
