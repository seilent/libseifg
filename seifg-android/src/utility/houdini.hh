#ifndef HOUDINI_HEADER
#define HOUDINI_HEADER

#if defined(__i386__) || defined(__x86_64__)

#include <optional>

#include "native_bridge.h"
#include "singleton.hh"

class Houdini : public Singleton<Houdini> {
    friend class Singleton;

public:
    std::optional<void*> LoadLibrary(const char* name, int flag);
    bool CallJNI(void* handle, void* vm, void* reserved);
    const char* GetError();

private:
    Houdini();
    int houdini_ver_ = 0;
    android::NativeBridgeCallbacks* houdini_itf_ = nullptr;
};

#endif // architecture defined

#endif // houdini.hh