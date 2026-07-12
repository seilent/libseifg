#ifndef CONFIG_HEADER
#define CONFIG_HEADER

#include <jni.h>
#include <sys/mman.h>

#include <optional>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include "logger.hh"

class ConfigValue {
public:
    int delay_ = 5;
    int fps_ = 90;
    bool mod_opcode_ = true;
    float scale_ = -1;
    int multiplier_ = 2;
    int quality_ = 2;

    ConfigValue(){};
    ConfigValue(int delay, int fps, bool mod_opcode, float scale)
        : delay_(delay),
          fps_(fps),
          mod_opcode_(mod_opcode),
          scale_(scale){};
    ConfigValue(const ConfigValue& lhs) {
        delay_ = lhs.delay_;
        fps_ = lhs.fps_;
        mod_opcode_ = lhs.mod_opcode_;
        scale_ = lhs.scale_;
        multiplier_ = lhs.multiplier_;
        quality_ = lhs.quality_;
    }

    void DebugPrint() const {
        LOG("\tdelay: %d | fps: %d | mod_opcode: %d | scale: %f | multiplier: %d | quality: %d",
            delay_, fps_, mod_opcode_, scale_, multiplier_, quality_);
    }
};

namespace Utility {
    std::optional<rapidjson::Document> LoadJsonFromFile(const char*);
    jobject GetApplication(JNIEnv* env);
    std::optional<jobject> GetApplicationInfo(JNIEnv* env);
    std::optional<std::string> GetLibraryPath(JNIEnv* env, jobject application_info);
    std::optional<JavaVM*> GetVM();
    int ChangeMemPermission(void* p, size_t n, int permission = PROT_READ | PROT_WRITE | PROT_EXEC);
    void NopFunc(unsigned char* ptr);
}; // namespace Utility

#endif // config.hh