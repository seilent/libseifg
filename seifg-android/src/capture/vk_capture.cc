#include "capture/vk_capture.hh"

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <time.h>

#include <dlfcn.h>
#include "shadowhook.h"
#include "utility/logger.hh"
#include "present/sc_present.hh"
#include "seifg.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace seifg_capture {

static PFN_vkGetInstanceProcAddr g_real_gipa = nullptr;
static PFN_vkGetDeviceProcAddr g_real_gdpa = nullptr;
static PFN_vkCreateDevice g_real_create_device = nullptr;
static PFN_vkCreateSwapchainKHR g_real_create_swapchain = nullptr;
static PFN_vkQueuePresentKHR g_real_present = nullptr;
static VkInstance g_vkInstance = VK_NULL_HANDLE;

typedef EGLBoolean (*PFN_eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static PFN_eglSwapBuffers_t g_orig_egl_swap = nullptr;

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID = nullptr;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES = nullptr;

typedef int (*pfn_sh_init)(shadowhook_mode_t, bool);
typedef void* (*pfn_sh_hook_sym_name)(const char*, const char*, void*, void**);
typedef int (*pfn_sh_get_errno)(void);
typedef const char* (*pfn_sh_to_errmsg)(int);

static void* g_sh_handle = nullptr;
static pfn_sh_init p_sh_init = nullptr;
static pfn_sh_hook_sym_name p_sh_hook_sym_name = nullptr;
static pfn_sh_get_errno p_sh_get_errno = nullptr;
static pfn_sh_to_errmsg p_sh_to_errmsg = nullptr;

static bool load_shadowhook() {
    if (g_sh_handle) return true;
    g_sh_handle = dlopen("/system/lib64/libshadowhook.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_sh_handle) g_sh_handle = dlopen("libshadowhook.so", RTLD_NOW | RTLD_GLOBAL);
    if (!g_sh_handle) { ERROR("[Install] dlopen libshadowhook.so failed: %s", dlerror()); return false; }
    p_sh_init = (pfn_sh_init)dlsym(g_sh_handle, "shadowhook_init");
    p_sh_hook_sym_name = (pfn_sh_hook_sym_name)dlsym(g_sh_handle, "shadowhook_hook_sym_name");
    p_sh_get_errno = (pfn_sh_get_errno)dlsym(g_sh_handle, "shadowhook_get_errno");
    p_sh_to_errmsg = (pfn_sh_to_errmsg)dlsym(g_sh_handle, "shadowhook_to_errmsg");
    if (!p_sh_init || !p_sh_hook_sym_name || !p_sh_get_errno || !p_sh_to_errmsg) { ERROR("[Install] shadowhook dlsym failed"); return false; }
    LOG("[Install] libshadowhook.so loaded via dlopen");
    return true;
}

static std::atomic<bool> g_seifg_inited{false};
static std::atomic<bool> g_gen_ready{false};
static int32_t g_ctx = -1;
static AHardwareBuffer* g_ahbIn0 = nullptr;
static AHardwareBuffer* g_ahbIn1 = nullptr;
static AHardwareBuffer* g_ahbOut = nullptr;
static AHardwareBuffer* g_ahbReal = nullptr;
static GLuint g_texIn0 = 0, g_fboIn0 = 0, g_texIn1 = 0, g_fboIn1 = 0;
static GLuint g_texOut = 0, g_fboOut = 0;
static int g_genW = 0, g_genH = 0;
static char g_triggerPath[512] = {0};
static char g_noReinjectPath[512] = {0};
static bool g_reinject = true;

static VkPhysicalDevice g_vkPhys = VK_NULL_HANDLE;
static VkDevice g_vkDevice = VK_NULL_HANDLE;
static VkQueue g_vkQueue = VK_NULL_HANDLE;
static uint32_t g_vkFamily = 0;
static VkCommandPool g_vkPool = VK_NULL_HANDLE;
static VkCommandBuffer g_vkCmd = VK_NULL_HANDLE;
static VkFence g_vkFence = VK_NULL_HANDLE;
static VkBuffer g_vkStaging = VK_NULL_HANDLE;
static VkDeviceMemory g_vkStagingMem = VK_NULL_HANDLE;
static VkDeviceSize g_vkStagingSize = 0;
static std::atomic<bool> g_vk_gen_ready{false};
static int g_vkW = 0;
static int g_vkH = 0;
static VkFormat g_vkFmt = VK_FORMAT_UNDEFINED;

static bool g_ahbImportAvailable = false;
static constexpr int kMaxOutputs = 2;
static VkImage g_ahbIn0Img = VK_NULL_HANDLE;
static VkImage g_ahbIn1Img = VK_NULL_HANDLE;
static VkImage g_ahbRealImg = VK_NULL_HANDLE;
static VkImage g_ahbOutImg[kMaxOutputs] = {};
static VkDeviceMemory g_ahbIn0Mem = VK_NULL_HANDLE;
static VkDeviceMemory g_ahbIn1Mem = VK_NULL_HANDLE;
static VkDeviceMemory g_ahbRealMem = VK_NULL_HANDLE;
static VkDeviceMemory g_ahbOutMem[kMaxOutputs] = {};
static bool g_ahbImagesImported = false;

static VkCommandPool g_gpuCopyPool = VK_NULL_HANDLE;
static VkCommandBuffer g_gpuCopyCmd = VK_NULL_HANDLE;
static VkFence g_gpuCopyFence = VK_NULL_HANDLE;

static VkCommandPool g_gpuSnapPool = VK_NULL_HANDLE;
static VkCommandBuffer g_gpuSnapCmd = VK_NULL_HANDLE;
static VkFence g_gpuSnapFence = VK_NULL_HANDLE;

static PFN_vkGetAndroidHardwareBufferPropertiesANDROID g_vkGetAhbProps = nullptr;

static std::atomic<uint64_t> g_captureCount{0};

static int g_cfgFps = 0;
static int g_cfgMultiplier = 2;
static int g_cfgQuality = 2;
static int g_cfgTargetFps = 0;

static std::atomic<int64_t> g_renderIntervalNs{0};
static std::atomic<int64_t> g_presentSpacingNs{0};
static std::atomic<int> g_pacingK{1};
static int64_t g_lastPeriodNs = 0;

static ScPresenter g_scPresenter;

static void recomputePacing() {
    int64_t P = g_scPresenter.clock().periodNs();
    if (P <= 0 || g_cfgTargetFps <= 0) return;
    if (g_lastPeriodNs != 0 && llabs(P - g_lastPeriodNs) * 8 < g_lastPeriodNs) return;
    int64_t R = (1000000000LL + P / 2) / P;
    int m = g_cfgMultiplier;
    int k = static_cast<int>((R + g_cfgTargetFps / 2) / g_cfgTargetFps);
    if (k < 1) k = 1;
    int64_t baseFps = R / (static_cast<int64_t>(k) * m);
    if (baseFps < 10) {
        k = static_cast<int>(R / (10LL * m));
        if (k < 1) k = 1;
    }
    g_pacingK.store(k, std::memory_order_relaxed);
    g_renderIntervalNs.store(static_cast<int64_t>(k) * m * P, std::memory_order_relaxed);
    g_presentSpacingNs.store(static_cast<int64_t>(k) * P, std::memory_order_relaxed);
    g_lastPeriodNs = P;
    LOG("[Pacing] P=%lld R=%lld k=%d m=%d renderInterval=%lld presentSpacing=%lld",
        (long long)P, (long long)R, k, m,
        (long long)(static_cast<int64_t>(k) * m * P),
        (long long)(static_cast<int64_t>(k) * P));
}

static AHardwareBuffer* g_ahbOutN[kMaxOutputs] = {};
static GLuint g_texOutN[kMaxOutputs] = {};
static GLuint g_fboOutN[kMaxOutputs] = {};

static int32_t g_scBufTransform = 0;

struct SwapchainInfo {
    VkDevice device;
    VkFormat format;
    VkExtent2D extent;
    uint32_t imageCount;
    std::vector<VkImage> images;
    bool virtualized = false;
    VkSwapchainKHR realSwapchain = VK_NULL_HANDLE;
    std::vector<VkImage> virtualImages;
    std::vector<VkDeviceMemory> virtualMem;
    std::vector<VkImage> realImages;
    uint32_t nextVirtualIdx = 0;
    VkSemaphore realAcquireSem = VK_NULL_HANDLE;
    VkSemaphore blitDoneSem = VK_NULL_HANDLE;
    VkFence blitFence = VK_NULL_HANDLE;
    VkCommandPool blitPool = VK_NULL_HANDLE;
    VkCommandBuffer blitCmd = VK_NULL_HANDLE;
};

static std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;
static std::mutex g_mu;

static std::unordered_map<VkSurfaceKHR, ANativeWindow*> g_surfaceWindowMap;
static std::unordered_map<VkSwapchainKHR, ANativeWindow*> g_swapchainWindowMap;
static std::mutex g_scMu;

static std::thread g_fgThread;
static std::mutex g_fgMutex;
static std::condition_variable g_fgCv;
static std::atomic<bool> g_fgRunning{false};
static bool g_fgPendingValid = false;
static std::mutex g_fgBufMutex;

struct FgRequest {
    uint64_t captureCount = 0;
    uint64_t seq = 0;
};
static FgRequest g_fgPending;

static std::atomic<uint64_t> g_fgSeq{0};
static int64_t g_nextAcquireNs = 0;

struct FgInstrStats {
    std::atomic<uint64_t> captured{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint64_t> generated{0};
    std::atomic<uint64_t> lastPrevSeq{0};
    std::atomic<uint64_t> lastCurSeq{0};
    std::atomic<uint64_t> gapSum{0};
    std::atomic<uint64_t> gapCount{0};
};
static FgInstrStats g_instr;

static void framegen_thread_loop();

static PFN_vkGetSwapchainImagesKHR g_real_get_swapchain_images = nullptr;
static PFN_vkAcquireNextImageKHR g_real_acquire = nullptr;
static PFN_vkAcquireNextImage2KHR g_real_acquire2 = nullptr;
static PFN_vkCreateAndroidSurfaceKHR g_real_create_android_surface = nullptr;
static PFN_vkDestroySurfaceKHR g_real_destroy_surface = nullptr;

static VkResult VKAPI_CALL hooked_CreateDevice(
    VkPhysicalDevice phys,
    const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a,
    VkDevice* pDev);
static VkResult VKAPI_CALL hooked_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a,
    VkSwapchainKHR* pSc);
static VkResult VKAPI_CALL hooked_QueuePresentKHR(
    VkQueue q,
    const VkPresentInfoKHR* pi);
static VkResult VKAPI_CALL hooked_GetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages);
static VkResult VKAPI_CALL hooked_AcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex);
static VkResult VKAPI_CALL hooked_AcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex);
static VkResult VKAPI_CALL hooked_CreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface);
static void VKAPI_CALL hooked_DestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_GetDeviceProcAddr(VkDevice device, const char* pName);
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_GetInstanceProcAddr(VkInstance instance, const char* pName);

static void seifg_write_png(const char* name, int w, int h, const uint8_t* rgba, bool flipV) {
    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) {
        size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf);
        (void)rd;
        fclose(cf);
    }
    char path[512];
    snprintf(path, sizeof(path), "/sdcard/Android/data/%s/files/%s", pkg, name);
    stbi_flip_vertically_on_write(flipV ? 1 : 0);
    int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
    LOG("[Gen] wrote %s ok=%d", path, ok);
}

static void dump_ahb(AHardwareBuffer* ahb, const char* name, bool flipV) {
    if (!ahb) return;
    AHardwareBuffer_Desc dd = {};
    AHardwareBuffer_describe(ahb, &dd);
    void* base = nullptr;
    int lr = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &base);
    if (lr != 0 || !base) { LOG("[Gen] lock %s failed rc=%d", name, lr); return; }
    int w = static_cast<int>(dd.width);
    int h = static_cast<int>(dd.height);
    uint32_t stride = dd.stride;
    std::vector<uint8_t> tight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    const uint8_t* src = static_cast<const uint8_t*>(base);
    for (int y = 0; y < h; ++y) {
        memcpy(tight.data() + static_cast<size_t>(y) * w * 4,
               src + static_cast<size_t>(y) * stride * 4,
               static_cast<size_t>(w) * 4);
    }
    AHardwareBuffer_unlock(ahb, nullptr);
    seifg_write_png(name, w, h, tight.data(), flipV);
}

static AHardwareBuffer* alloc_ahb(int w, int h) {
    AHardwareBuffer_Desc d = {};
    d.width = static_cast<uint32_t>(w);
    d.height = static_cast<uint32_t>(h);
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
              AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
              AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN |
              AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* a = nullptr;
    int r = AHardwareBuffer_allocate(&d, &a);
    if (r != 0) { LOG("[Gen] ahb alloc rc=%d", r); return nullptr; }
    return a;
}

static bool make_ahb_fbo(EGLDisplay dpy, AHardwareBuffer* ahb, GLuint* tex, GLuint* fbo) {
    EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(ahb);
    EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attrs);
    if (img == EGL_NO_IMAGE_KHR) { LOG("[Gen] eglCreateImageKHR failed 0x%x", eglGetError()); return false; }
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (st != GL_FRAMEBUFFER_COMPLETE) { LOG("[Gen] fbo incomplete 0x%x", st); return false; }
    return true;
}

static void seifg_gen_setup(EGLDisplay dpy, int w, int h) {
    if (!p_eglGetNativeClientBufferANDROID) {
        p_eglGetNativeClientBufferANDROID =
            (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        p_glEGLImageTargetTexture2DOES =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
    if (!p_eglGetNativeClientBufferANDROID || !p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) {
        LOG("[Gen] procs missing");
        return;
    }

    g_ahbIn0 = alloc_ahb(w, h);
    g_ahbIn1 = alloc_ahb(w, h);
    int numOut = g_cfgMultiplier - 1;
    for (int i = 0; i < numOut; ++i)
        g_ahbOutN[i] = alloc_ahb(w, h);
    if (numOut > 0) g_ahbOut = g_ahbOutN[0];
    if (!g_ahbIn0 || !g_ahbIn1 || (numOut > 0 && !g_ahbOut)) { LOG("[Gen] ahb alloc failed"); return; }

    GLint prevTex = 0, prevFbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    bool ok0 = make_ahb_fbo(dpy, g_ahbIn0, &g_texIn0, &g_fboIn0);
    bool ok1 = make_ahb_fbo(dpy, g_ahbIn1, &g_texIn1, &g_fboIn1);
    bool okOut = true;
    for (int i = 0; i < numOut; ++i) {
        if (!make_ahb_fbo(dpy, g_ahbOutN[i], &g_texOutN[i], &g_fboOutN[i])) {
            okOut = false;
            break;
        }
    }
    if (numOut > 0) {
        g_texOut = g_texOutN[0];
        g_fboOut = g_fboOutN[0];
    }
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (!ok0 || !ok1 || !okOut) { LOG("[Gen] fbo setup failed"); return; }

    g_genW = w;
    g_genH = h;

    if (numOut > 0) {
        std::vector<AHardwareBuffer*> outVec(numOut);
        for (int i = 0; i < numOut; ++i)
            outVec[i] = g_ahbOutN[i];
        g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, outVec,
                                            VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)},
                                            VK_FORMAT_R8G8B8A8_UNORM);
    }

    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) { size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf); (void)rd; fclose(cf); }
    snprintf(g_triggerPath, sizeof(g_triggerPath), "/sdcard/Android/data/%s/files/seifg_trigger", pkg);
    snprintf(g_noReinjectPath, sizeof(g_noReinjectPath), "/sdcard/Android/data/%s/files/seifg_no_reinject", pkg);

    LOG("[Gen] setup ctx=%d %dx%d trigger=%s", g_ctx, w, h, g_triggerPath);
    if (g_ctx >= 0 || numOut == 0) g_gen_ready = true;
}

static void seifg_gen_frame(EGLDisplay dpy, EGLSurface surf, uint64_t n) {
    GLint prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fboIn1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fboIn0);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fboIn1);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));

    if (g_triggerPath[0] && access(g_triggerPath, F_OK) == 0) {
        glFinish();
        if (g_ctx >= 0) {
            seifg::presentContext(g_ctx, -1, {});
            seifg::waitIdle();
        }
        dump_ahb(g_ahbIn0, "seifg_in0.png", true);
        dump_ahb(g_ahbIn1, "seifg_in1.png", true);
        dump_ahb(g_ahbOut, "seifg_out.png", true);
        LOG("[Gen] triggered dump at frame %llu", static_cast<unsigned long long>(n));
        unlink(g_triggerPath);
    }
}

static EGLBoolean seifg_reinject_frame(EGLDisplay dpy, EGLSurface surf) {
    static std::atomic<uint64_t> rf{0};
    uint64_t k = rf.fetch_add(1);

    if (g_cfgFps > 0) {
        static struct timespec s_lastGl = {0, 0};
        if (s_lastGl.tv_sec == 0 && s_lastGl.tv_nsec == 0)
            clock_gettime(CLOCK_MONOTONIC, &s_lastGl);
        int64_t intervalNs = 1000000000LL / g_cfgFps;
        struct timespec target;
        target.tv_sec = s_lastGl.tv_sec;
        target.tv_nsec = s_lastGl.tv_nsec + intervalNs;
        if (target.tv_nsec >= 1000000000LL) {
            target.tv_sec += target.tv_nsec / 1000000000LL;
            target.tv_nsec %= 1000000000LL;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr);
        s_lastGl = target;
    }

    GLint pr = 0, pd = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &pr);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &pd);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fboIn1);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFinish();

    int numInterp = g_cfgMultiplier - 1;

    if (numInterp > 0) {
        seifg::presentContext(g_ctx, -1, {});
        seifg::waitIdle();

        for (int i = 0; i < numInterp; ++i) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fboOutN[i]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            g_orig_egl_swap(dpy, surf);
        }
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fboIn1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    EGLBoolean r = g_orig_egl_swap(dpy, surf);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fboIn1);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fboIn0);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(pr));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(pd));

    if (k < 3 || k % 300 == 0) {
        LOG("[Reinject] frame %llu presented %dx (interp=%d+real)",
            static_cast<unsigned long long>(k), g_cfgMultiplier, numInterp);
    }
    return r;
}

static EGLBoolean hooked_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    static std::atomic<uint64_t> frame{0};
    uint64_t n = frame.fetch_add(1);

    if (n < 3 || n % 300 == 0) {
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
        LOG("[GlCapture] eglSwapBuffers #%llu %dx%d",
            static_cast<unsigned long long>(n), w, h);
    }

    if (g_seifg_inited.load()) {
        if (!g_gen_ready.load()) {
            EGLint w = 0, h = 0;
            eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
            eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
            if (w > 0 && h > 0) seifg_gen_setup(dpy, w, h);
        }
        if (g_gen_ready.load()) {
            if ((n % 30) == 0 && g_noReinjectPath[0]) {
                g_reinject = (access(g_noReinjectPath, F_OK) != 0);
            }
            if (g_reinject) return seifg_reinject_frame(dpy, surface);
            seifg_gen_frame(dpy, surface, n);
        }
    }

    return g_orig_egl_swap(dpy, surface);
}

static PFN_vkVoidFunction resolve_real(VkDevice device, const char* name) {
    PFN_vkVoidFunction p = nullptr;
    if (g_real_gdpa) p = g_real_gdpa(device, name);
    if (!p && g_real_gipa && g_vkInstance) p = g_real_gipa(g_vkInstance, name);
    return p;
}

static PFN_vkVoidFunction resolve_instance(VkInstance instance, const char* name) {
    if (g_real_gipa) return g_real_gipa(instance, name);
    return nullptr;
}

static bool import_ahb_as_image(AHardwareBuffer* ahb, VkImage* outImg, VkDeviceMemory* outMem) {
    if (!g_vkGetAhbProps || !ahb) return false;

    VkAndroidHardwareBufferFormatPropertiesANDROID fmtProps{};
    fmtProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID ahbProps{};
    ahbProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    ahbProps.pNext = &fmtProps;
    if (g_vkGetAhbProps(g_vkDevice, ahb, &ahbProps) != VK_SUCCESS) return false;

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);

    VkExternalMemoryImageCreateInfo ext{};
    ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {desc.width, desc.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    auto realCreateImage = reinterpret_cast<PFN_vkCreateImage>(resolve_real(g_vkDevice, "vkCreateImage"));
    if (!realCreateImage) return false;
    if (realCreateImage(g_vkDevice, &ici, nullptr, outImg) != VK_SUCCESS) return false;

    VkPhysicalDeviceMemoryProperties memProps;
    auto realGetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        resolve_instance(g_vkInstance, "vkGetPhysicalDeviceMemoryProperties"));
    if (realGetMemProps) realGetMemProps(g_vkPhys, &memProps);
    else vkGetPhysicalDeviceMemoryProperties(g_vkPhys, &memProps);

    uint32_t memType = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        if (ahbProps.memoryTypeBits & (1u << i)) { memType = i; break; }
    if (memType == UINT32_MAX) {
        auto realDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(resolve_real(g_vkDevice, "vkDestroyImage"));
        if (realDestroyImage) realDestroyImage(g_vkDevice, *outImg, nullptr);
        *outImg = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryDedicatedAllocateInfo ded{};
    ded.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    ded.image = *outImg;
    VkImportAndroidHardwareBufferInfoANDROID imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    imp.pNext = &ded;
    imp.buffer = ahb;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = ahbProps.allocationSize;
    mai.memoryTypeIndex = memType;

    auto realAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(resolve_real(g_vkDevice, "vkAllocateMemory"));
    if (!realAllocMem || realAllocMem(g_vkDevice, &mai, nullptr, outMem) != VK_SUCCESS) {
        auto realDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(resolve_real(g_vkDevice, "vkDestroyImage"));
        if (realDestroyImage) realDestroyImage(g_vkDevice, *outImg, nullptr);
        *outImg = VK_NULL_HANDLE;
        return false;
    }

    auto realBindImageMem = reinterpret_cast<PFN_vkBindImageMemory>(resolve_real(g_vkDevice, "vkBindImageMemory"));
    if (!realBindImageMem || realBindImageMem(g_vkDevice, *outImg, *outMem, 0) != VK_SUCCESS) {
        auto realDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(resolve_real(g_vkDevice, "vkDestroyImage"));
        auto realFreeMem = reinterpret_cast<PFN_vkFreeMemory>(resolve_real(g_vkDevice, "vkFreeMemory"));
        if (realDestroyImage) realDestroyImage(g_vkDevice, *outImg, nullptr);
        if (realFreeMem) realFreeMem(g_vkDevice, *outMem, nullptr);
        *outImg = VK_NULL_HANDLE; *outMem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool init_gpu_copy_resources() {
    auto realCreatePool = reinterpret_cast<PFN_vkCreateCommandPool>(resolve_real(g_vkDevice, "vkCreateCommandPool"));
    auto realAllocCmd = reinterpret_cast<PFN_vkAllocateCommandBuffers>(resolve_real(g_vkDevice, "vkAllocateCommandBuffers"));
    auto realCreateFence = reinterpret_cast<PFN_vkCreateFence>(resolve_real(g_vkDevice, "vkCreateFence"));
    if (!realCreatePool || !realAllocCmd || !realCreateFence) return false;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_vkFamily;
    if (realCreatePool(g_vkDevice, &pci, nullptr, &g_gpuCopyPool) != VK_SUCCESS) return false;
    if (realCreatePool(g_vkDevice, &pci, nullptr, &g_gpuSnapPool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    cba.commandPool = g_gpuCopyPool;
    if (realAllocCmd(g_vkDevice, &cba, &g_gpuCopyCmd) != VK_SUCCESS) return false;
    cba.commandPool = g_gpuSnapPool;
    if (realAllocCmd(g_vkDevice, &cba, &g_gpuSnapCmd) != VK_SUCCESS) return false;

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (realCreateFence(g_vkDevice, &fci, nullptr, &g_gpuCopyFence) != VK_SUCCESS) return false;
    if (realCreateFence(g_vkDevice, &fci, nullptr, &g_gpuSnapFence) != VK_SUCCESS) return false;

    return true;
}

static bool ensure_ahb_images_imported() {
    if (g_ahbImagesImported) return true;
    if (!g_ahbImportAvailable || !g_ahbIn0 || !g_ahbIn1 || !g_ahbReal) return false;

    if (!g_gpuCopyPool && !init_gpu_copy_resources()) {
        g_ahbImportAvailable = false;
        return false;
    }

    if (!import_ahb_as_image(g_ahbIn0, &g_ahbIn0Img, &g_ahbIn0Mem)) { g_ahbImportAvailable = false; return false; }
    if (!import_ahb_as_image(g_ahbIn1, &g_ahbIn1Img, &g_ahbIn1Mem)) { g_ahbImportAvailable = false; return false; }
    if (!import_ahb_as_image(g_ahbReal, &g_ahbRealImg, &g_ahbRealMem)) { g_ahbImportAvailable = false; return false; }

    int numOut = g_cfgMultiplier - 1;
    for (int i = 0; i < numOut; ++i) {
        if (g_ahbOutN[i] && !import_ahb_as_image(g_ahbOutN[i], &g_ahbOutImg[i], &g_ahbOutMem[i])) {
            g_ahbImportAvailable = false;
            return false;
        }
    }

    g_ahbImagesImported = true;
    LOG("[GpuBlit] AHB images imported successfully");
    return true;
}

static VkResult VKAPI_CALL hooked_CreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface) {

    if (!g_real_create_android_surface) {
        g_real_create_android_surface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(
            resolve_instance(instance, "vkCreateAndroidSurfaceKHR"));
    }
    if (!g_real_create_android_surface) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult r = g_real_create_android_surface(instance, pCreateInfo, pAllocator, pSurface);
    if (r == VK_SUCCESS && pSurface && pCreateInfo->window) {
        ANativeWindow_acquire(pCreateInfo->window);
        std::lock_guard<std::mutex> lk(g_scMu);
        g_surfaceWindowMap[*pSurface] = pCreateInfo->window;
        LOG("[VkSC] captured window %p for surface %p",
            reinterpret_cast<void*>(pCreateInfo->window), reinterpret_cast<void*>(*pSurface));
    }
    return r;
}

static void VKAPI_CALL hooked_DestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator) {

    {
        std::lock_guard<std::mutex> lk(g_scMu);
        auto it = g_surfaceWindowMap.find(surface);
        if (it != g_surfaceWindowMap.end()) {
            g_scPresenter.destroy();
            ANativeWindow_release(it->second);
            g_surfaceWindowMap.erase(it);
            LOG("[VkSC] released window for surface %p", reinterpret_cast<void*>(surface));
        }
    }

    if (!g_real_destroy_surface) {
        g_real_destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(
            resolve_instance(instance, "vkDestroySurfaceKHR"));
    }
    if (g_real_destroy_surface)
        g_real_destroy_surface(instance, surface, pAllocator);
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) {
        if (!g_real_present)
            g_real_present = reinterpret_cast<PFN_vkQueuePresentKHR>(resolve_real(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_QueuePresentKHR);
    }
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        if (!g_real_create_swapchain)
            g_real_create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(resolve_real(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_CreateSwapchainKHR);
    }
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
        if (!g_real_get_swapchain_images)
            g_real_get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolve_real(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_GetSwapchainImagesKHR);
    }
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
        if (!g_real_acquire)
            g_real_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(resolve_real(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_AcquireNextImageKHR);
    }
    if (strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
        if (!g_real_acquire2)
            g_real_acquire2 = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(resolve_real(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_AcquireNextImage2KHR);
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(my_GetDeviceProcAddr);
    return g_real_gdpa ? g_real_gdpa(device, pName) : nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;
    if (instance != VK_NULL_HANDLE && g_vkInstance == VK_NULL_HANDLE) g_vkInstance = instance;

    static std::atomic_flag first_call = ATOMIC_FLAG_INIT;
    if (!first_call.test_and_set()) {
        LOG("[VkGen] GIPA intercepted first call");
    }

    if (strcmp(pName, "vkGetInstanceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(my_GetInstanceProcAddr);
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        if (!g_real_gdpa && g_real_gipa)
            g_real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(my_GetDeviceProcAddr);
    }
    if (strcmp(pName, "vkCreateDevice") == 0) {
        if (!g_real_create_device && g_real_gipa)
            g_real_create_device = reinterpret_cast<PFN_vkCreateDevice>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_CreateDevice);
    }
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        if (!g_real_create_swapchain && g_real_gipa)
            g_real_create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_CreateSwapchainKHR);
    }
    if (strcmp(pName, "vkQueuePresentKHR") == 0) {
        if (!g_real_present && g_real_gipa)
            g_real_present = reinterpret_cast<PFN_vkQueuePresentKHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_QueuePresentKHR);
    }
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
        if (!g_real_get_swapchain_images && g_real_gipa)
            g_real_get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_GetSwapchainImagesKHR);
    }
    if (strcmp(pName, "vkAcquireNextImageKHR") == 0) {
        if (!g_real_acquire && g_real_gipa)
            g_real_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_AcquireNextImageKHR);
    }
    if (strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
        if (!g_real_acquire2 && g_real_gipa)
            g_real_acquire2 = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_AcquireNextImage2KHR);
    }
    if (strcmp(pName, "vkCreateAndroidSurfaceKHR") == 0) {
        if (!g_real_create_android_surface && g_real_gipa)
            g_real_create_android_surface = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_CreateAndroidSurfaceKHR);
    }
    if (strcmp(pName, "vkDestroySurfaceKHR") == 0) {
        if (!g_real_destroy_surface && g_real_gipa)
            g_real_destroy_surface = reinterpret_cast<PFN_vkDestroySurfaceKHR>(g_real_gipa(instance, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_DestroySurfaceKHR);
    }
    return g_real_gipa ? g_real_gipa(instance, pName) : nullptr;
}

static VkResult VKAPI_CALL hooked_CreateDevice(
    VkPhysicalDevice phys,
    const VkDeviceCreateInfo* ci,
    const VkAllocationCallbacks* a,
    VkDevice* pDev) {

    if (!g_real_create_device) {
        ERROR("[VkGen] hooked_CreateDevice called but g_real_create_device is null");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    static const char* kRequiredExts[] = {
        "VK_ANDROID_external_memory_android_hardware_buffer",
        "VK_KHR_sampler_ycbcr_conversion",
        "VK_EXT_queue_family_foreign",
        "VK_KHR_external_memory",
        "VK_KHR_bind_memory2",
        "VK_KHR_get_memory_requirements2",
        "VK_KHR_dedicated_allocation",
        "VK_KHR_maintenance1",
    };
    static constexpr uint32_t kNumRequired = sizeof(kRequiredExts) / sizeof(kRequiredExts[0]);

    auto realEnumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        resolve_instance(g_vkInstance, "vkEnumerateDeviceExtensionProperties"));
    if (!realEnumExt && g_real_gipa)
        realEnumExt = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            g_real_gipa(g_vkInstance, "vkEnumerateDeviceExtensionProperties"));

    bool allSupported = false;
    std::vector<const char*> extraExts;

    if (realEnumExt) {
        uint32_t extCount = 0;
        realEnumExt(phys, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> available(extCount);
        realEnumExt(phys, nullptr, &extCount, available.data());

        allSupported = true;
        for (uint32_t r = 0; r < kNumRequired; ++r) {
            bool found = false;
            for (uint32_t e = 0; e < extCount; ++e) {
                if (strcmp(available[e].extensionName, kRequiredExts[r]) == 0) { found = true; break; }
            }
            if (!found) { allSupported = false; break; }
        }

        if (allSupported) {
            for (uint32_t r = 0; r < kNumRequired; ++r) {
                bool alreadyEnabled = false;
                for (uint32_t e = 0; e < ci->enabledExtensionCount; ++e) {
                    if (strcmp(ci->ppEnabledExtensionNames[e], kRequiredExts[r]) == 0) { alreadyEnabled = true; break; }
                }
                if (!alreadyEnabled) extraExts.push_back(kRequiredExts[r]);
            }
        }
    }

    VkDeviceCreateInfo modCi = *ci;
    std::vector<const char*> allExts;
    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeature{};

    if (allSupported && !extraExts.empty()) {
        allExts.reserve(ci->enabledExtensionCount + extraExts.size());
        for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i)
            allExts.push_back(ci->ppEnabledExtensionNames[i]);
        for (auto e : extraExts) allExts.push_back(e);
        modCi.enabledExtensionCount = static_cast<uint32_t>(allExts.size());
        modCi.ppEnabledExtensionNames = allExts.data();

        ycbcrFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
        ycbcrFeature.samplerYcbcrConversion = VK_TRUE;
        ycbcrFeature.pNext = const_cast<void*>(modCi.pNext);
        modCi.pNext = &ycbcrFeature;
    }

    VkResult result = g_real_create_device(phys, allSupported ? &modCi : ci, a, pDev);
    if (result == VK_SUCCESS && pDev) {
        g_vkPhys = phys;
        g_vkDevice = *pDev;
        if (ci->queueCreateInfoCount > 0) {
            g_vkFamily = ci->pQueueCreateInfos[0].queueFamilyIndex;
        }

        auto realGetQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(resolve_real(*pDev, "vkGetDeviceQueue"));
        if (realGetQueue) realGetQueue(*pDev, g_vkFamily, 0, &g_vkQueue);
        else vkGetDeviceQueue(*pDev, g_vkFamily, 0, &g_vkQueue);

        if (allSupported) {
            g_vkGetAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                resolve_real(*pDev, "vkGetAndroidHardwareBufferPropertiesANDROID"));
            g_ahbImportAvailable = (g_vkGetAhbProps != nullptr);
            LOG("[VkGen] AHB import %s (injected %zu exts)",
                g_ahbImportAvailable ? "available" : "unavailable (null getAhbProps)",
                extraExts.size());
        } else {
            g_ahbImportAvailable = false;
            LOG("[VkGen] AHB import unavailable (missing required ext)");
        }

        LOG("[VkGen] device recorded dev=%p phys=%p family=%u",
            reinterpret_cast<void*>(g_vkDevice),
            reinterpret_cast<void*>(g_vkPhys),
            g_vkFamily);
    }
    return result;
}

static uint32_t find_device_local_memory(VkPhysicalDevice phys, uint32_t typeBits) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            return i;
        }
    }
    return UINT32_MAX;
}

static VkResult VKAPI_CALL hooked_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a,
    VkSwapchainKHR* pSc) {

    if (!g_real_create_swapchain) {
        ERROR("[VkGen] hooked_CreateSwapchainKHR called but g_real_create_swapchain is null");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) { size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf); (void)rd; fclose(cf); }
    char reinjectFlag[512];
    snprintf(reinjectFlag, sizeof(reinjectFlag), "/sdcard/Android/data/%s/files/seifg_no_reinject", pkg);
    bool doVirtualize = (access(reinjectFlag, F_OK) != 0);

    if (!doVirtualize) {
        VkSwapchainCreateInfoKHR modCi = *ci;
        modCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        LOG("[VkCapture] adding TRANSFER_SRC to swapchain usage (was 0x%x now 0x%x)",
            ci->imageUsage, modCi.imageUsage);

        VkResult result = g_real_create_swapchain(device, &modCi, a, pSc);
        if (result == VK_SUCCESS && pSc) {
            if (!g_real_get_swapchain_images)
                g_real_get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolve_real(device, "vkGetSwapchainImagesKHR"));
            if (!g_real_get_swapchain_images) {
                ERROR("[VkGen] could not resolve vkGetSwapchainImagesKHR");
                return result;
            }
            uint32_t n = 0;
            g_real_get_swapchain_images(device, *pSc, &n, nullptr);
            std::vector<VkImage> images(n);
            g_real_get_swapchain_images(device, *pSc, &n, images.data());

            std::lock_guard<std::mutex> lock(g_mu);
            auto& info = g_swapchains[*pSc];
            info.device = device;
            info.format = ci->imageFormat;
            info.extent = ci->imageExtent;
            info.imageCount = n;
            info.images = std::move(images);
            info.virtualized = false;

            LOG("[VkCapture] swapchain created %ux%u fmt=%d images=%u",
                ci->imageExtent.width, ci->imageExtent.height,
                static_cast<int>(ci->imageFormat), n);
        }
        return result;
    }

    LOG("[VkReinject] virtualization enabled, creating real swapchain with FIFO");

    {
        VkSurfaceTransformFlagBitsKHR pt = ci->preTransform;
        int32_t bt = 0;
        if (pt == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
            bt = 7;
        else if (pt == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
            bt = 3;
        else if (pt == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
            bt = 4;
        g_scBufTransform = bt;
        g_scPresenter.setBufferTransform(bt);
        LOG("[VkReinject] preTransform=0x%x -> bufTransform=%d", static_cast<int>(pt), bt);
    }

    VkSwapchainCreateInfoKHR modCi = *ci;
    modCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    modCi.presentMode = VK_PRESENT_MODE_FIFO_KHR;

    VkResult result = g_real_create_swapchain(device, &modCi, a, pSc);
    if (result != VK_SUCCESS || !pSc) return result;

    VkSwapchainKHR realSc = *pSc;

    {
        std::lock_guard<std::mutex> lk(g_scMu);
        auto it = g_surfaceWindowMap.find(ci->surface);
        if (it != g_surfaceWindowMap.end())
            g_swapchainWindowMap[realSc] = it->second;
    }

    if (!g_real_get_swapchain_images)
        g_real_get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolve_real(device, "vkGetSwapchainImagesKHR"));
    if (!g_real_get_swapchain_images) {
        ERROR("[VkReinject] could not resolve vkGetSwapchainImagesKHR");
        return result;
    }

    uint32_t realCount = 0;
    g_real_get_swapchain_images(device, realSc, &realCount, nullptr);
    std::vector<VkImage> realImages(realCount);
    g_real_get_swapchain_images(device, realSc, &realCount, realImages.data());

    std::vector<VkImage> virtImages(realCount);
    std::vector<VkDeviceMemory> virtMem(realCount);

    VkImageCreateInfo imgCi{};
    imgCi.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgCi.imageType = VK_IMAGE_TYPE_2D;
    imgCi.format = ci->imageFormat;
    imgCi.extent = {ci->imageExtent.width, ci->imageExtent.height, 1};
    imgCi.mipLevels = 1;
    imgCi.arrayLayers = 1;
    imgCi.samples = VK_SAMPLE_COUNT_1_BIT;
    imgCi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgCi.usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imgCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imgCi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    for (uint32_t i = 0; i < realCount; ++i) {
        VkResult r = vkCreateImage(device, &imgCi, nullptr, &virtImages[i]);
        if (r != VK_SUCCESS) {
            ERROR("[VkReinject] vkCreateImage[%u] failed %d", i, r);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, virtImages[i], &memReqs);

        uint32_t memIdx = find_device_local_memory(g_vkPhys, memReqs.memoryTypeBits);
        if (memIdx == UINT32_MAX) {
            ERROR("[VkReinject] no DEVICE_LOCAL memory type");
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        VkMemoryAllocateInfo memAi{};
        memAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memAi.allocationSize = memReqs.size;
        memAi.memoryTypeIndex = memIdx;
        r = vkAllocateMemory(device, &memAi, nullptr, &virtMem[i]);
        if (r != VK_SUCCESS) {
            ERROR("[VkReinject] vkAllocateMemory[%u] failed %d", i, r);
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        r = vkBindImageMemory(device, virtImages[i], virtMem[i], 0);
        if (r != VK_SUCCESS) {
            ERROR("[VkReinject] vkBindImageMemory[%u] failed %d", i, r);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }

    VkSemaphoreCreateInfo semCi{};
    semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore realAcqSem = VK_NULL_HANDLE;
    VkSemaphore blitDoneSem = VK_NULL_HANDLE;
    vkCreateSemaphore(device, &semCi, nullptr, &realAcqSem);
    vkCreateSemaphore(device, &semCi, nullptr, &blitDoneSem);

    VkFenceCreateInfo fenceCi{};
    fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence blitFence = VK_NULL_HANDLE;
    vkCreateFence(device, &fenceCi, nullptr, &blitFence);

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = g_vkFamily;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool blitPool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &poolCi, nullptr, &blitPool);

    VkCommandBufferAllocateInfo cmdAi{};
    cmdAi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAi.commandPool = blitPool;
    cmdAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAi.commandBufferCount = 1;
    VkCommandBuffer blitCmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cmdAi, &blitCmd);

    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto& info = g_swapchains[realSc];
        info.device = device;
        info.format = ci->imageFormat;
        info.extent = ci->imageExtent;
        info.imageCount = realCount;
        info.images = {};
        info.virtualized = true;
        info.realSwapchain = realSc;
        info.virtualImages = std::move(virtImages);
        info.virtualMem = std::move(virtMem);
        info.realImages = std::move(realImages);
        info.nextVirtualIdx = 0;
        info.realAcquireSem = realAcqSem;
        info.blitDoneSem = blitDoneSem;
        info.blitFence = blitFence;
        info.blitPool = blitPool;
        info.blitCmd = blitCmd;
    }

    LOG("[VkReinject] virtualized swapchain %ux%u fmt=%d virtualImages=%u realImages=%u",
        ci->imageExtent.width, ci->imageExtent.height,
        static_cast<int>(ci->imageFormat), realCount, realCount);
    g_nextAcquireNs = 0;
    return VK_SUCCESS;
}

static void vk_capture_setup();

static bool vk_virtualized_capture(VkImage virtImg, const SwapchainInfo& scInfo) {
    if (!g_vk_gen_ready.load()) {
        g_vkW = static_cast<int>(scInfo.extent.width);
        g_vkH = static_cast<int>(scInfo.extent.height);
        g_vkFmt = scInfo.format;
        vk_capture_setup();
        if (!g_vk_gen_ready.load()) return false;
    }

    if (!g_fgBufMutex.try_lock()) return false;
    std::lock_guard<std::mutex> blk(g_fgBufMutex, std::adopt_lock);

    bool useGpu = g_ahbImportAvailable && ensure_ahb_images_imported();

    if (useGpu) {
        auto realResetCmd = reinterpret_cast<PFN_vkResetCommandBuffer>(resolve_real(g_vkDevice, "vkResetCommandBuffer"));
        auto realBeginCmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(resolve_real(g_vkDevice, "vkBeginCommandBuffer"));
        auto realEndCmd = reinterpret_cast<PFN_vkEndCommandBuffer>(resolve_real(g_vkDevice, "vkEndCommandBuffer"));
        auto realCmdBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(resolve_real(g_vkDevice, "vkCmdPipelineBarrier"));
        auto realCmdCopy = reinterpret_cast<PFN_vkCmdCopyImage>(resolve_real(g_vkDevice, "vkCmdCopyImage"));
        auto realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(resolve_real(g_vkDevice, "vkQueueSubmit"));
        auto realWaitFences = reinterpret_cast<PFN_vkWaitForFences>(resolve_real(g_vkDevice, "vkWaitForFences"));
        auto realResetFences = reinterpret_cast<PFN_vkResetFences>(resolve_real(g_vkDevice, "vkResetFences"));

        if (!realResetCmd || !realBeginCmd || !realEndCmd || !realCmdBarrier || !realCmdCopy ||
            !realSubmit || !realWaitFences || !realResetFences) {
            useGpu = false;
            g_ahbImportAvailable = false;
        } else {
            realResetCmd(g_gpuCopyCmd, 0);
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            realBeginCmd(g_gpuCopyCmd, &beginInfo);

            VkImageMemoryBarrier bars[4]{};
            uint32_t barCount = 0;

            bars[barCount].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bars[barCount].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bars[barCount].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bars[barCount].srcAccessMask = 0;
            bars[barCount].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            bars[barCount].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[barCount].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[barCount].image = g_ahbIn1Img;
            bars[barCount].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barCount++;

            bars[barCount].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            bars[barCount].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bars[barCount].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bars[barCount].srcAccessMask = 0;
            bars[barCount].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bars[barCount].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[barCount].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bars[barCount].image = g_ahbIn0Img;
            bars[barCount].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            barCount++;

            realCmdBarrier(g_gpuCopyCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, barCount, bars);

            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {static_cast<uint32_t>(g_vkW), static_cast<uint32_t>(g_vkH), 1};
            realCmdCopy(g_gpuCopyCmd, g_ahbIn1Img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        g_ahbIn0Img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier midBars[3]{};
            uint32_t midCount = 0;

            midBars[midCount].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            midBars[midCount].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            midBars[midCount].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            midBars[midCount].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            midBars[midCount].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            midBars[midCount].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midBars[midCount].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midBars[midCount].image = g_ahbIn1Img;
            midBars[midCount].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            midCount++;

            midBars[midCount].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            midBars[midCount].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            midBars[midCount].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            midBars[midCount].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
            midBars[midCount].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            midBars[midCount].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midBars[midCount].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            midBars[midCount].image = virtImg;
            midBars[midCount].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            midCount++;

            realCmdBarrier(g_gpuCopyCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, midCount, midBars);

            realCmdCopy(g_gpuCopyCmd, virtImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        g_ahbIn1Img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier postBar{};
            postBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            postBar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            postBar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            postBar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            postBar.dstAccessMask = 0;
            postBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            postBar.image = virtImg;
            postBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            realCmdBarrier(g_gpuCopyCmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &postBar);

            realEndCmd(g_gpuCopyCmd);

            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &g_gpuCopyCmd;
            realSubmit(g_vkQueue, 1, &submit, g_gpuCopyFence);
            realWaitFences(g_vkDevice, 1, &g_gpuCopyFence, VK_TRUE, UINT64_MAX);
            realResetFences(g_vkDevice, 1, &g_gpuCopyFence);
        }
    }

    if (!useGpu) {
        AHardwareBuffer_Desc d0{}, d1{};
        AHardwareBuffer_describe(g_ahbIn0, &d0);
        AHardwareBuffer_describe(g_ahbIn1, &d1);
        void* base0 = nullptr;
        void* base1 = nullptr;
        AHardwareBuffer_lock(g_ahbIn1, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &base1);
        AHardwareBuffer_lock(g_ahbIn0, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &base0);
        if (base0 && base1) {
            uint32_t copyW = static_cast<uint32_t>(g_vkW) * 4;
            for (int y = 0; y < g_vkH; ++y) {
                memcpy(static_cast<uint8_t*>(base0) + static_cast<size_t>(y) * d0.stride * 4,
                       static_cast<uint8_t*>(base1) + static_cast<size_t>(y) * d1.stride * 4,
                       copyW);
            }
        }
        AHardwareBuffer_unlock(g_ahbIn0, nullptr);
        AHardwareBuffer_unlock(g_ahbIn1, nullptr);

        vkResetCommandBuffer(g_vkCmd, 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_vkCmd, &beginInfo);

        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = virtImg;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(g_vkCmd,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);

        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {static_cast<uint32_t>(g_vkW), static_cast<uint32_t>(g_vkH), 1};
        vkCmdCopyImageToBuffer(g_vkCmd, virtImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_vkStaging, 1, &region);

        bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        bar.dstAccessMask = 0;
        vkCmdPipelineBarrier(g_vkCmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &bar);

        vkEndCommandBuffer(g_vkCmd);

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g_vkCmd;
        vkQueueSubmit(g_vkQueue, 1, &submit, g_vkFence);
        vkWaitForFences(g_vkDevice, 1, &g_vkFence, VK_TRUE, UINT64_MAX);
        vkResetFences(g_vkDevice, 1, &g_vkFence);

        void* mapped = nullptr;
        vkMapMemory(g_vkDevice, g_vkStagingMem, 0, g_vkStagingSize, 0, &mapped);
        if (mapped) {
            AHardwareBuffer_Desc dd{};
            AHardwareBuffer_describe(g_ahbIn1, &dd);
            void* ahbBase = nullptr;
            AHardwareBuffer_lock(g_ahbIn1, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ahbBase);
            if (ahbBase) {
                for (int y = 0; y < g_vkH; ++y) {
                    memcpy(static_cast<uint8_t*>(ahbBase) + static_cast<size_t>(y) * dd.stride * 4,
                           static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * g_vkW * 4,
                           static_cast<size_t>(g_vkW) * 4);
                }
            }
            AHardwareBuffer_unlock(g_ahbIn1, nullptr);
            vkUnmapMemory(g_vkDevice, g_vkStagingMem);
        }
    }

    g_captureCount.fetch_add(1);

    return true;
}

static void vk_capture_setup() {
    if (g_vk_gen_ready.load()) return;
    if (!g_vkDevice || !g_vkQueue) return;

    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_swapchains.empty()) return;
        auto& info = g_swapchains.begin()->second;
        extent = info.extent;
        format = info.format;
    }

    g_vkW = static_cast<int>(extent.width);
    g_vkH = static_cast<int>(extent.height);
    g_vkFmt = format;

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = g_vkFamily;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkResult r = vkCreateCommandPool(g_vkDevice, &poolCi, nullptr, &g_vkPool);
    if (r != VK_SUCCESS) { LOG("[VkGen] createCommandPool failed %d", r); return; }

    VkCommandBufferAllocateInfo allocCi{};
    allocCi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocCi.commandPool = g_vkPool;
    allocCi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocCi.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(g_vkDevice, &allocCi, &g_vkCmd);
    if (r != VK_SUCCESS) { LOG("[VkGen] allocCommandBuffers failed %d", r); return; }

    VkFenceCreateInfo fenceCi{};
    fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = vkCreateFence(g_vkDevice, &fenceCi, nullptr, &g_vkFence);
    if (r != VK_SUCCESS) { LOG("[VkGen] createFence failed %d", r); return; }

    g_vkStagingSize = static_cast<VkDeviceSize>(g_vkW) * g_vkH * 4;
    VkBufferCreateInfo bufCi{};
    bufCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCi.size = g_vkStagingSize;
    bufCi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    r = vkCreateBuffer(g_vkDevice, &bufCi, nullptr, &g_vkStaging);
    if (r != VK_SUCCESS) { LOG("[VkGen] createBuffer failed %d", r); return; }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(g_vkDevice, g_vkStaging, &memReqs);

    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(g_vkPhys, &memProps);

    uint32_t memIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((memReqs.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            memIdx = i;
            break;
        }
    }
    if (memIdx == UINT32_MAX) { LOG("[VkGen] no suitable memory type"); return; }

    VkMemoryAllocateInfo memAi{};
    memAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAi.allocationSize = memReqs.size;
    memAi.memoryTypeIndex = memIdx;
    r = vkAllocateMemory(g_vkDevice, &memAi, nullptr, &g_vkStagingMem);
    if (r != VK_SUCCESS) { LOG("[VkGen] allocMemory failed %d", r); return; }

    r = vkBindBufferMemory(g_vkDevice, g_vkStaging, g_vkStagingMem, 0);
    if (r != VK_SUCCESS) { LOG("[VkGen] bindBufferMemory failed %d", r); return; }

    if (!g_ahbIn0) {
        g_ahbIn0 = alloc_ahb(g_vkW, g_vkH);
        g_ahbIn1 = alloc_ahb(g_vkW, g_vkH);
        g_ahbReal = alloc_ahb(g_vkW, g_vkH);
        int numOut = g_cfgMultiplier - 1;
        for (int i = 0; i < numOut; ++i)
            g_ahbOutN[i] = alloc_ahb(g_vkW, g_vkH);
        if (numOut > 0) g_ahbOut = g_ahbOutN[0];
        if (!g_ahbIn0 || !g_ahbIn1 || !g_ahbReal || (numOut > 0 && !g_ahbOut)) { LOG("[VkGen] ahb alloc failed"); return; }
    }

    int numOut = g_cfgMultiplier - 1;
    if (numOut > 0) {
        std::vector<AHardwareBuffer*> outVec(numOut);
        for (int i = 0; i < numOut; ++i)
            outVec[i] = g_ahbOutN[i];

        g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, outVec,
                                            VkExtent2D{extent.width, extent.height},
                                            VK_FORMAT_R8G8B8A8_UNORM);
    }

    if (g_triggerPath[0] == '\0') {
        char pkg[256] = {0};
        FILE* cf = fopen("/proc/self/cmdline", "r");
        if (cf) { size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf); (void)rd; fclose(cf); }
        snprintf(g_triggerPath, sizeof(g_triggerPath), "/sdcard/Android/data/%s/files/seifg_trigger", pkg);
    }

    LOG("[VkGen] setup ctx=%d %dx%d fmt=%d", g_ctx, g_vkW, g_vkH, static_cast<int>(format));
    if (g_ctx >= 0 || numOut == 0) g_vk_gen_ready = true;
}

static void framegen_instr_tick() {
    static struct timespec s_last = {0, 0};
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s_last.tv_sec == 0 && s_last.tv_nsec == 0) { s_last = now; return; }
    int64_t elapsedNs = (now.tv_sec - s_last.tv_sec) * 1000000000LL + (now.tv_nsec - s_last.tv_nsec);
    if (elapsedNs < 1000000000LL) return;
    s_last = now;

    uint64_t cap = g_instr.captured.exchange(0);
    uint64_t drop = g_instr.dropped.exchange(0);
    uint64_t gen = g_instr.generated.exchange(0);
    uint64_t prevSeq = g_instr.lastPrevSeq.load();
    uint64_t curSeq = g_instr.lastCurSeq.load();
    uint64_t gapS = g_instr.gapSum.exchange(0);
    uint64_t gapC = g_instr.gapCount.exchange(0);
    float gapAvg = gapC > 0 ? static_cast<float>(gapS) / static_cast<float>(gapC) : 0.0f;

    LOG("[VkReinject] framegen: captured=%llu/s dropped=%llu/s generated=%llu/s presentPerGen=%d lastPair=(%llu,%llu) gapAvg=%.2f",
        static_cast<unsigned long long>(cap),
        static_cast<unsigned long long>(drop),
        static_cast<unsigned long long>(gen),
        g_cfgMultiplier,
        static_cast<unsigned long long>(prevSeq),
        static_cast<unsigned long long>(curSeq),
        gapAvg);
}

static int64_t nowNsCapture() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void framegen_thread_loop() {
    LOG("[FgThread] started");
    uint64_t prevProcessedSeq = 0;
    static int64_t s_cyGenSum = 0, s_cySnapSum = 0, s_cyPresSum = 0, s_cyCycleSum = 0, s_cyLockSum = 0;
    static int s_cyCount = 0;
    while (true) {
        FgRequest req;
        {
            std::unique_lock<std::mutex> lk(g_fgMutex);
            g_fgCv.wait(lk, [] {
                return g_fgPendingValid || !g_fgRunning.load(std::memory_order_relaxed);
            });
            if (!g_fgRunning.load(std::memory_order_relaxed) && !g_fgPendingValid)
                break;
            req = g_fgPending;
            g_fgPendingValid = false;
        }

        if (!g_vk_gen_ready.load() || (g_ctx < 0 && g_cfgMultiplier > 1)) {
            continue;
        }

        int64_t tCycleStart = nowNsCapture();

        g_instr.lastPrevSeq.store(prevProcessedSeq, std::memory_order_relaxed);
        g_instr.lastCurSeq.store(req.seq, std::memory_order_relaxed);
        if (prevProcessedSeq > 0) {
            uint64_t gap = req.seq - prevProcessedSeq;
            g_instr.gapSum.fetch_add(gap, std::memory_order_relaxed);
            g_instr.gapCount.fetch_add(1, std::memory_order_relaxed);
        }
        prevProcessedSeq = req.seq;

        int64_t tLockBefore = nowNsCapture();
        {
            std::lock_guard<std::mutex> blk(g_fgBufMutex);
            int64_t tLockAfter = nowNsCapture();
            int64_t tGenStart = tLockAfter;

            const int numInterp = g_cfgMultiplier - 1;

            if (numInterp > 0) {
                seifg::presentContext(g_ctx, -1, {});
                seifg::waitIdle();
            }
            int64_t tGenEnd = nowNsCapture();

            bool snapGpu = g_ahbImportAvailable && g_ahbImagesImported;
            if (snapGpu) {
                auto realResetCmd = reinterpret_cast<PFN_vkResetCommandBuffer>(resolve_real(g_vkDevice, "vkResetCommandBuffer"));
                auto realBeginCmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(resolve_real(g_vkDevice, "vkBeginCommandBuffer"));
                auto realEndCmd = reinterpret_cast<PFN_vkEndCommandBuffer>(resolve_real(g_vkDevice, "vkEndCommandBuffer"));
                auto realCmdBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(resolve_real(g_vkDevice, "vkCmdPipelineBarrier"));
                auto realCmdCopy = reinterpret_cast<PFN_vkCmdCopyImage>(resolve_real(g_vkDevice, "vkCmdCopyImage"));
                auto realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(resolve_real(g_vkDevice, "vkQueueSubmit"));
                auto realWaitFences = reinterpret_cast<PFN_vkWaitForFences>(resolve_real(g_vkDevice, "vkWaitForFences"));
                auto realResetFences = reinterpret_cast<PFN_vkResetFences>(resolve_real(g_vkDevice, "vkResetFences"));

                if (realResetCmd && realBeginCmd && realEndCmd && realCmdBarrier && realCmdCopy &&
                    realSubmit && realWaitFences && realResetFences) {
                    realResetCmd(g_gpuSnapCmd, 0);
                    VkCommandBufferBeginInfo bi{};
                    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                    realBeginCmd(g_gpuSnapCmd, &bi);

                    VkImageMemoryBarrier bars[2]{};
                    bars[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    bars[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                    bars[0].srcAccessMask = 0;
                    bars[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                    bars[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bars[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bars[0].image = g_ahbIn1Img;
                    bars[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

                    bars[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                    bars[1].srcAccessMask = 0;
                    bars[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                    bars[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bars[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    bars[1].image = g_ahbRealImg;
                    bars[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

                    realCmdBarrier(g_gpuSnapCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, bars);

                    VkImageCopy region{};
                    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                    region.extent = {static_cast<uint32_t>(g_vkW), static_cast<uint32_t>(g_vkH), 1};
                    realCmdCopy(g_gpuSnapCmd, g_ahbIn1Img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                g_ahbRealImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

                    realEndCmd(g_gpuSnapCmd);

                    VkSubmitInfo si{};
                    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                    si.commandBufferCount = 1;
                    si.pCommandBuffers = &g_gpuSnapCmd;
                    realSubmit(g_vkQueue, 1, &si, g_gpuSnapFence);
                    realWaitFences(g_vkDevice, 1, &g_gpuSnapFence, VK_TRUE, UINT64_MAX);
                    realResetFences(g_vkDevice, 1, &g_gpuSnapFence);
                } else {
                    snapGpu = false;
                }
            }

            if (!snapGpu) {
                AHardwareBuffer_Desc dSrc{}, dDst{};
                AHardwareBuffer_describe(g_ahbIn1, &dSrc);
                AHardwareBuffer_describe(g_ahbReal, &dDst);
                void* src = nullptr;
                void* dst = nullptr;
                AHardwareBuffer_lock(g_ahbIn1, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &src);
                AHardwareBuffer_lock(g_ahbReal, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &dst);
                if (src && dst) {
                    uint32_t rowBytes = static_cast<uint32_t>(g_vkW) * 4;
                    for (int y = 0; y < g_vkH; ++y) {
                        memcpy(static_cast<uint8_t*>(dst) + static_cast<size_t>(y) * dDst.stride * 4,
                               static_cast<uint8_t*>(src) + static_cast<size_t>(y) * dSrc.stride * 4,
                               rowBytes);
                    }
                }
                AHardwareBuffer_unlock(g_ahbIn1, nullptr);
                AHardwareBuffer_unlock(g_ahbReal, nullptr);
            }

            int64_t tSnapEnd = nowNsCapture();

            if (g_triggerPath[0] && access(g_triggerPath, F_OK) == 0) {
                dump_ahb(g_ahbIn0, "seifg_in0.png", false);
                dump_ahb(g_ahbIn1, "seifg_in1.png", false);
                dump_ahb(g_ahbOut, "seifg_out.png", false);
                LOG("[FgThread] triggered dump");
                unlink(g_triggerPath);
            }

            s_cyGenSum += tGenEnd - tGenStart;
            s_cySnapSum += tSnapEnd - tGenEnd;
            s_cyLockSum += tLockAfter - tLockBefore;
        }

        g_instr.generated.fetch_add(1, std::memory_order_relaxed);

        int64_t tPresStart = nowNsCapture();
        const int numInterp = g_cfgMultiplier - 1;
        const int64_t period = g_scPresenter.clock().periodNs();
        if (period <= 0) {
            g_scPresenter.presentOne(g_ahbReal, 0);
            int64_t tPresEnd = nowNsCapture();
            s_cyPresSum += tPresEnd - tPresStart;
            s_cyCycleSum += tPresEnd - tCycleStart;
            s_cyCount++;
            if (s_cyCount >= 60) {
                LOG("[FgCycle] gen=%.1f snap=%.1f present=%.1f lock=%.1f cycle=%.1f ms (n=60)",
                    (double)s_cyGenSum / 60.0 / 1e6, (double)s_cySnapSum / 60.0 / 1e6,
                    (double)s_cyPresSum / 60.0 / 1e6, (double)s_cyLockSum / 60.0 / 1e6,
                    (double)s_cyCycleSum / 60.0 / 1e6);
                s_cyGenSum = s_cySnapSum = s_cyPresSum = s_cyCycleSum = s_cyLockSum = 0;
                s_cyCount = 0;
            }
            framegen_instr_tick();
            continue;
        }

        recomputePacing();

        int64_t spacing = g_presentSpacingNs.load(std::memory_order_relaxed);
        if (spacing <= 0) spacing = period;

        int64_t base = g_scPresenter.nextVsyncSlot();
        int m = g_cfgMultiplier;

        for (int i = 0; i < numInterp; ++i) {
            g_scPresenter.presentOne(g_ahbOutN[i], base + static_cast<int64_t>(i) * spacing);
        }
        g_scPresenter.presentOne(g_ahbReal, base + static_cast<int64_t>(numInterp) * spacing);

        g_scPresenter.commitBurst(base + static_cast<int64_t>(m) * spacing - period);

        int64_t tPresEnd = nowNsCapture();
        s_cyPresSum += tPresEnd - tPresStart;
        s_cyCycleSum += tPresEnd - tCycleStart;
        s_cyCount++;
        if (s_cyCount >= 60) {
            LOG("[FgCycle] gen=%.1f snap=%.1f present=%.1f lock=%.1f cycle=%.1f ms (n=60)",
                (double)s_cyGenSum / 60.0 / 1e6, (double)s_cySnapSum / 60.0 / 1e6,
                (double)s_cyPresSum / 60.0 / 1e6, (double)s_cyLockSum / 60.0 / 1e6,
                (double)s_cyCycleSum / 60.0 / 1e6);
            s_cyGenSum = s_cySnapSum = s_cyPresSum = s_cyCycleSum = s_cyLockSum = 0;
            s_cyCount = 0;
        }

        framegen_instr_tick();
    }
    LOG("[FgThread] exiting");
}

static void start_framegen_thread() {
    if (g_fgRunning.load(std::memory_order_relaxed)) return;
    g_fgRunning.store(true, std::memory_order_relaxed);
    g_fgThread = std::thread(framegen_thread_loop);
}

static void stop_framegen_thread() {
    if (g_fgRunning.exchange(false)) {
        std::lock_guard<std::mutex> lk(g_fgMutex);
        g_fgCv.notify_all();
    }
    if (g_fgThread.joinable()) g_fgThread.join();
}

static void vk_capture_present(const VkPresentInfoKHR* pi) {
    static std::atomic<uint64_t> vkFrame{0};
    uint64_t n = vkFrame.fetch_add(1);

    SwapchainInfo info;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_swapchains.find(pi->pSwapchains[0]);
        if (it == g_swapchains.end()) return;
        info = it->second;
    }

    VkImage img = info.images[pi->pImageIndices[0]];

    {
        AHardwareBuffer_Desc d0{}, d1{};
        AHardwareBuffer_describe(g_ahbIn0, &d0);
        AHardwareBuffer_describe(g_ahbIn1, &d1);
        void* base0 = nullptr;
        void* base1 = nullptr;
        AHardwareBuffer_lock(g_ahbIn1, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &base1);
        AHardwareBuffer_lock(g_ahbIn0, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &base0);
        if (base0 && base1) {
            uint32_t copyW = static_cast<uint32_t>(g_vkW) * 4;
            for (int y = 0; y < g_vkH; ++y) {
                memcpy(static_cast<uint8_t*>(base0) + static_cast<size_t>(y) * d0.stride * 4,
                       static_cast<uint8_t*>(base1) + static_cast<size_t>(y) * d1.stride * 4,
                       copyW);
            }
        }
        AHardwareBuffer_unlock(g_ahbIn0, nullptr);
        AHardwareBuffer_unlock(g_ahbIn1, nullptr);
    }

    vkResetCommandBuffer(g_vkCmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_vkCmd, &beginInfo);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_vkCmd,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {static_cast<uint32_t>(g_vkW), static_cast<uint32_t>(g_vkH), 1};
    vkCmdCopyImageToBuffer(g_vkCmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           g_vkStaging, 1, &region);

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = 0;
    vkCmdPipelineBarrier(g_vkCmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    vkEndCommandBuffer(g_vkCmd);

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_vkCmd;
    VkResult r = vkQueueSubmit(g_vkQueue, 1, &submit, g_vkFence);
    if (r != VK_SUCCESS) { LOG("[VkGen] queueSubmit failed %d", r); return; }

    vkWaitForFences(g_vkDevice, 1, &g_vkFence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_vkDevice, 1, &g_vkFence);

    void* mapped = nullptr;
    r = vkMapMemory(g_vkDevice, g_vkStagingMem, 0, g_vkStagingSize, 0, &mapped);
    if (r != VK_SUCCESS || !mapped) { LOG("[VkGen] mapMemory failed %d", r); return; }

    {
        AHardwareBuffer_Desc dd{};
        AHardwareBuffer_describe(g_ahbIn1, &dd);
        void* ahbBase = nullptr;
        AHardwareBuffer_lock(g_ahbIn1, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr, &ahbBase);
        if (ahbBase) {
            for (int y = 0; y < g_vkH; ++y) {
                memcpy(static_cast<uint8_t*>(ahbBase) + static_cast<size_t>(y) * dd.stride * 4,
                       static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * g_vkW * 4,
                       static_cast<size_t>(g_vkW) * 4);
            }
        }
        AHardwareBuffer_unlock(g_ahbIn1, nullptr);
    }
    vkUnmapMemory(g_vkDevice, g_vkStagingMem);

    if (g_triggerPath[0] && access(g_triggerPath, F_OK) == 0) {
        seifg::presentContext(g_ctx, -1, {});
        seifg::waitIdle();
        dump_ahb(g_ahbIn0, "seifg_in0.png", false);
        dump_ahb(g_ahbIn1, "seifg_in1.png", false);
        dump_ahb(g_ahbOut, "seifg_out.png", false);
        LOG("[VkGen] triggered dump");
        unlink(g_triggerPath);
    }

    if (n < 3 || n % 300 == 0) {
        LOG("[VkGen] capture running frame=%llu %dx%d fmt=%d",
            static_cast<unsigned long long>(n), g_vkW, g_vkH, static_cast<int>(g_vkFmt));
    }
}

static VkResult VKAPI_CALL hooked_GetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages) {

    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end() && it->second.virtualized) {
            uint32_t count = static_cast<uint32_t>(it->second.virtualImages.size());
            if (!pSwapchainImages) {
                *pSwapchainImageCount = count;
                return VK_SUCCESS;
            }
            uint32_t toWrite = *pSwapchainImageCount < count ? *pSwapchainImageCount : count;
            for (uint32_t i = 0; i < toWrite; ++i)
                pSwapchainImages[i] = it->second.virtualImages[i];
            *pSwapchainImageCount = toWrite;
            return toWrite < count ? VK_INCOMPLETE : VK_SUCCESS;
        }
    }

    if (!g_real_get_swapchain_images)
        g_real_get_swapchain_images = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolve_real(device, "vkGetSwapchainImagesKHR"));
    if (!g_real_get_swapchain_images) return VK_ERROR_INITIALIZATION_FAILED;
    return g_real_get_swapchain_images(device, swapchain, pSwapchainImageCount, pSwapchainImages);
}

static void acquire_rate_cap() {
    int64_t P = g_scPresenter.clock().periodNs();
    if (P <= 0) return;

    recomputePacing();

    int64_t intervalNs = g_renderIntervalNs.load(std::memory_order_relaxed);
    if (intervalNs <= 0) return;

    int64_t now = nowNsCapture();

    if (g_nextAcquireNs == 0) {
        int64_t lastVsync = g_scPresenter.clock().lastVsyncNs();
        if (lastVsync > 0) {
            int64_t slots = (now - lastVsync + P - 1) / P;
            g_nextAcquireNs = lastVsync + slots * P;
        } else {
            g_nextAcquireNs = now;
        }
        return;
    }

    if (now < g_nextAcquireNs) {
        struct timespec ts;
        ts.tv_sec = g_nextAcquireNs / 1000000000LL;
        ts.tv_nsec = g_nextAcquireNs % 1000000000LL;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
        now = nowNsCapture();
    }

    g_nextAcquireNs += intervalNs;
    if (g_nextAcquireNs < now)
        g_nextAcquireNs = now + intervalNs;
}

static VkResult VKAPI_CALL hooked_AcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex) {

    bool isVirt = false;
    uint32_t idx = 0;
    uint32_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end() && it->second.virtualized) {
            isVirt = true;
            count = static_cast<uint32_t>(it->second.virtualImages.size());
            idx = it->second.nextVirtualIdx;
            it->second.nextVirtualIdx = (idx + 1) % count;
        }
    }

    if (!isVirt) {
        if (!g_real_acquire)
            g_real_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(resolve_real(device, "vkAcquireNextImageKHR"));
        if (!g_real_acquire) return VK_ERROR_INITIALIZATION_FAILED;
        return g_real_acquire(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }

    acquire_rate_cap();

    *pImageIndex = idx;

    if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (semaphore != VK_NULL_HANDLE) {
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &semaphore;
        }
        vkQueueSubmit(g_vkQueue, 1, &submit, fence);
    }

    return VK_SUCCESS;
}

static VkResult VKAPI_CALL hooked_AcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex) {

    bool isVirt = false;
    uint32_t idx = 0;
    uint32_t count = 0;
    VkSwapchainKHR swapchain = pAcquireInfo->swapchain;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end() && it->second.virtualized) {
            isVirt = true;
            count = static_cast<uint32_t>(it->second.virtualImages.size());
            idx = it->second.nextVirtualIdx;
            it->second.nextVirtualIdx = (idx + 1) % count;
        }
    }

    if (!isVirt) {
        if (!g_real_acquire2)
            g_real_acquire2 = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(resolve_real(device, "vkAcquireNextImage2KHR"));
        if (!g_real_acquire2) return VK_ERROR_INITIALIZATION_FAILED;
        return g_real_acquire2(device, pAcquireInfo, pImageIndex);
    }

    acquire_rate_cap();

    *pImageIndex = idx;

    VkSemaphore semaphore = pAcquireInfo->semaphore;
    VkFence fence = pAcquireInfo->fence;

    if (semaphore != VK_NULL_HANDLE || fence != VK_NULL_HANDLE) {
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        if (semaphore != VK_NULL_HANDLE) {
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &semaphore;
        }
        vkQueueSubmit(g_vkQueue, 1, &submit, fence);
    }

    return VK_SUCCESS;
}

static VkResult VKAPI_CALL hooked_QueuePresentKHR(
    VkQueue q,
    const VkPresentInfoKHR* pi) {

    if (!g_real_present) {
        static std::atomic_flag warned = ATOMIC_FLAG_INIT;
        if (!warned.test_and_set())
            ERROR("[VkGen] hooked_QueuePresentKHR called but g_real_present is null");
        return VK_SUCCESS;
    }

    static std::atomic_flag present_first = ATOMIC_FLAG_INIT;
    if (!present_first.test_and_set()) {
        LOG("[VkGen] present intercepted");
    }

    static std::atomic<uint64_t> frame{0};
    uint64_t n = frame.fetch_add(1);

    bool isVirt = false;
    SwapchainInfo scInfo{};
    if (pi->swapchainCount > 0) {
        std::lock_guard<std::mutex> lock(g_mu);
        auto it = g_swapchains.find(pi->pSwapchains[0]);
        if (it != g_swapchains.end()) {
            scInfo = it->second;
            isVirt = it->second.virtualized;
        }
    }

    if (isVirt) {
        static uint64_t s_virtPairCount = 0;
        uint32_t virtIdx = pi->pImageIndices[0];

        if (!g_real_acquire)
            g_real_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(resolve_real(scInfo.device, "vkAcquireNextImageKHR"));

        if (g_seifg_inited.load()) {
            if (!vk_virtualized_capture(scInfo.virtualImages[virtIdx], scInfo)) {
                g_instr.dropped.fetch_add(1, std::memory_order_relaxed);
                return VK_SUCCESS;
            }
            g_instr.captured.fetch_add(1, std::memory_order_relaxed);
        }

        bool havePrev = (g_captureCount.load() >= 2);

        {
            std::lock_guard<std::mutex> lk(g_scMu);
            if (!g_scPresenter.ready()) {
                auto wit = g_swapchainWindowMap.find(pi->pSwapchains[0]);
                if (wit != g_swapchainWindowMap.end() && wit->second) {
                    g_scPresenter.init(wit->second);
                    g_scPresenter.setBufferTransform(g_scBufTransform);
                    LOG("[VkSC] presenter initialized transform=%d", g_scBufTransform);
                }
            }
        }

        if (havePrev && g_vk_gen_ready.load() && (g_ctx >= 0 || g_cfgMultiplier == 1) && g_scPresenter.ready()) {
            if (!g_fgRunning.load(std::memory_order_relaxed))
                start_framegen_thread();

            {
                std::lock_guard<std::mutex> lk(g_fgMutex);
                uint64_t seq = g_fgSeq.fetch_add(1, std::memory_order_relaxed) + 1;
                g_fgPending = FgRequest{g_captureCount.load(), seq};
                g_fgPendingValid = true;
            }
            g_fgCv.notify_one();

            return VK_SUCCESS;
        }

        if (g_scPresenter.ready()) {
            g_scPresenter.presentOne(g_ahbIn1, g_scPresenter.nextVsyncSlot());
            if (n < 3 || n % 300 == 0)
                LOG("[VkSC] first-frame passthrough #%llu", static_cast<unsigned long long>(n));
            return VK_SUCCESS;
        }

        uint32_t realIdx = 0;
        VkResult acqRes = g_real_acquire(scInfo.device, scInfo.realSwapchain, UINT64_MAX,
                                         scInfo.realAcquireSem, VK_NULL_HANDLE, &realIdx);
        if (acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR) {
            ERROR("[VkReinject] real acquire failed %d", acqRes);
            return acqRes;
        }

        vkResetCommandBuffer(scInfo.blitCmd, 0);
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(scInfo.blitCmd, &beginInfo);

        VkImageMemoryBarrier barriers[2]{};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[0].image = scInfo.virtualImages[virtIdx];
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[1].image = scInfo.realImages[realIdx];
        barriers[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(scInfo.blitCmd,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 2, barriers);

        VkImageBlit region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.srcOffsets[0] = {0, 0, 0};
        region.srcOffsets[1] = {static_cast<int32_t>(scInfo.extent.width),
                                static_cast<int32_t>(scInfo.extent.height), 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstOffsets[0] = {0, 0, 0};
        region.dstOffsets[1] = {static_cast<int32_t>(scInfo.extent.width),
                                static_cast<int32_t>(scInfo.extent.height), 1};
        vkCmdBlitImage(scInfo.blitCmd,
                       scInfo.virtualImages[virtIdx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       scInfo.realImages[realIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region, VK_FILTER_NEAREST);

        VkImageMemoryBarrier postBar{};
        postBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        postBar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        postBar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        postBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        postBar.dstAccessMask = 0;
        postBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        postBar.image = scInfo.realImages[realIdx];
        postBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        vkCmdPipelineBarrier(scInfo.blitCmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &postBar);

        vkEndCommandBuffer(scInfo.blitCmd);

        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &scInfo.realAcquireSem;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &scInfo.blitCmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &scInfo.blitDoneSem;
        vkQueueSubmit(g_vkQueue, 1, &submit, scInfo.blitFence);

        VkPresentInfoKHR realPi{};
        realPi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        realPi.waitSemaphoreCount = 1;
        realPi.pWaitSemaphores = &scInfo.blitDoneSem;
        realPi.swapchainCount = 1;
        realPi.pSwapchains = &scInfo.realSwapchain;
        realPi.pImageIndices = &realIdx;
        VkResult presRes = g_real_present(q, &realPi);

        vkWaitForFences(scInfo.device, 1, &scInfo.blitFence, VK_TRUE, UINT64_MAX);
        vkResetFences(scInfo.device, 1, &scInfo.blitFence);

        if (n < 3 || n % 300 == 0) {
            LOG("[VkReinject] first-frame passthrough #%llu",
                static_cast<unsigned long long>(n));
        }

        return presRes;
    }

    if (n < 3 || n % 300 == 0) {
        LOG("[VkCapture] present #%llu swapchains=%u %ux%u",
            static_cast<unsigned long long>(n), pi->swapchainCount,
            scInfo.extent.width, scInfo.extent.height);
    }

    if (g_seifg_inited.load() && g_vkDevice) {
        if (!g_vk_gen_ready.load()) vk_capture_setup();
        if (g_vk_gen_ready.load() && pi->swapchainCount > 0) vk_capture_present(pi);
    }

    return g_real_present(q, pi);
}

void SetConfig(int fps, int multiplier, int quality) {
    g_cfgFps = fps;
    g_cfgMultiplier = (multiplier < 1) ? 1 : (multiplier > 3) ? 3 : multiplier;
    g_cfgQuality = (quality < 0) ? 0 : (quality > 2) ? 2 : quality;
    LOG("[VkCapture] build=%s %s config fps=%d multiplier=%d quality=%d", __DATE__, __TIME__, g_cfgFps, g_cfgMultiplier, g_cfgQuality);
}

void SetTargetFps(int targetFps) {
    g_cfgTargetFps = targetFps;
    recomputePacing();
    LOG("[VkCapture] targetFps=%d", g_cfgTargetFps);
}

void Install() {
    static std::atomic_flag done = ATOMIC_FLAG_INIT;
    if (done.test_and_set()) return;

    if (!load_shadowhook()) return;

    int init_ret = p_sh_init(SHADOWHOOK_MODE_UNIQUE, false);
    LOG("[VkCapture] shadowhook_init returned %d", init_ret);

    void* stub_gipa = p_sh_hook_sym_name(
        "libvulkan.so", "vkGetInstanceProcAddr",
        reinterpret_cast<void*>(my_GetInstanceProcAddr),
        reinterpret_cast<void**>(&g_real_gipa));
    if (stub_gipa) {
        LOG("[VkCapture] hooked vkGetInstanceProcAddr stub=%p (GIPA interception active)", stub_gipa);
    } else {
        ERROR("[VkCapture] hook vkGetInstanceProcAddr failed: %s",
              p_sh_to_errmsg(p_sh_get_errno()));
    }

    void* stub_egl = p_sh_hook_sym_name(
        "libEGL.so", "eglSwapBuffers",
        reinterpret_cast<void*>(hooked_eglSwapBuffers),
        reinterpret_cast<void**>(&g_orig_egl_swap));
    if (stub_egl) {
        LOG("[GlCapture] hooked eglSwapBuffers stub=%p", stub_egl);
    } else {
        ERROR("[GlCapture] hook eglSwapBuffers failed: %s",
              p_sh_to_errmsg(p_sh_get_errno()));
    }

    std::thread([]() {
        seifg::initialize(0, false, static_cast<uint32_t>(g_cfgQuality),
                          static_cast<uint64_t>(g_cfgMultiplier), {});
        int level = 3 - g_cfgQuality;
        if (level < 1) level = 1;
        if (level > 3) level = 3;
        seifg::setFlowDownscale(static_cast<uint32_t>(level));
        LOG("[seifg] initialize done quality=%d multiplier=%d upscaleOnlyLevels=%d device=%p phys=%p",
            g_cfgQuality, g_cfgMultiplier, level,
            reinterpret_cast<void*>(seifg::getDevice()),
            reinterpret_cast<void*>(seifg::getPhysicalDevice()));
        g_seifg_inited = true;
    }).detach();
}

}
