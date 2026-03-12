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
#include <filesystem>

// ─────────────────────────────────────────────────────────────────────────────
// imageContainsMetadata, deleteImgAppMetadata and getAppMetadataFromImg are
// identical for both native and legacy paths.
// ─────────────────────────────────────────────────────────────────────────────

bool ImageUnpacker::imageContainsMetadata() const
{
    return access(appMetadataImagePath.c_str(), F_OK) == 0;
}

void ImageUnpacker::deleteImgAppMetadata()
{
    if (imageContainsMetadata()) {
        LOG_DEBUG("Deleting app metadata file from unpacked image");
        ::unlink(appMetadataImagePath.c_str());
    }
}

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

// ─────────────────────────────────────────────────────────────────────────────
#ifdef USE_SKOPEO_UMOCI
// ── Legacy umoci-based implementation ─────────────────────────────────────────

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

#else  // USE_SKOPEO_UMOCI
// ── Native libarchive-based OCI unpacker ──────────────────────────────────────

#include <archive.h>
#include <archive_entry.h>

namespace {

// Strip leading "./" and "/" from a tar entry pathname
static std::string normalizeEntryPath(const char* rawPath)
{
    std::string p = rawPath ? rawPath : "";
    for (;;) {
        if (p.size() >= 2 && p[0] == '.' && p[1] == '/')
            p = p.substr(2);
        else if (!p.empty() && p[0] == '/')
            p = p.substr(1);
        else
            break;
    }
    return p;
}

// Extract hash portion from a "algo:hash" OCI digest string
static std::string digestToHash(const std::string& digest)
{
    auto pos = digest.find(':');
    if (pos == std::string::npos)
        return digest;
    return digest.substr(pos + 1);
}

// Extract a single OCI layer tarball into rootfsPath using libarchive.
// Handles OCI whiteout files (.wh.<name> and .wh..wh..opq).
static bool extractLayer(const std::string& layerPath, const std::string& rootfsPath)
{
    namespace fs = std::filesystem;

    struct archive* a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, layerPath.c_str(), 65536) != ARCHIVE_OK) {
        LOG_ERROR("Failed to open layer archive %s: %s",
                  layerPath.c_str(), archive_error_string(a));
        archive_read_free(a);
        return false;
    }

    struct archive* ext = archive_write_disk_new();
    archive_write_disk_set_options(ext,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_ACL  | ARCHIVE_EXTRACT_FFLAGS |
        ARCHIVE_EXTRACT_XATTR);
    archive_write_disk_set_standard_lookup(ext);

    bool ok = true;
    struct archive_entry* entry;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        std::string normalized = normalizeEntryPath(archive_entry_pathname(entry));

        // Skip the root "." entry
        if (normalized.empty() || normalized == ".") {
            archive_read_data_skip(a);
            continue;
        }

        // Check for OCI whiteout files
        fs::path entryFsPath(normalized);
        std::string fname = entryFsPath.filename().string();

        if (fname.size() > 4 && fname.substr(0, 4) == ".wh.") {
            std::error_code ec;
            fs::path parentInRootfs = fs::path(rootfsPath) / entryFsPath.parent_path();

            if (fname == ".wh..wh..opq") {
                // Opaque whiteout: clear all contents of the parent directory
                if (fs::exists(parentInRootfs, ec)) {
                    for (auto& p : fs::directory_iterator(parentInRootfs, ec))
                        fs::remove_all(p.path(), ec);
                }
            } else {
                // Regular whiteout: delete the named entry
                fs::path target = parentInRootfs / fname.substr(4);
                fs::remove_all(target, ec);
            }
            archive_read_data_skip(a);
            continue;
        }

        // Write the entry to disk under rootfsPath
        std::string destPath = rootfsPath + "/" + normalized;
        archive_entry_set_pathname(entry, destPath.c_str());

        // Update hardlink target paths to be under rootfsPath as well
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink) {
            std::string normalizedHl = normalizeEntryPath(hardlink);
            std::string hlDest = rootfsPath + "/" + normalizedHl;
            archive_entry_set_hardlink(entry, hlDest.c_str());
        }

        int r = archive_write_header(ext, entry);
        if (r != ARCHIVE_OK) {
            LOG_WARNING("archive_write_header failed for %s: %s",
                        destPath.c_str(), archive_error_string(ext));
            archive_read_data_skip(a);
            continue;
        }

        // Copy data blocks
        const void* buff;
        size_t      size;
        la_int64_t  offset;
        while ((r = archive_read_data_block(a, &buff, &size, &offset)) == ARCHIVE_OK) {
            if (archive_write_data_block(ext, buff, size, offset) != ARCHIVE_OK) {
                LOG_WARNING("archive_write_data_block failed: %s",
                            archive_error_string(ext));
            }
        }
        if (r != ARCHIVE_EOF) {
            LOG_WARNING("archive_read_data_block error for %s: %s",
                        normalized.c_str(), archive_error_string(a));
        }

        archive_write_finish_entry(ext);
    }

    archive_read_close(a);
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);

    return ok;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
ImageUnpacker::ImageUnpacker(const std::string& src, const std::string& dst)
    : src(src), dst(dst)
{
    appMetadataImagePath = dst + "/rootfs/appmetadata.json";
}

// ─────────────────────────────────────────────────────────────────────────────
bool ImageUnpacker::unpackImage(const std::string& tag, bool deleteAfter)
{
    namespace fs = std::filesystem;

    // ── Step 1: read index.json ───────────────────────────────────────────────
    std::string indexPath = src + "/index.json";
    std::ifstream indexFile(indexPath);
    if (!indexFile) {
        LOG_ERROR("Failed to open OCI index.json: %s", indexPath.c_str());
        return false;
    }

    nlohmann::json indexJson;
    try {
        indexJson = nlohmann::json::parse(indexFile);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse index.json: %s", e.what());
        return false;
    }

    if (!indexJson.contains("manifests") || indexJson["manifests"].empty()) {
        LOG_ERROR("No manifests found in index.json");
        return false;
    }

    // Find manifest matching the requested tag (by annotation), fall back to first
    std::string manifestDigest;
    for (const auto& m : indexJson["manifests"]) {
        if (m.contains("digest") &&
            m.contains("annotations") &&
            m["annotations"].contains("org.opencontainers.image.ref.name") &&
            m["annotations"]["org.opencontainers.image.ref.name"].get<std::string>() == tag)
        {
            manifestDigest = m["digest"].get<std::string>();
            break;
        }
    }
    if (manifestDigest.empty()) {
        // Fallback: use first manifest regardless of tag
        const auto& first = indexJson["manifests"][0];
        if (!first.contains("digest")) {
            LOG_ERROR("First manifest in index.json has no digest field");
            return false;
        }
        manifestDigest = first["digest"].get<std::string>();
    }

    // ── Step 2: read the manifest blob ───────────────────────────────────────
    std::string manifestHash = digestToHash(manifestDigest);
    std::string manifestPath = src + "/blobs/sha256/" + manifestHash;

    std::ifstream manifestFile(manifestPath);
    if (!manifestFile) {
        LOG_ERROR("Failed to open manifest blob: %s", manifestPath.c_str());
        return false;
    }

    nlohmann::json manifestJson;
    try {
        manifestJson = nlohmann::json::parse(manifestFile);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse manifest blob: %s", e.what());
        return false;
    }

    // ── Step 3: read OCI image config blob and translate to OCI Runtime Spec ────
    if (!manifestJson.contains("config") || !manifestJson["config"].contains("digest")) {
        LOG_ERROR("Manifest does not contain a config digest");
        return false;
    }
    std::string configDigest = manifestJson["config"]["digest"].get<std::string>();
    std::string configHash   = digestToHash(configDigest);
    std::string configBlob   = src + "/blobs/sha256/" + configHash;

    std::ifstream configBlobFile(configBlob);
    if (!configBlobFile) {
        LOG_ERROR("Failed to open OCI image config blob: %s", configBlob.c_str());
        return false;
    }

    nlohmann::json ociImageConfig;
    try {
        ociImageConfig = nlohmann::json::parse(configBlobFile);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse OCI image config blob: %s", e.what());
        return false;
    }

    // Translate OCI image config fields into OCI Runtime Spec process section.
    // The "config" sub-object in the image config blob holds Docker/OCI
    // image-level fields (Entrypoint, Cmd, WorkingDir, Env, Tty, User).
    const nlohmann::json& imgCfg = ociImageConfig.contains("config")
                                       ? ociImageConfig["config"]
                                       : nlohmann::json::object();

    // process.cwd — from WorkingDir, default "/"
    std::string cwd = "/";
    if (imgCfg.contains("WorkingDir") && imgCfg["WorkingDir"].is_string()) {
        std::string wd = imgCfg["WorkingDir"].get<std::string>();
        if (!wd.empty())
            cwd = wd;
    }

    // process.args — Entrypoint + Cmd concatenated; default ["/bin/sh"]
    nlohmann::json args = nlohmann::json::array();
    if (imgCfg.contains("Entrypoint") && imgCfg["Entrypoint"].is_array()) {
        for (const auto& a : imgCfg["Entrypoint"])
            args.push_back(a);
    }
    if (imgCfg.contains("Cmd") && imgCfg["Cmd"].is_array()) {
        for (const auto& a : imgCfg["Cmd"])
            args.push_back(a);
    }
    if (args.empty())
        args.push_back("/bin/sh");

    // process.env — from Env array (strings like "KEY=value")
    nlohmann::json env = nlohmann::json::array();
    if (imgCfg.contains("Env") && imgCfg["Env"].is_array()) {
        for (const auto& e : imgCfg["Env"])
            env.push_back(e);
    }

    // process.terminal — from Tty, default false
    bool terminal = false;
    if (imgCfg.contains("Tty") && imgCfg["Tty"].is_boolean())
        terminal = imgCfg["Tty"].get<bool>();

    // process.user — parse "uid:gid" or "username" from User field
    int uid = 0, gid = 0;
    if (imgCfg.contains("User") && imgCfg["User"].is_string()) {
        std::string userStr = imgCfg["User"].get<std::string>();
        auto colon = userStr.find(':');
        if (colon != std::string::npos) {
            try { uid = std::stoi(userStr.substr(0, colon)); }
            catch (...) { LOG_WARNING("Non-numeric uid in User field '%s', defaulting to 0", userStr.c_str()); }
            try { gid = std::stoi(userStr.substr(colon + 1)); }
            catch (...) { LOG_WARNING("Non-numeric gid in User field '%s', defaulting to 0", userStr.c_str()); }
        } else if (!userStr.empty()) {
            try { uid = std::stoi(userStr); }
            catch (...) { LOG_WARNING("Non-numeric User field '%s', defaulting uid to 0", userStr.c_str()); }
        }
    }

    // Build the OCI Runtime Spec skeleton
    nlohmann::json runtimeSpec = {
        {"ociVersion", "1.0.2"},
        {"process", {
            {"terminal", terminal},
            {"cwd", cwd},
            {"args", args},
            {"env", env},
            {"user", {{"uid", uid}, {"gid", gid}}}
        }},
        {"root", {
            {"path", "rootfs"},
            {"readonly", false}
        }},
        {"mounts", nlohmann::json::array()},
        {"linux", nlohmann::json::object()}
    };

    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) {
        LOG_ERROR("Failed to create destination directory %s: %s",
                  dst.c_str(), ec.message().c_str());
        return false;
    }

    std::ofstream configOut(dst + "/config.json");
    if (!configOut) {
        LOG_ERROR("Failed to open config.json for writing in %s", dst.c_str());
        return false;
    }
    configOut << runtimeSpec.dump(2);
    configOut.close();
    if (!configOut) {
        LOG_ERROR("Failed to flush and close config.json in %s", dst.c_str());
        return false;
    }

    // ── Step 4: create rootfs and extract layers ──────────────────────────────
    std::string rootfsPath = dst + "/rootfs";
    fs::create_directories(rootfsPath, ec);
    if (ec) {
        LOG_ERROR("Failed to create rootfs directory: %s", ec.message().c_str());
        return false;
    }

    if (!manifestJson.contains("layers")) {
        LOG_ERROR("Manifest does not contain layers");
        return false;
    }

    for (const auto& layer : manifestJson["layers"]) {
        if (!layer.contains("digest")) {
            LOG_ERROR("Layer entry in manifest is missing digest field");
            return false;
        }
        std::string layerDigest = layer["digest"].get<std::string>();
        std::string layerHash   = digestToHash(layerDigest);
        std::string layerBlob   = src + "/blobs/sha256/" + layerHash;

        LOG_DEBUG("Extracting layer %s", layerHash.substr(0, 12).c_str());

        if (!extractLayer(layerBlob, rootfsPath)) {
            LOG_ERROR("Failed to extract layer %s", layerHash.c_str());
            return false;
        }
    }

    LOG_INFO("Unpacked image successfully to %s", dst.c_str());

    if (deleteAfter) {
        LOG_DEBUG("Deleting downloaded image");
        fs::remove_all(src, ec);
        if (ec) {
            LOG_WARNING("Failed to delete downloaded image %s: %s",
                        src.c_str(), ec.message().c_str());
        }
    }

    return true;
}

#endif  // USE_SKOPEO_UMOCI

