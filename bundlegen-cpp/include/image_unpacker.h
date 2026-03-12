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

class ImageUnpacker {
public:
    ImageUnpacker(const std::string& src, const std::string& dst);

    // Unpack the OCI image at src:<tag> into dst using umoci.
    // If deleteAfter is true, removes src after unpacking.
    bool unpackImage(const std::string& tag, bool deleteAfter = false);

    // Returns true if the unpacked rootfs contains appmetadata.json
    bool imageContainsMetadata() const;

    // Delete the appmetadata.json from the unpacked rootfs
    void deleteImgAppMetadata();

    // Parse and return appmetadata.json from the unpacked rootfs (or empty JSON)
    nlohmann::json getAppMetadataFromImg() const;

private:
    bool umociFound;
    std::string src;
    std::string dst;
    std::string appMetadataImagePath;
};
