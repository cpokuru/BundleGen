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
#include "nlohmann/json.hpp"

class ImageDownloader {
public:
    ImageDownloader();

    // Extract the image tag from a URL (e.g. docker://repo/name:tag → "tag")
    static std::string getImageTag(const std::string& url);

    // Download (or copy) an OCI image to a temp directory.
    // For oci: URLs, performs a native recursive directory copy.
    // For remote URLs (docker://, etc.) with USE_SKOPEO_UMOCI=1, uses skopeo.
    // Returns the destination path on success, empty string on failure.
    std::string downloadImage(const std::string& url,
                               const std::string& creds,
                               const nlohmann::json& platformCfg);

private:
#ifdef USE_SKOPEO_UMOCI
    bool skopeoFound;
#endif
};
