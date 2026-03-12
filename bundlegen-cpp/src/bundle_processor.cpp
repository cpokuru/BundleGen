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

#include "bundle_processor.h"
#include "capabilities.h"
#include "utils.h"
#include "logger.h"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <cstring>
#include <cerrno>

namespace fs = std::filesystem;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: replace all occurrences of {key} with value in a string
static std::string fmtId(std::string s, const std::string& id)
{
    std::string token = "{id}";
    size_t pos = 0;
    while ((pos = s.find(token, pos)) != std::string::npos) {
        s.replace(pos, token.size(), id);
        pos += id.size();
    }
    return s;
}

// Helper: SHA256 of a string, returned as lower-case hex
static std::string sha256hex(const std::string& input)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(input.data()),
           input.size(), digest);

    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return oss.str();
}

// ─────────────────────────────────────────────────────────────────────────────
BundleProcessor::BundleProcessor(const nlohmann::json& platformCfg,
                                   const std::string& bundlePath,
                                   const nlohmann::json& appMetadata,
                                   bool noDepWalking,
                                   const std::string& libMatchingMode,
                                   bool createMountPoints,
                                   bool crunOnly)
    : platformCfg(platformCfg)
    , bundlePath(bundlePath)
    , rootfsPath(bundlePath + "/rootfs")
    , appMetadata(appMetadata)
    , createMountPoints(createMountPoints)
    , crunOnly(crunOnly)
    , libMatcher(
        platformCfg,
        bundlePath,
        [this](const std::string& src, const std::string& dst, bool cmp) {
            addBindMount(src, dst, cmp);
        },
        noDepWalking,
        libMatchingMode,
        createMountPoints)
{
    ociConfig = loadConfig();
}

// ─────────────────────────────────────────────────────────────────────────────
nlohmann::json BundleProcessor::loadConfig()
{
    std::string configPath = bundlePath + "/config.json";
    std::ifstream f(configPath);
    if (!f) {
        LOG_ERROR("Failed to open config.json: %s", configPath.c_str());
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(f);
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Failed to parse config.json: %s", e.what());
        return nlohmann::json::object();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool BundleProcessor::checkCompatibility()
{
    if (!compatibilityCheck()) {
        LOG_ERROR("App is not compatible with the selected platform");
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool BundleProcessor::validateAppMetadataConfig()
{
    LOG_DEBUG("validate JSON config files with the app meta data schemas.");

    std::ifstream f("bundlegen/schema/appMetadataSchema.json");
    if (!f) {
        LOG_ERROR("IOError during metadata schema open.");
        return false;
    }

    try {
        nlohmann::json schema = nlohmann::json::parse(f);
        (void)schema;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Error parsing appMetadataSchema.json: %s", e.what());
        return false;
    }

    LOG_SUCCESS("validateWithSchema success!");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool BundleProcessor::beginProcessing()
{
    LOG_INFO("Starting processing of bundle using platform template");

    if (createMountPoints)
        createMountPointsUmoci();

    processOciVersion();
    processProcess();
    processRoot();
    processMounts();
    processResources();
    processGpu();

    if (!crunOnly)
        processDobbyPluginDependencies();

    processUsersAndGroups();
    processCapabilities();
    processHostname();
    processApparmorProfile();
    processSeccomp();

    if (!crunOnly)
        addRdkPlugins();

    processNetwork();
    processStorage();
    processLogging();
    processDynamicDevices();
    processIpc();
    processMinidump();
    processOomCrash();
    processThunder();
    processGpuPlugin();

    processHooks();

    writeConfigJson();
    cleanupUmociLeftovers();

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool BundleProcessor::compatibilityCheck()
{
    LOG_DEBUG("Checking app compatibility");

    // Graphics check
    bool appGraphics = appMetadata.value("graphics", false);
    bool platformGraphics = false;
    if (platformCfg.contains("hardware") && platformCfg["hardware"].contains("graphics"))
        platformGraphics = platformCfg["hardware"]["graphics"].get<bool>();

    if (appGraphics && !platformGraphics) {
        LOG_ERROR("Platform does not support graphics output");
        return false;
    }

    // Features check
    if (platformCfg.contains("rdk") && platformCfg["rdk"].contains("supportedFeatures")) {
        std::set<std::string> supported;
        for (const auto& f : platformCfg["rdk"]["supportedFeatures"])
            supported.insert(f.get<std::string>());

        if (appMetadata.contains("features")) {
            for (const auto& feat : appMetadata["features"]) {
                if (!supported.count(feat.get<std::string>())) {
                    LOG_ERROR("App requires feature '%s' which is not supported by the platform",
                              feat.get<std::string>().c_str());
                    return false;
                }
            }
        }
    }

    // Network check
    if (appMetadata.contains("network")) {
        std::string appNetType = appMetadata["network"].value("type", "");
        if (!appNetType.empty() && platformCfg.contains("network")) {
            bool supported = false;
            for (const auto& opt : platformCfg["network"]["options"]) {
                if (opt.get<std::string>() == appNetType) {
                    supported = true;
                    break;
                }
            }
            if (!supported) {
                LOG_ERROR("App requires %s networking, which is not supported by the platform",
                          appNetType.c_str());
                return false;
            }
        }
    }

    // Storage checks
    auto storageCfg = appMetadata.contains("storage") ? &appMetadata["storage"] : nullptr;

    // Persistent storage
    if (storageCfg && storageCfg->contains("persistent")) {
        const auto& persistentArr = (*storageCfg)["persistent"];
        if (!persistentArr.empty()) {
            if (!platformCfg.contains("storage") || !platformCfg["storage"].contains("persistent")) {
                LOG_ERROR("Cannot create persistent storage - platform does not define options");
                return false;
            }

            auto& platStorage = platformCfg["storage"]["persistent"];
            long long maxSize     = platStorage.contains("maxSize")      ? Utils::parseSize(platStorage["maxSize"].get<std::string>())      : 0;
            long long minSize     = platStorage.contains("minSize")      ? Utils::parseSize(platStorage["minSize"].get<std::string>())      : 0;
            long long maxTotalSize= platStorage.contains("maxTotalSize") ? Utils::parseSize(platStorage["maxTotalSize"].get<std::string>()) : 0;

            long long totalSize = 0;
            for (auto& persistent : appMetadata["storage"]["persistent"]) {
                std::string sizeStr = persistent.value("size", "0");
                long long size = Utils::parseSize(sizeStr);

                if (maxSize > 0 && size > maxSize) {
                    LOG_ERROR("Persistent storage exceeds platform limit (%s > %s)",
                              sizeStr.c_str(), platStorage["maxSize"].get<std::string>().c_str());
                    return false;
                }

                if (minSize > 0 && size < minSize) {
                    LOG_WARNING("Persistent storage less than minimum, adjusting to %s",
                                platStorage["minSize"].get<std::string>().c_str());
                    persistent["size"] = platStorage["minSize"];
                    size = minSize;
                }
                totalSize += size;
            }

            if (maxTotalSize > 0 && totalSize > maxTotalSize) {
                LOG_ERROR("Total persistent storage exceeds platform limit");
                return false;
            }
        }
    }

    // Temp storage
    if (storageCfg && storageCfg->contains("temp")) {
        const auto& tempArr = (*storageCfg)["temp"];
        if (!tempArr.empty() && platformCfg.contains("storage") && platformCfg["storage"].contains("temp")) {
            auto& platTemp = platformCfg["storage"]["temp"];
            long long maxSize     = platTemp.contains("maxSize")      ? Utils::parseSize(platTemp["maxSize"].get<std::string>())      : 0;
            long long minSize     = platTemp.contains("minSize")      ? Utils::parseSize(platTemp["minSize"].get<std::string>())      : 0;
            long long maxTotalSize= platTemp.contains("maxTotalSize") ? Utils::parseSize(platTemp["maxTotalSize"].get<std::string>()) : 0;

            long long totalSize = 0;
            for (auto& tmp : appMetadata["storage"]["temp"]) {
                std::string sizeStr = tmp.value("size", "0");
                long long size = Utils::parseSize(sizeStr);

                if (maxSize > 0 && size > maxSize) {
                    LOG_ERROR("Temp storage exceeds platform limit");
                    return false;
                }

                if (minSize > 0 && size < minSize) {
                    LOG_WARNING("Temp storage less than minimum, adjusting");
                    tmp["size"] = platTemp["minSize"];
                    size = minSize;
                }
                totalSize += size;
            }

            if (maxTotalSize > 0 && totalSize > maxTotalSize) {
                LOG_ERROR("Total temp storage exceeds platform limit");
                return false;
            }
        }
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool BundleProcessor::shouldGenerateCompliantConfig()
{
    return platformCfg.contains("dobby")
        && platformCfg["dobby"].contains("generateCompliantConfig")
        && platformCfg["dobby"]["generateCompliantConfig"].get<bool>();
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::createMountPointsUmoci()
{
    if (!ociConfig.contains("mounts")) return;
    for (const auto& mount : ociConfig["mounts"]) {
        std::string dest = mount.value("destination", "");
        if (dest != "/etc/resolv.conf")
            createEmptyDirInRootfs(dest);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processOciVersion()
{
    LOG_DEBUG("Setting OCI version");
    if (!shouldGenerateCompliantConfig() && !crunOnly)
        ociConfig["ociVersion"] = "1.0.2-dobby";
    else
        ociConfig["ociVersion"] = "1.0.2";
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processProcess()
{
    LOG_DEBUG("Processing process section");

    std::string appId = appMetadata.value("id", "");

    if (!crunOnly) {
        std::string dobbyInitPath = "/usr/libexec/DobbyInit";
        if (platformCfg.contains("dobby") && platformCfg["dobby"].contains("dobbyInitPath"))
            dobbyInitPath = platformCfg["dobby"]["dobbyInitPath"].get<std::string>();

        // Flatten args (handle embedded spaces / \\@ escapes like Python version)
        nlohmann::json newArgs = nlohmann::json::array();
        if (ociConfig.contains("process") && ociConfig["process"].contains("args")) {
            for (const auto& arg : ociConfig["process"]["args"]) {
                std::string s = arg.get<std::string>();
                // Replace \@ with space
                size_t pos = 0;
                while ((pos = s.find("\\@", pos)) != std::string::npos) {
                    s.replace(pos, 2, " ");
                    pos += 1;
                }
                // If the string contains spaces, split it
                if (s.find(' ') != std::string::npos) {
                    std::istringstream iss(s);
                    std::string token;
                    while (iss >> token)
                        newArgs.push_back(token);
                } else {
                    newArgs.push_back(s);
                }
            }
        }
        // Prepend DobbyInit
        nlohmann::json finalArgs = nlohmann::json::array();
        finalArgs.push_back(dobbyInitPath);
        for (const auto& a : newArgs)
            finalArgs.push_back(a);
        ociConfig["process"]["args"] = finalArgs;

        addBindMount(dobbyInitPath, dobbyInitPath, createMountPoints);
    }

    // Add platform envvars
    if (platformCfg.contains("envvar")) {
        for (const auto& envvar : platformCfg["envvar"])
            ociConfig["process"]["env"].push_back(envvar);
    }

    // Add resource limits
    if (platformCfg.contains("resourceLimits")) {
        for (const auto& limit : platformCfg["resourceLimits"])
            ociConfig["process"]["rlimits"].push_back(limit);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processRoot()
{
    LOG_DEBUG("Processing root section");

    if (!platformCfg.contains("root")) return;
    const auto& root = platformCfg["root"];

    if (root.contains("readonly"))
        ociConfig["root"]["readonly"] = root["readonly"];

    if (root.contains("path")) {
        std::string path = fmtId(root["path"].get<std::string>(), appMetadata.value("id", ""));
        ociConfig["root"]["path"] = path;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processHostname()
{
    LOG_DEBUG("Processing hostname section");

    if (platformCfg.contains("hostname")) {
        std::string hostname = fmtId(platformCfg["hostname"].get<std::string>(), appMetadata.value("id", ""));
        ociConfig["hostname"] = hostname;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::addMount(nlohmann::json& mount)
{
    if (mount.contains("source")) {
        std::string src = fmtId(mount["source"].get<std::string>(), appMetadata.value("id", ""));
        mount["source"] = src;
    }

    if (createMountPoints) {
        bool hasXMkdir = false;
        if (mount.contains("options")) {
            for (const auto& opt : mount["options"]) {
                if (opt.get<std::string>() == "X-mount.mkdir") {
                    hasXMkdir = true;
                    break;
                }
            }
        }
        if (hasXMkdir)
            createEmptyDirInRootfs(mount.value("destination", ""));
        else
            createAndWriteFileInRootfs(mount.value("destination", ""), "", 0644);
    }

    // Skip mounts with X-mount.no option
    if (mount.contains("options")) {
        for (const auto& opt : mount["options"]) {
            if (opt.get<std::string>() == "X-mount.no")
                return;
        }
    }

    ociConfig["mounts"].push_back(mount);
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processMounts()
{
    LOG_DEBUG("Processing mounts");

    if (platformCfg.contains("mounts")) {
        for (auto mount : platformCfg["mounts"])
            addMount(mount);
    }

    if (appMetadata.contains("mounts")) {
        for (auto mount : appMetadata["mounts"])
            addMount(mount);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processResources()
{
    LOG_DEBUG("Processing resources");

    if (!ociConfig.contains("linux") || ociConfig["linux"].is_null())
        ociConfig["linux"] = nlohmann::json::object();

    if (!ociConfig["linux"].contains("resources"))
        ociConfig["linux"]["resources"] = nlohmann::json::object();

    if (!ociConfig["linux"]["resources"].contains("devices"))
        ociConfig["linux"]["resources"]["devices"] = nlohmann::json::array();

    // Add deny-all device cgroup rule at the start
    nlohmann::json denyAll = {{"allow", false}, {"access", "rwm"}};
    bool hasDenyAll = false;
    for (const auto& d : ociConfig["linux"]["resources"]["devices"]) {
        if (d == denyAll) { hasDenyAll = true; break; }
    }
    if (!hasDenyAll) {
        // Insert at front
        auto& devs = ociConfig["linux"]["resources"]["devices"];
        devs.insert(devs.begin(), denyAll);
    }

    // Set memory limit
    auto hwMaxRam = platformCfg.contains("hardware") && platformCfg["hardware"].contains("maxRam")
                  ? platformCfg["hardware"]["maxRam"].get<std::string>() : "";

    if (!hwMaxRam.empty() && appMetadata.contains("resources") && appMetadata["resources"].contains("ram")) {
        std::string appRamStr = appMetadata["resources"]["ram"].get<std::string>();
        long long appRamBytes      = Utils::parseSize(appRamStr);
        long long platformRamBytes = Utils::parseSize(hwMaxRam);

        ociConfig["linux"]["resources"]["memory"] = nlohmann::json::object();

        if (appRamBytes > platformRamBytes) {
            LOG_WARNING("App memory requirements too large for platform (%s > %s). Setting to platform limit",
                        appRamStr.c_str(), hwMaxRam.c_str());
            ociConfig["linux"]["resources"]["memory"]["limit"] = platformRamBytes;
        } else {
            ociConfig["linux"]["resources"]["memory"]["limit"] = appRamBytes;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processGpu()
{
    LOG_DEBUG("Processing GPU");

    bool appGraphics = appMetadata.value("graphics", false);
    if (!appGraphics) return;

    bool platformGraphics = platformCfg.contains("hardware")
                         && platformCfg["hardware"].value("graphics", false);
    if (!platformGraphics) {
        LOG_ERROR("App requires graphics but platform does not support graphics output");
        return;
    }

    if (!platformCfg.contains("gpu")) return;
    const auto& gpu = platformCfg["gpu"];

    // Extra mounts
    if (gpu.contains("extraMounts")) {
        for (auto mount : gpu["extraMounts"])
            addMount(mount);
    }

    // Envvars
    if (gpu.contains("envvar")) {
        for (const auto& envvar : gpu["envvar"])
            ociConfig["process"]["env"].push_back(envvar);
    }

    // GPU libraries
    if (gpu.contains("gfxLibs")) {
        for (const auto& lib : gpu["gfxLibs"])
            libMatcher.mount(lib["src"].get<std::string>(), lib["dst"].get<std::string>());
    }

    // Westeros socket
    if (gpu.contains("westeros")) {
        std::string socket = gpu["westeros"].value("hostSocket", "");
        if (!socket.empty())
            addBindMount(socket, "/tmp/westeros", false,
                         nlohmann::json({"rbind", "nosuid", "nodev"}));
    }

    std::string waylandDisplay = gpu.value("waylandDisplay", "westeros");
    ociConfig["process"]["env"].push_back("WAYLAND_DISPLAY=" + waylandDisplay);

    // GPU devices
    if (!ociConfig["linux"].contains("devices"))
        ociConfig["linux"]["devices"] = nlohmann::json::array();
    if (!ociConfig["linux"]["resources"].contains("devices"))
        ociConfig["linux"]["resources"]["devices"] = nlohmann::json::array();

    if (gpu.contains("devs")) {
        for (const auto& dev : gpu["devs"]) {
            nlohmann::json devCfg = {
                {"path",  dev["path"]},
                {"type",  dev["type"]},
                {"major", dev["major"]},
                {"minor", dev["minor"]}
            };
            ociConfig["linux"]["devices"].push_back(devCfg);

            nlohmann::json devPerms = {
                {"allow",  true},
                {"type",   dev["type"]},
                {"major",  dev["major"]},
                {"minor",  dev["minor"]},
                {"access", dev["access"]}
            };
            ociConfig["linux"]["resources"]["devices"].push_back(devPerms);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
bool BundleProcessor::isMapped(int id, const nlohmann::json& mappings)
{
    if (mappings.is_null() || !mappings.is_array()) return false;
    for (const auto& entry : mappings) {
        int containerID = entry.value("containerID", -1);
        int size        = entry.value("size", 0);
        if (id >= containerID && id < containerID + size)
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::checkUidGidMappings()
{
    auto& process = ociConfig["process"];
    int uid = -1, gid = -1;
    if (process.contains("user")) {
        uid = process["user"].value("uid", -1);
        gid = process["user"].value("gid", -1);
    }

    auto uidMappings = ociConfig["linux"].contains("uidMappings") ? ociConfig["linux"]["uidMappings"] : nlohmann::json(nullptr);
    auto gidMappings = ociConfig["linux"].contains("gidMappings") ? ociConfig["linux"]["gidMappings"] : nlohmann::json(nullptr);

    if (uid >= 0 && !isMapped(uid, uidMappings))
        LOG_WARNING("No mapping found for UID %d", uid);
    if (gid >= 0 && !isMapped(gid, gidMappings))
        LOG_WARNING("No mapping found for GID %d", gid);

    if (process.contains("user") && process["user"].contains("additionalGids")) {
        for (const auto& gidEntry : process["user"]["additionalGids"]) {
            int g = gidEntry.get<int>();
            if (!isMapped(g, gidMappings))
                LOG_WARNING("No mapping found for additional GID %d", g);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processUsersAndGroups()
{
    LOG_DEBUG("Adding user/group mappings");

    if (platformCfg.contains("usersAndGroups")) {
        const auto& uag = platformCfg["usersAndGroups"];
        if (uag.contains("user")) {
            const auto& user = uag["user"];
            if (user.contains("uid"))
                ociConfig["process"]["user"]["uid"] = user["uid"];
            if (user.contains("gid"))
                ociConfig["process"]["user"]["gid"] = user["gid"];
            if (user.contains("additionalGids"))
                ociConfig["process"]["user"]["additionalGids"] = user["additionalGids"];
        }
    }

    if (platformCfg.value("disableUserNamespacing", false)) {
        LOG_DEBUG("User namespacing disabled on this platform");

        // Remove the user namespace from the list
        if (ociConfig["linux"].contains("namespaces")) {
            auto& ns = ociConfig["linux"]["namespaces"];
            ns.erase(std::remove_if(ns.begin(), ns.end(), [](const nlohmann::json& n) {
                return n.value("type", "") == "user";
            }), ns.end());
        }

        ociConfig["linux"].erase("uidMappings");
        ociConfig["linux"].erase("gidMappings");
        checkUidGidMappings();
        return;
    }

    // Platform supports user namespaces
    ociConfig["linux"]["uidMappings"] = nlohmann::json::array();
    ociConfig["linux"]["gidMappings"] = nlohmann::json::array();

    if (!platformCfg.contains("usersAndGroups")) {
        LOG_DEBUG("Platform does not have a user/group ID mapping set - this must be set at runtime");
        return;
    }

    if (platformCfg["usersAndGroups"].contains("uidMap")) {
        for (const auto& m : platformCfg["usersAndGroups"]["uidMap"])
            ociConfig["linux"]["uidMappings"].push_back(m);
    }
    if (platformCfg["usersAndGroups"].contains("gidMap")) {
        for (const auto& m : platformCfg["usersAndGroups"]["gidMap"])
            ociConfig["linux"]["gidMappings"].push_back(m);
    }

    checkUidGidMappings();
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processCapabilities()
{
    LOG_DEBUG("Adding capabilities");

    std::set<std::string> appCapabilities;

    if (!platformCfg["capabilities"].is_null()) {
        for (const auto& cap : platformCfg.value("capabilities", nlohmann::json::array()))
            appCapabilities.insert(cap.get<std::string>());
    } else {
        for (const auto& cap : getDefaultCaps())
            appCapabilities.insert(cap);
    }

    if (appMetadata.contains("capabilities")) {
        const auto& caps = appMetadata["capabilities"];
        if (caps.contains("add")) {
            for (const auto& cap : caps["add"])
                appCapabilities.insert(cap.get<std::string>());
        }
        if (caps.contains("drop")) {
            for (const auto& cap : caps["drop"])
                appCapabilities.erase(cap.get<std::string>());
        }
    }

    nlohmann::json capList = nlohmann::json::array();
    for (const auto& cap : appCapabilities)
        capList.push_back(cap);

    if (ociConfig.contains("process") && ociConfig["process"].contains("capabilities")) {
        auto& cfgCaps = ociConfig["process"]["capabilities"];
        cfgCaps["bounding"]    = capList;
        cfgCaps["permitted"]   = capList;
        cfgCaps["effective"]   = capList;
        cfgCaps["inheritable"] = capList;
        cfgCaps["ambient"]     = capList;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processApparmorProfile()
{
    LOG_DEBUG("_process_apparmor ENTER");

    if (!platformCfg.contains("apparmorProfile")) {
        LOG_INFO("Platform does not have apparmor profile set");
        return;
    }
    ociConfig["process"]["apparmorProfile"] = platformCfg["apparmorProfile"];
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processSeccomp()
{
    LOG_DEBUG("_process_seccomp ENTER");

    if (!platformCfg.contains("seccomp")) {
        LOG_SUCCESS("Platform does not have seccomp set");
        return;
    }
    ociConfig["linux"]["seccomp"] = platformCfg["seccomp"];
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::addRdkPlugins()
{
    ociConfig["rdkPlugins"] = nlohmann::json::object();

    if (platformCfg.contains("dobby") && platformCfg["dobby"].contains("pluginDir")) {
        std::string pluginDir = platformCfg["dobby"]["pluginDir"].get<std::string>();
        addBindMount(pluginDir, pluginDir);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processNetwork()
{
    if (crunOnly) {
        if (appMetadata.contains("network"))
            LOG_WARNING("App metadata 'network' section ignored for crun");
        return;
    }

    LOG_DEBUG("Processing network");

    if (!appMetadata.contains("network")) return;

    ociConfig["rdkPlugins"]["networking"] = {
        {"required", true},
        {"data", appMetadata["network"]}
    };

    // Create /etc/nsswitch.conf
    std::string nsswitch = "hosts:     files mdns4_minimal [NOTFOUND=return] dns mdns4\n"
                           "protocols: files\n";
    createAndWriteFileInRootfs("etc/nsswitch.conf", nsswitch, 0644);

    // Create /etc/hosts
    createAndWriteFileInRootfs("etc/hosts", "127.0.0.1\tlocalhost\n", 0644);

    // Create /etc/resolv.conf if createMountPoints
    if (createMountPoints)
        createAndWriteFileInRootfs("etc/resolv.conf", "", 0644);
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processStorage()
{
    if (crunOnly) {
        if (appMetadata.contains("storage"))
            LOG_WARNING("App metadata 'storage' section ignored for crun");
        return;
    }

    LOG_DEBUG("Processing storage");

    if (!appMetadata.contains("storage")) return;
    const auto& storageCfg = appMetadata["storage"];

    // Persistent storage → storage plugin with loopback
    if (storageCfg.contains("persistent") && !storageCfg["persistent"].empty()) {
        nlohmann::json loopbackPlugin = {
            {"required", true},
            {"data", {{"loopback", nlohmann::json::array()}}}
        };

        auto& platPersist = platformCfg["storage"]["persistent"];
        std::string fstype = platPersist.value("fstype", "ext4");
        std::string storageDir = platPersist.value("storageDir", "/var/lib/dobby/storage");

        for (const auto& persistent : storageCfg["persistent"]) {
            std::string sizeStr  = persistent.value("size", "0");
            std::string destPath = persistent.value("path", "");
            long long sizeBytes  = Utils::parseSize(sizeStr);

            // SHA256 of dest path → img file name
            std::string imgName  = sha256hex(destPath);
            std::string appId    = appMetadata.value("id", "app");
            std::string sourcePath = storageDir + "/" + appId + "/" + imgName + ".img";

            nlohmann::json loopbackDef = {
                {"destination", destPath},
                {"flags",       14},
                {"fstype",      fstype},
                {"source",      sourcePath},
                {"imgsize",     sizeBytes}
            };

            loopbackPlugin["data"]["loopback"].push_back(loopbackDef);
            createEmptyDirInRootfs(destPath);
            if (createMountPoints)
                createEmptyDirInRootfs(destPath + ".temp");
        }

        ociConfig["rdkPlugins"]["storage"] = loopbackPlugin;
    }

    // Temp storage → tmpfs mounts
    if (storageCfg.contains("temp") && !storageCfg["temp"].empty()) {
        int uid = -1, gid = -1;
        if (ociConfig.contains("process") && ociConfig["process"].contains("user")) {
            uid = ociConfig["process"]["user"].value("uid", -1);
            gid = ociConfig["process"]["user"].value("gid", -1);
        }

        for (const auto& tmpMnt : storageCfg["temp"]) {
            long long size = Utils::parseSize(tmpMnt.value("size", "0"));
            nlohmann::json options = {"nosuid", "strictatime", "mode=755",
                                       "size=" + std::to_string(size)};
            if (uid >= 0) options.push_back("uid=" + std::to_string(uid));
            if (gid >= 0) options.push_back("gid=" + std::to_string(gid));

            nlohmann::json mntToAdd = {
                {"destination", tmpMnt["path"]},
                {"type",        "tmpfs"},
                {"source",      "tmpfs"},
                {"options",     options}
            };
            ociConfig["mounts"].push_back(mntToAdd);
            createEmptyDirInRootfs(tmpMnt.value("path", ""));
        }
    }

    // Optional mounts → move to storage plugin dynamic section
    nlohmann::json optionalMounts = nlohmann::json::array();
    if (ociConfig.contains("mounts")) {
        auto& mounts = ociConfig["mounts"];
        for (auto it = mounts.begin(); it != mounts.end(); ) {
            bool isOptional = false;
            if (it->contains("options")) {
                for (const auto& opt : (*it)["options"]) {
                    if (opt.get<std::string>() == "X-dobby.optional") {
                        isOptional = true;
                        break;
                    }
                }
            }
            if (isOptional) {
                nlohmann::json m = *it;
                // Remove the optional flag from options
                auto& opts = m["options"];
                opts.erase(std::remove_if(opts.begin(), opts.end(), [](const nlohmann::json& o) {
                    return o.get<std::string>() == "X-dobby.optional";
                }), opts.end());
                optionalMounts.push_back(m);
                it = mounts.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (!optionalMounts.empty()) {
        if (!ociConfig["rdkPlugins"].contains("storage")) {
            ociConfig["rdkPlugins"]["storage"] = {{"required", true}, {"data", nlohmann::json::object()}};
        }
        ociConfig["rdkPlugins"]["storage"]["data"]["dynamic"] = optionalMounts;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::addAnnotation(const std::string& key, const std::string& value)
{
    if (!ociConfig.contains("annotations"))
        ociConfig["annotations"] = nlohmann::json::object();
    ociConfig["annotations"][key] = value;
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processLogging()
{
    if (crunOnly) {
        if (platformCfg.contains("logging"))
            LOG_WARNING("Platform 'logging' section ignored for crun");
        return;
    }

    if (!platformCfg.contains("logging")) {
        LOG_INFO("Platform does not contain logging options - container will not produce any logs");
        return;
    }

    LOG_DEBUG("Configuring logging");
    ociConfig["process"]["terminal"] = true;

    const auto& loggingCfg = platformCfg["logging"];
    std::string mode = loggingCfg.value("mode", "");
    std::string appId = appMetadata.value("id", "app");

    nlohmann::json loggingPlugin;

    if (mode == "file") {
        std::string logDir  = loggingCfg.value("logDir", "/var/log");
        std::string logFile = logDir + "/" + appId + ".log";

        loggingPlugin = {
            {"required", true},
            {"data", {
                {"sink", "file"},
                {"fileOptions", {{"path", logFile}}}
            }}
        };
        addAnnotation("run.oci.hooks.stderr", "/dev/stderr");
        addAnnotation("run.oci.hooks.stdout", "/dev/stdout");

        if (loggingCfg.contains("limit"))
            loggingPlugin["data"]["fileOptions"]["limit"] = loggingCfg["limit"];

    } else if (mode == "journald") {
        loggingPlugin = {
            {"required", true},
            {"data", {{"sink", "journald"}}}
        };
        if (loggingCfg.contains("journaldOptions"))
            loggingPlugin["data"]["journaldOptions"] = loggingCfg["journaldOptions"];

    } else if (mode == "devnull") {
        loggingPlugin = {
            {"required", true},
            {"data", {{"sink", "devnull"}}}
        };
    }

    if (!loggingPlugin.is_null())
        ociConfig["rdkPlugins"]["logging"] = loggingPlugin;
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processDynamicDevices()
{
    bool platformGraphics = platformCfg.contains("hardware")
                         && platformCfg["hardware"].value("graphics", false);
    if (!platformGraphics) return;

    LOG_DEBUG("Configuring devicemapper");

    if (!platformCfg.contains("gpu") || !platformCfg["gpu"].contains("devs")) return;

    nlohmann::json dynamicDevices = nlohmann::json::array();
    for (const auto& dev : platformCfg["gpu"]["devs"]) {
        if (dev.value("dynamic", false))
            dynamicDevices.push_back(dev["path"]);
    }

    if (dynamicDevices.empty()) return;

    if (crunOnly) {
        LOG_WARNING("Platform gpu 'dynamic' section ignored for crun");
        return;
    }

    ociConfig["rdkPlugins"]["devicemapper"] = {
        {"required", true},
        {"data", {{"devices", dynamicDevices}}}
    };
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processDobbyPluginDependencies()
{
    if (platformCfg.contains("dobby") && platformCfg["dobby"].contains("pluginDependencies")) {
        LOG_DEBUG("Adding library mounts for Dobby plugins");
        LOG_DEBUG("rootfs path is %s", rootfsPath.c_str());
        for (const auto& lib : platformCfg["dobby"]["pluginDependencies"]) {
            std::string libStr = lib.get<std::string>();
            libMatcher.mountOrUseRootfs(libStr, libStr);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processIpc()
{
    if (appMetadata.contains("ipc") && appMetadata["ipc"].value("enable", false)) {
        if (platformCfg.contains("ipc")) {
            ociConfig["rdkPlugins"]["ipc"] = {
                {"required", true},
                {"data", platformCfg["ipc"]}
            };
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processMinidump()
{
    if (appMetadata.contains("minidump") && appMetadata["minidump"].value("enable", false)) {
        if (platformCfg.contains("minidump")) {
            ociConfig["rdkPlugins"]["minidump"] = {
                {"required", true},
                {"data", platformCfg["minidump"]}
            };
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processOomCrash()
{
    if (appMetadata.contains("oomcrash") && appMetadata["oomcrash"].value("enable", false)) {
        if (platformCfg.contains("oomcrash")) {
            ociConfig["rdkPlugins"]["oomcrash"] = {
                {"required", true},
                {"data", platformCfg["oomcrash"]}
            };
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processThunder()
{
    if (appMetadata.contains("thunder")) {
        ociConfig["rdkPlugins"]["thunder"] = {
            {"required",  true},
            {"dependsOn", {"networking"}},
            {"data",      appMetadata["thunder"]}
        };
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processGpuPlugin()
{
    if (appMetadata.contains("resources") && appMetadata["resources"].contains("gpu")) {
        std::string gpuStr = appMetadata["resources"]["gpu"].get<std::string>();
        long long gpuBytes = Utils::parseSize(gpuStr);
        ociConfig["rdkPlugins"]["gpu"] = {
            {"required", true},
            {"data", {{"memory", gpuBytes}}}
        };
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::processHooks()
{
    if (crunOnly) return;
    if (!shouldGenerateCompliantConfig()) return;

    if (!ociConfig.contains("rdkPlugins") || ociConfig["rdkPlugins"].empty()) return;

    LOG_DEBUG("Generating hooks");

    std::string hookLauncherExec = "/usr/bin/DobbyPluginLauncher";
    if (platformCfg.contains("dobby") && platformCfg["dobby"].contains("hookLauncherExecutablePath"))
        hookLauncherExec = platformCfg["dobby"]["hookLauncherExecutablePath"].get<std::string>();

    if (!platformCfg.contains("dobby") || !platformCfg["dobby"].contains("hookLauncherParametersPath")) {
        LOG_ERROR("Config dobby.hookLauncherParametersPath is required when dobby.generateCompliantConfig is true");
        return;
    }

    std::string hookParams = fmtId(
        platformCfg["dobby"]["hookLauncherParametersPath"].get<std::string>(),
        appMetadata.value("id", ""));

    // Ensure it ends with /config.json
    auto bn = hookParams;
    if (bn.find('/') != std::string::npos)
        bn = bn.substr(bn.rfind('/') + 1);
    if (bn != "config.json")
        hookParams += "/config.json";

    auto makeHook = [&](const std::string& hookName) {
        return nlohmann::json::array({
            {
                {"path", hookLauncherExec},
                {"args", {
                    "DobbyPluginLauncher",
                    "-h", hookName,
                    "-c", hookParams
                }}
            }
        });
    };

    ociConfig["hooks"] = {
        {"createRuntime",  makeHook("createRuntime")},
        {"createContainer",makeHook("createContainer")},
        {"poststart",      makeHook("poststart")},
        {"poststop",       makeHook("poststop")}
    };
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::writeConfigJson()
{
    std::string configPath = bundlePath + "/config.json";
    LOG_DEBUG("Saving modified OCI config to %s", configPath.c_str());

    std::ofstream f(configPath);
    if (!f) {
        LOG_ERROR("Failed to open config.json for writing: %s", configPath.c_str());
        return;
    }
    f << ociConfig.dump(4);
    LOG_DEBUG("Written config.json successfully");
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::cleanupUmociLeftovers()
{
    LOG_DEBUG("Cleaning up umoci leftovers");

    std::string umociJson = bundlePath + "/umoci.json";
    ::unlink(umociJson.c_str());

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(bundlePath, ec)) {
        if (ec) break;
        std::string filename = entry.path().filename().string();
        if (filename.find("sha256_") == 0 && filename.size() > 7
            && filename.substr(filename.size()-6) == ".mtree") {
            LOG_DEBUG("Deleting %s", entry.path().string().c_str());
            ::unlink(entry.path().string().c_str());
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::addBindMount(const std::string& src, const std::string& dst,
                                    bool createMountPoint,
                                    const nlohmann::json& options)
{
    LOG_DEBUG("Adding bind mount [src: %s, dst: %s]", src.c_str(), dst.c_str());

    nlohmann::json mntToAdd;
    if (options.is_null() || !options.is_array()) {
        mntToAdd = {
            {"source",      src},
            {"destination", dst},
            {"type",        "bind"},
            {"options",     {"rbind", "nosuid", "nodev", "ro"}}
        };
    } else {
        mntToAdd = {
            {"source",      src},
            {"destination", dst},
            {"type",        "bind"},
            {"options",     options}
        };
    }

    if (createMountPoint)
        createAndWriteFileInRootfs(dst, "", 0644);

    if (!ociConfig.contains("mounts"))
        ociConfig["mounts"] = nlohmann::json::array();

    // Only add if not already present
    bool found = false;
    for (const auto& m : ociConfig["mounts"]) {
        if (m == mntToAdd) { found = true; break; }
    }
    if (!found)
        ociConfig["mounts"].push_back(mntToAdd);
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::createAndWriteFileInRootfs(const std::string& path,
                                                   const std::string& contents,
                                                   int mode)
{
    std::string stripped = path;
    while (!stripped.empty() && stripped.front() == '/') stripped.erase(stripped.begin());

    std::string fullPath = rootfsPath + "/" + stripped;
    std::string directory = fullPath.substr(0, fullPath.rfind('/'));

    // Create parent directories
    if (!directory.empty()) {
        std::error_code ec;
        fs::create_directories(directory, ec);
    }

    std::ofstream f(fullPath, std::ios::trunc);
    if (!f) {
        LOG_DEBUG("createAndWriteFileInRootfs: could not create %s", fullPath.c_str());
        return;
    }
    f << contents;
    f.close();
    ::chmod(fullPath.c_str(), (mode_t)mode);
}

// ─────────────────────────────────────────────────────────────────────────────
void BundleProcessor::createEmptyDirInRootfs(const std::string& path)
{
    std::string stripped = path;
    while (!stripped.empty() && stripped.front() == '/') stripped.erase(stripped.begin());

    std::string fullPath = rootfsPath + "/" + stripped;
    LOG_DEBUG("Creating directory %s", fullPath.c_str());

    std::error_code ec;
    fs::create_directories(fullPath, ec);
}

// ─────────────────────────────────────────────────────────────────────────────
std::pair<int,int> BundleProcessor::getRealUidGid()
{
    int uid = -1, gid = -1;
    if (ociConfig.contains("process") && ociConfig["process"].contains("user")) {
        uid = ociConfig["process"]["user"].value("uid", -1);
        gid = ociConfig["process"]["user"].value("gid", -1);
    }

    if (platformCfg.value("disableUserNamespacing", false))
        return {uid, gid};

    // User namespacing enabled → resolve mappings
    if (!platformCfg.contains("usersAndGroups")) {
        LOG_WARNING("User namespacing enabled but usersAndGroups not defined");
        return {uid, gid};
    }

    const auto& uag = platformCfg["usersAndGroups"];
    int realUid = -1, realGid = -1;

    if (uag.contains("uidMap")) {
        for (const auto& m : uag["uidMap"]) {
            if (m.value("containerID", -1) == uid) {
                realUid = m.value("hostID", -1);
                break;
            }
        }
    }
    if (uag.contains("gidMap")) {
        for (const auto& m : uag["gidMap"]) {
            if (m.value("containerID", -1) == gid) {
                realGid = m.value("hostID", -1);
                break;
            }
        }
    }

    if (realUid >= 0 && realGid >= 0) {
        LOG_DEBUG("User namespacing enabled - resolved host uid/gid to %d:%d", realUid, realGid);
        return {realUid, realGid};
    }

    LOG_WARNING("User namespacing enabled but could not resolve host uid/gid");
    return {uid, gid};
}
