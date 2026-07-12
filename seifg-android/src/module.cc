#include "module.hh"

#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>

#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "capture/vk_capture.hh"
#include "fpslimiter.hh"
#include "third/riru_hide/hide.hh"
#include "utility/houdini.hh"
#include "utility/socket.hh"

using namespace rapidjson;

static std::mutex config_mutex;
static std::unordered_map<std::string, ConfigValue> custom_list;
static ConfigValue global_cfg;

constexpr const char* ConfigFile = "/data/local/tmp/TargetList.json";

bool LoadConfig() {
    custom_list.clear();

    auto read_path = Utility::LoadJsonFromFile(ConfigFile);
    if (!read_path) {
        return false;
    }

    Document& doc = *read_path;
    if (auto itor = doc.FindMember("global"); itor != doc.MemberEnd() && itor->value.IsObject()) {
        const auto& itor_json_object = itor->value.GetObject();
        if (auto itor2 = itor_json_object.FindMember("delay"); itor2 != itor_json_object.MemberEnd() && itor2->value.IsInt()) {
            global_cfg.delay_ = itor2->value.GetInt();
        }
        if (auto itor2 = itor_json_object.FindMember("fps"); itor2 != itor_json_object.MemberEnd() && itor2->value.IsInt()) {
            global_cfg.fps_ = itor2->value.GetInt();
        }
        if (auto itor2 = itor_json_object.FindMember("mod_opcode"); itor2 != itor_json_object.MemberEnd() && itor2->value.IsBool()) {
            global_cfg.mod_opcode_ = itor2->value.GetBool();
        }
        if (auto itor2 = itor_json_object.FindMember("scale"); itor2 != itor_json_object.MemberEnd() && itor2->value.IsFloat()) {
            global_cfg.scale_ = itor2->value.GetFloat();
        }
        if (auto itor2 = itor_json_object.FindMember("multiplier"); itor2 != itor_json_object.MemberEnd() && itor2->value.IsInt()) {
            global_cfg.multiplier_ = itor2->value.GetInt();
        }
        if (auto itor2 = itor_json_object.FindMember("quality"); itor2 != itor_json_object.MemberEnd() && itor2->value.IsInt()) {
            global_cfg.quality_ = itor2->value.GetInt();
        }
    }

    if (auto itor = doc.FindMember("custom"); itor != doc.MemberEnd() && itor->value.IsObject()) {
        for (const auto& item : itor->value.GetObject()) {
            if (item.value.IsObject()) {
                auto cfg(global_cfg);
                if (item.value.MemberCount()) {
                    if (auto itor2 = item.value.FindMember("delay"); itor2 != item.value.MemberEnd() && itor2->value.IsInt()) {
                        cfg.delay_ = itor2->value.GetInt();
                    }
                    if (auto itor2 = item.value.FindMember("fps"); itor2 != item.value.MemberEnd() && itor2->value.IsInt()) {
                        cfg.fps_ = itor2->value.GetInt();
                    }
                    if (auto itor2 = item.value.FindMember("mod_opcode"); itor2 != item.value.MemberEnd() && itor2->value.IsBool()) {
                        cfg.mod_opcode_ = itor2->value.GetBool();
                    }
                    if (auto itor2 = item.value.FindMember("scale"); itor2 != item.value.MemberEnd() && itor2->value.IsFloat()) {
                        cfg.scale_ = itor2->value.GetFloat();
                    }
                    if (auto itor2 = item.value.FindMember("multiplier"); itor2 != item.value.MemberEnd() && itor2->value.IsInt()) {
                        cfg.multiplier_ = itor2->value.GetInt();
                    }
                    if (auto itor2 = item.value.FindMember("quality"); itor2 != item.value.MemberEnd() && itor2->value.IsInt()) {
                        cfg.quality_ = itor2->value.GetInt();
                    }
                }
                custom_list[item.name.GetString()] = cfg;
            }
        }
    }

    LOG("[LoadConfig] custom_list: %zu", custom_list.size());
    LOG("[LoadConfig] global_cfg: ");
    global_cfg.DebugPrint();

    return true;
}

void CompanionEntry(int s) {
    std::string package_name = read_string(s);

    std::lock_guard<std::mutex> lock(config_mutex);
    global_cfg = ConfigValue{};
    custom_list.clear();
    LoadConfig();

    if (auto itor = custom_list.find(package_name); itor != custom_list.end()) {
        write_int(s, 1);
        write_int(s, itor->second.delay_);
        write_int(s, itor->second.fps_);
        write_int(s, itor->second.mod_opcode_);
        write_float(s, itor->second.scale_);
        write_int(s, itor->second.multiplier_);
        write_int(s, itor->second.quality_);
    }
    else {
        write_int(s, 0);
        write_int(s, global_cfg.delay_);
        write_int(s, global_cfg.fps_);
        write_int(s, global_cfg.mod_opcode_);
        write_float(s, global_cfg.scale_);
        write_int(s, global_cfg.multiplier_);
        write_int(s, global_cfg.quality_);
    }
}

REGISTER_ZYGISK_MODULE(MyModule)
REGISTER_ZYGISK_COMPANION(CompanionEntry)

void MyModule::onLoad(Api* api, JNIEnv* env) {
    this->api = api;
    this->env = env;
}

void MyModule::preAppSpecialize(AppSpecializeArgs* args) {
    int client_socket = api->connectCompanion();

    package_name_ = env->GetStringUTFChars(args->nice_name, nullptr);
    write_string(client_socket, package_name_);

    has_custom_cfg_ = read_int(client_socket);
    current_cfg_.delay_ = read_int(client_socket);
    current_cfg_.fps_ = read_int(client_socket);
    current_cfg_.mod_opcode_ = read_int(client_socket);
    current_cfg_.scale_ = read_float(client_socket);
    current_cfg_.multiplier_ = read_int(client_socket);
    current_cfg_.quality_ = read_int(client_socket);

    close(client_socket);
}

void MyModule::ForHoudini() {
#if defined(__i386__) || defined(__x86_64__)
    std::thread([=]() {
        std::chrono::seconds sleep_duration(current_cfg_.delay_);
        std::this_thread::sleep_for(sleep_duration);
#ifdef __x86_64__
#define libdir       "/lib64/x86_64"
#define library_name "arm64-v8a.so"
#endif

#ifdef __i386__
#define libdir       "/lib64/x86"
#define library_name "armeabi-v7a.so"
#endif

        auto vms = Utility::GetVM();
        if (!vms) {
            ERROR("GetVM failed");
            return;
        }

        JNIEnv* env = nullptr;
        if ((*vms)->AttachCurrentThread(&env, nullptr) < 0) {
            ERROR("Cannot connect to JNI environment");
            return;
        }

        auto app_info = Utility::GetApplicationInfo(env);
        auto path = Utility::GetLibraryPath(env, app_info ? *app_info : nullptr);

        if (!path) {
            ERROR("GetLibraryPath failed");
            return;
        }

        if (path->find(libdir) == std::string::npos) {
            auto& houdini = Houdini::GetInstance();
            auto plugin = houdini.LoadLibrary("/data/local/tmp/gh@hexstr/UnityFPSUnlocker/" library_name, RTLD_NOW);
            if (plugin) {
                if (*plugin == nullptr) {
                    ERROR("Failed to load library : %s", Houdini::GetInstance().GetError());
                    return;
                }
                ConfigValue config(0, current_cfg_.fps_, current_cfg_.mod_opcode_, current_cfg_.scale_);
                if (!houdini.CallJNI(*plugin, *vms, &config)) {
                    ERROR("CallJNI failed");
                }
                riru_hide("/data/local/tmp/gh@hexstr/UnityFPSUnlocker/" library_name);
            }
            else {
                ERROR("LoadLibrary failed: %s", Houdini::GetInstance().GetError());
            }
        }
        else {
            FPSLimiter::Start(current_cfg_);
        }
    }).detach();
#endif
}

void MyModule::postAppSpecialize(const AppSpecializeArgs* args) {
    bool is_target = has_custom_cfg_;

    if (is_target) {
        seifg_capture::SetConfig(current_cfg_.fps_, current_cfg_.multiplier_, current_cfg_.quality_);
        seifg_capture::Install();

        if (current_cfg_.fps_ > 0) {
#if defined(__ARM_ARCH_7A__) || defined(__aarch64__)
            std::thread([=]() {
                FPSLimiter::Start(current_cfg_);
            }).detach();
#endif
            ForHoudini();
        }
    }
    else {
        api->setOption(zygisk::Option::DLCLOSE_MODULE_LIBRARY);
    }
    env->ReleaseStringUTFChars(args->nice_name, package_name_);
}

#if defined(__ARM_ARCH_7A__) || defined(__aarch64__)

extern "C" {
JNIEXPORT void JNICALL Java_io_github_hexstr_UnityFPSUnlocker_MyModule_HelloWorld(JNIEnv* env, jobject obj, jint delay, jint fps, jint mod_opcode, jfloat scale) {
    LOG("[UnityFPSUnlocker][xposed] delay: %d | fps: %d | mod_opcode: %d | scale: %f", delay, fps, mod_opcode, scale);
    ConfigValue current_cfg(delay, fps, mod_opcode, scale);
    std::thread([=]() {
        FPSLimiter::Start(current_cfg);
    }).detach();
}
}

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved) {
    if (reserved) {
        ConfigValue* config = reinterpret_cast<ConfigValue*>(reserved);

        std::thread([=]() {
            FPSLimiter::Start(*config);
        }).detach();
    }
    return JNI_VERSION_1_6;
}

#endif