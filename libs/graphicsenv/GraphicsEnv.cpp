/*
 * Copyright 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 1
#define LOG_TAG "GraphicsEnv"
#include <graphicsenv/GraphicsEnv.h>

#include <dlfcn.h>
#include <unistd.h>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android/dlext.h>
#include <cutils/properties.h>
#include <log/log.h>
#include <sys/prctl.h>

#include <memory>
#include <mutex>
#include <string>

#include <dlfcn.h>

// TODO(b/37049319) Get this from a header once one exists
extern "C" {
android_namespace_t* android_get_exported_namespace(const char*);
android_namespace_t* android_create_namespace(const char* name, const char* ld_library_path,
                                              const char* default_library_path, uint64_t type,
                                              const char* permitted_when_isolated_path,
                                              android_namespace_t* parent);
bool android_link_namespaces(android_namespace_t* from, android_namespace_t* to,
                             const char* shared_libs_sonames);

enum {
    ANDROID_NAMESPACE_TYPE_ISOLATED = 1,
    ANDROID_NAMESPACE_TYPE_SHARED = 2,
};
}

// TODO(ianelliott@): Get the following from an ANGLE header:
#define CURRENT_ANGLE_API_VERSION 2 // Current API verion we are targetting
// Version-2 API:
typedef bool (*fpANGLEGetFeatureSupportUtilAPIVersion)(unsigned int* versionToUse);
typedef bool (*fpANGLEAndroidParseRulesString)(const char* rulesString, void** rulesHandle,
                                               int* rulesVersion);
typedef bool (*fpANGLEGetSystemInfo)(void** handle);
typedef bool (*fpANGLEAddDeviceInfoToSystemInfo)(const char* deviceMfr, const char* deviceModel,
                                                 void* handle);
typedef bool (*fpANGLEShouldBeUsedForApplication)(void* rulesHandle, int rulesVersion,
                                                  void* systemInfoHandle, const char* appName);
typedef bool (*fpANGLEFreeRulesHandle)(void* handle);
typedef bool (*fpANGLEFreeSystemInfoHandle)(void* handle);

namespace android {

enum NativeLibrary {
    LLNDK = 0,
    VNDKSP = 1,
};

static constexpr const char* kNativeLibrariesSystemConfigPath[] = {"/etc/llndk.libraries.txt",
                                                                   "/etc/vndksp.libraries.txt"};

static std::string vndkVersionStr() {
#ifdef __BIONIC__
    std::string version = android::base::GetProperty("ro.vndk.version", "");
    if (version != "" && version != "current") {
        return "." + version;
    }
#endif
    return "";
}

static void insertVndkVersionStr(std::string* fileName) {
    LOG_ALWAYS_FATAL_IF(!fileName, "fileName should never be nullptr");
    size_t insertPos = fileName->find_last_of(".");
    if (insertPos == std::string::npos) {
        insertPos = fileName->length();
    }
    fileName->insert(insertPos, vndkVersionStr());
}

static bool readConfig(const std::string& configFile, std::vector<std::string>* soNames) {
    // Read list of public native libraries from the config file.
    std::string fileContent;
    if (!base::ReadFileToString(configFile, &fileContent)) {
        return false;
    }

    std::vector<std::string> lines = base::Split(fileContent, "\n");

    for (auto& line : lines) {
        auto trimmedLine = base::Trim(line);
        if (!trimmedLine.empty()) {
            soNames->push_back(trimmedLine);
        }
    }

    return true;
}

static const std::string getSystemNativeLibraries(NativeLibrary type) {
    static const char* androidRootEnv = getenv("ANDROID_ROOT");
    static const std::string rootDir = androidRootEnv != nullptr ? androidRootEnv : "/system";

    std::string nativeLibrariesSystemConfig = rootDir + kNativeLibrariesSystemConfigPath[type];

    insertVndkVersionStr(&nativeLibrariesSystemConfig);

    std::vector<std::string> soNames;
    if (!readConfig(nativeLibrariesSystemConfig, &soNames)) {
        ALOGE("Failed to retrieve library names from %s", nativeLibrariesSystemConfig.c_str());
        return "";
    }

    return base::Join(soNames, ':');
}

/*static*/ GraphicsEnv& GraphicsEnv::getInstance() {
    static GraphicsEnv env;
    return env;
}

int GraphicsEnv::getCanLoadSystemLibraries() {
    if (property_get_bool("ro.debuggable", false) && prctl(PR_GET_DUMPABLE, 0, 0, 0, 0)) {
        // Return an integer value since this crosses library boundaries
        return 1;
    }
    return 0;
}

void GraphicsEnv::setDriverPath(const std::string path) {
    if (!mDriverPath.empty()) {
        ALOGV("ignoring attempt to change driver path from '%s' to '%s'", mDriverPath.c_str(),
              path.c_str());
        return;
    }
    ALOGV("setting driver path to '%s'", path.c_str());
    mDriverPath = path;
}

void* GraphicsEnv::loadLibrary(std::string name) {
    const android_dlextinfo dlextinfo = {
            .flags = ANDROID_DLEXT_USE_NAMESPACE,
            .library_namespace = getAngleNamespace(),
    };

    std::string libName = std::string("lib") + name + "_angle.so";

    void* so = android_dlopen_ext(libName.c_str(), RTLD_LOCAL | RTLD_NOW, &dlextinfo);

    if (so) {
        ALOGD("dlopen_ext from APK (%s) success at %p", libName.c_str(), so);
        return so;
    } else {
        ALOGE("dlopen_ext(\"%s\") failed: %s", libName.c_str(), dlerror());
    }

    return nullptr;
}

bool GraphicsEnv::checkAngleRules(void* so) {
    char manufacturer[PROPERTY_VALUE_MAX];
    char model[PROPERTY_VALUE_MAX];
    property_get("ro.product.manufacturer", manufacturer, "UNSET");
    property_get("ro.product.model", model, "UNSET");

    auto ANGLEGetFeatureSupportUtilAPIVersion =
            (fpANGLEGetFeatureSupportUtilAPIVersion)dlsym(so,
                                                          "ANGLEGetFeatureSupportUtilAPIVersion");

    if (!ANGLEGetFeatureSupportUtilAPIVersion) {
        ALOGW("Cannot find ANGLEGetFeatureSupportUtilAPIVersion function");
        return false;
    }

    // Negotiate the interface version by requesting most recent known to the platform
    unsigned int versionToUse = CURRENT_ANGLE_API_VERSION;
    if (!(ANGLEGetFeatureSupportUtilAPIVersion)(&versionToUse)) {
        ALOGW("Cannot use ANGLE feature-support library, it is older than supported by EGL, "
              "requested version %u",
              versionToUse);
        return false;
    }

    // Add and remove versions below as needed
    bool useAngle = false;
    switch (versionToUse) {
        case 2: {
            ALOGV("Using version %d of ANGLE feature-support library", versionToUse);
            void* rulesHandle = nullptr;
            int rulesVersion = 0;
            void* systemInfoHandle = nullptr;

            // Get the symbols for the feature-support-utility library:
#define GET_SYMBOL(symbol)                                                 \
    fp##symbol symbol = (fp##symbol)dlsym(so, #symbol);                    \
    if (!symbol) {                                                         \
        ALOGW("Cannot find " #symbol " in ANGLE feature-support library"); \
        break;                                                             \
    }
            GET_SYMBOL(ANGLEAndroidParseRulesString);
            GET_SYMBOL(ANGLEGetSystemInfo);
            GET_SYMBOL(ANGLEAddDeviceInfoToSystemInfo);
            GET_SYMBOL(ANGLEShouldBeUsedForApplication);
            GET_SYMBOL(ANGLEFreeRulesHandle);
            GET_SYMBOL(ANGLEFreeSystemInfoHandle);

            // Parse the rules, obtain the SystemInfo, and evaluate the
            // application against the rules:
            if (!(ANGLEAndroidParseRulesString)(mRulesBuffer.data(), &rulesHandle, &rulesVersion)) {
                ALOGW("ANGLE feature-support library cannot parse rules file");
                break;
            }
            if (!(ANGLEGetSystemInfo)(&systemInfoHandle)) {
                ALOGW("ANGLE feature-support library cannot obtain SystemInfo");
                break;
            }
            if (!(ANGLEAddDeviceInfoToSystemInfo)(manufacturer, model, systemInfoHandle)) {
                ALOGW("ANGLE feature-support library cannot add device info to SystemInfo");
                break;
            }
            useAngle = (ANGLEShouldBeUsedForApplication)(rulesHandle, rulesVersion,
                                                         systemInfoHandle, mAngleAppName.c_str());
            (ANGLEFreeRulesHandle)(rulesHandle);
            (ANGLEFreeSystemInfoHandle)(systemInfoHandle);
        } break;

        default:
            ALOGW("Version %u of ANGLE feature-support library is NOT supported.", versionToUse);
    }

    ALOGV("Close temporarily-loaded ANGLE opt-in/out logic");
    return useAngle;
}

bool GraphicsEnv::shouldUseAngle(std::string appName) {
    if (appName != mAngleAppName) {
        // Make sure we are checking the app we were init'ed for
        ALOGE("App name does not match: expected '%s', got '%s'", mAngleAppName.c_str(),
              appName.c_str());
        return false;
    }

    return shouldUseAngle();
}

bool GraphicsEnv::shouldUseAngle() {
    // Make sure we are init'ed
    if (mAngleAppName.empty()) {
        ALOGE("App name is empty. setAngleInfo() must be called first to enable ANGLE.");
        return false;
    }

    return mUseAngle;
}

void GraphicsEnv::updateUseAngle() {
    mUseAngle = false;

    const char* ANGLE_PREFER_ANGLE = "angle";
    const char* ANGLE_PREFER_NATIVE = "native";

    if (mAngleDeveloperOptIn == ANGLE_PREFER_ANGLE) {
        ALOGV("User set \"Developer Options\" to force the use of ANGLE");
        mUseAngle = true;
    } else if (mAngleDeveloperOptIn == ANGLE_PREFER_NATIVE) {
        ALOGV("User set \"Developer Options\" to force the use of Native");
        mUseAngle = false;
    } else {
        // The "Developer Options" value wasn't set to force the use of ANGLE.  Need to temporarily
        // load ANGLE and call the updatable opt-in/out logic:

        // Check if ANGLE is enabled. Workaround for several bugs:
        // b/119305693 b/119322355 b/119305887
        // Something is not working correctly in the feature library
        char prop[PROPERTY_VALUE_MAX];
        property_get("debug.angle.enable", prop, "0");
        void* featureSo = nullptr;
        if (atoi(prop)) {
            featureSo = loadLibrary("feature_support");
        }
        if (featureSo) {
            ALOGV("loaded ANGLE's opt-in/out logic from namespace");
            mUseAngle = checkAngleRules(featureSo);
            dlclose(featureSo);
            featureSo = nullptr;
        } else {
            ALOGV("Could not load the ANGLE opt-in/out logic, cannot use ANGLE.");
        }
    }
}

void GraphicsEnv::setAngleInfo(const std::string path, const std::string appName,
                               const std::string developerOptIn, const int rulesFd,
                               const long rulesOffset, const long rulesLength) {
    ALOGV("setting ANGLE path to '%s'", path.c_str());
    mAnglePath = path;
    ALOGV("setting ANGLE app name to '%s'", appName.c_str());
    mAngleAppName = appName;
    ALOGV("setting ANGLE application opt-in to '%s'", developerOptIn.c_str());
    mAngleDeveloperOptIn = developerOptIn;

    lseek(rulesFd, rulesOffset, SEEK_SET);
    mRulesBuffer = std::vector<char>(rulesLength + 1);
    ssize_t numBytesRead = read(rulesFd, mRulesBuffer.data(), rulesLength);
    if (numBytesRead < 0) {
        ALOGE("Cannot read rules file: numBytesRead = %zd", numBytesRead);
        numBytesRead = 0;
    } else if (numBytesRead == 0) {
        ALOGW("Empty rules file");
    }
    if (numBytesRead != rulesLength) {
        ALOGW("Did not read all of the necessary bytes from the rules file."
              "expected: %ld, got: %zd",
              rulesLength, numBytesRead);
    }
    mRulesBuffer[numBytesRead] = '\0';

    // Update the current status of whether we should use ANGLE or not
    updateUseAngle();
}

void GraphicsEnv::setLayerPaths(NativeLoaderNamespace* appNamespace, const std::string layerPaths) {
    if (mLayerPaths.empty()) {
        mLayerPaths = layerPaths;
        mAppNamespace = appNamespace;
    } else {
        ALOGV("Vulkan layer search path already set, not clobbering with '%s' for namespace %p'",
              layerPaths.c_str(), appNamespace);
    }
}

NativeLoaderNamespace* GraphicsEnv::getAppNamespace() {
    return mAppNamespace;
}

std::string& GraphicsEnv::getAngleAppName() {
    return mAngleAppName;
}

const std::string& GraphicsEnv::getLayerPaths() {
    return mLayerPaths;
}

const std::string& GraphicsEnv::getDebugLayers() {
    return mDebugLayers;
}

const std::string& GraphicsEnv::getDebugLayersGLES() {
    return mDebugLayersGLES;
}

void GraphicsEnv::setDebugLayers(const std::string layers) {
    mDebugLayers = layers;
}

void GraphicsEnv::setDebugLayersGLES(const std::string layers) {
    mDebugLayersGLES = layers;
}

android_namespace_t* GraphicsEnv::getDriverNamespace() {
    static std::once_flag once;
    std::call_once(once, [this]() {
        if (mDriverPath.empty()) return;

        auto vndkNamespace = android_get_exported_namespace("vndk");
        if (!vndkNamespace) return;

        mDriverNamespace = android_create_namespace("gfx driver",
                                                    mDriverPath.c_str(), // ld_library_path
                                                    mDriverPath.c_str(), // default_library_path
                                                    ANDROID_NAMESPACE_TYPE_ISOLATED,
                                                    nullptr, // permitted_when_isolated_path
                                                    nullptr);

        const std::string llndkLibraries = getSystemNativeLibraries(NativeLibrary::LLNDK);
        if (llndkLibraries.empty()) {
            mDriverNamespace = nullptr;
            return;
        }
        if (!android_link_namespaces(mDriverNamespace, nullptr, llndkLibraries.c_str())) {
            ALOGE("Failed to link default namespace[%s]", dlerror());
            mDriverNamespace = nullptr;
            return;
        }

        const std::string vndkspLibraries = getSystemNativeLibraries(NativeLibrary::VNDKSP);
        if (vndkspLibraries.empty()) {
            mDriverNamespace = nullptr;
            return;
        }
        if (!android_link_namespaces(mDriverNamespace, vndkNamespace, vndkspLibraries.c_str())) {
            ALOGE("Failed to link vndk namespace[%s]", dlerror());
            mDriverNamespace = nullptr;
            return;
        }
    });

    return mDriverNamespace;
}

android_namespace_t* GraphicsEnv::getAngleNamespace() {
    static std::once_flag once;
    std::call_once(once, [this]() {
        if (mAnglePath.empty()) return;

        mAngleNamespace = android_create_namespace("ANGLE",
                                                   nullptr,            // ld_library_path
                                                   mAnglePath.c_str(), // default_library_path
                                                   ANDROID_NAMESPACE_TYPE_SHARED |
                                                           ANDROID_NAMESPACE_TYPE_ISOLATED,
                                                   nullptr, // permitted_when_isolated_path
                                                   nullptr);
        if (!mAngleNamespace) ALOGD("Could not create ANGLE namespace from default");
    });

    return mAngleNamespace;
}

} // namespace android
