#include <jni.h>
#include "capture/vk_capture.hh"

extern "C" JNIEXPORT void JNICALL
Java_net_seilent_seifg_NativeBridge_install(JNIEnv*, jclass) {
    seifg_capture::Install();
}
