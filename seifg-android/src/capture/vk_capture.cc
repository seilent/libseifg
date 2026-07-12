#include "capture/vk_capture.hh"

#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <unordered_map>
#include <mutex>
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
    g_sh_handle = dlopen("libshadowhook.so", RTLD_NOW | RTLD_GLOBAL);
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
static GLuint g_texIn0 = 0, g_fboIn0 = 0, g_texIn1 = 0, g_fboIn1 = 0;
static GLuint g_texOut = 0, g_fboOut = 0;
static int g_genW = 0, g_genH = 0;
static char g_triggerPath[512] = {0};
static char g_reinjectPath[512] = {0};
static bool g_reinject = false;

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

static std::atomic<uint64_t> g_captureCount{0};

static int g_cfgFps = 0;
static int g_cfgMultiplier = 2;
static int g_cfgQuality = 2;

static constexpr int kMaxOutputs = 2;
static AHardwareBuffer* g_ahbOutN[kMaxOutputs] = {};
static GLuint g_texOutN[kMaxOutputs] = {};
static GLuint g_fboOutN[kMaxOutputs] = {};

static VkBuffer g_vkOutStaging = VK_NULL_HANDLE;
static VkDeviceMemory g_vkOutStagingMem = VK_NULL_HANDLE;
static VkSemaphore g_reinjectSem = VK_NULL_HANDLE;
static VkCommandPool g_reinjectPool = VK_NULL_HANDLE;
static VkCommandBuffer g_reinjectCmd = VK_NULL_HANDLE;
static VkFence g_reinjectFence = VK_NULL_HANDLE;
static bool g_reinjectResAlloc = false;

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

static PFN_vkGetSwapchainImagesKHR g_real_get_swapchain_images = nullptr;
static PFN_vkAcquireNextImageKHR g_real_acquire = nullptr;
static PFN_vkAcquireNextImage2KHR g_real_acquire2 = nullptr;

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
    g_ahbOut = g_ahbOutN[0];
    if (!g_ahbIn0 || !g_ahbIn1 || !g_ahbOut) { LOG("[Gen] ahb alloc failed"); return; }

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
    g_texOut = g_texOutN[0];
    g_fboOut = g_fboOutN[0];
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (!ok0 || !ok1 || !okOut) { LOG("[Gen] fbo setup failed"); return; }

    g_genW = w;
    g_genH = h;

    std::vector<AHardwareBuffer*> outVec(numOut);
    for (int i = 0; i < numOut; ++i)
        outVec[i] = g_ahbOutN[i];
    g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, outVec,
                                        VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)},
                                        VK_FORMAT_R8G8B8A8_UNORM);

    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) { size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf); (void)rd; fclose(cf); }
    snprintf(g_triggerPath, sizeof(g_triggerPath), "/sdcard/Android/data/%s/files/seifg_trigger", pkg);
    snprintf(g_reinjectPath, sizeof(g_reinjectPath), "/sdcard/Android/data/%s/files/seifg_reinject", pkg);

    LOG("[Gen] setup ctx=%d %dx%d trigger=%s", g_ctx, w, h, g_triggerPath);
    if (g_ctx >= 0) g_gen_ready = true;
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
        seifg::presentContext(g_ctx, -1, {});
        seifg::waitIdle();
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

    seifg::presentContext(g_ctx, -1, {});
    seifg::waitIdle();

    int numInterp = g_cfgMultiplier - 1;
    for (int i = 0; i < numInterp; ++i) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fboOutN[i]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        g_orig_egl_swap(dpy, surf);
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
            if ((n % 30) == 0 && g_reinjectPath[0]) {
                g_reinject = (access(g_reinjectPath, F_OK) == 0);
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

    VkResult result = g_real_create_device(phys, ci, a, pDev);
    if (result == VK_SUCCESS && pDev) {
        g_vkPhys = phys;
        g_vkDevice = *pDev;
        if (ci->queueCreateInfoCount > 0) {
            g_vkFamily = ci->pQueueCreateInfos[0].queueFamilyIndex;
        }
        vkGetDeviceQueue(g_vkDevice, g_vkFamily, 0, &g_vkQueue);

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
    snprintf(reinjectFlag, sizeof(reinjectFlag), "/sdcard/Android/data/%s/files/seifg_reinject", pkg);
    bool doVirtualize = (access(reinjectFlag, F_OK) == 0);

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

    VkSwapchainCreateInfoKHR modCi = *ci;
    modCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    modCi.presentMode = VK_PRESENT_MODE_FIFO_KHR;

    VkResult result = g_real_create_swapchain(device, &modCi, a, pSc);
    if (result != VK_SUCCESS || !pSc) return result;

    VkSwapchainKHR realSc = *pSc;

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
    return VK_SUCCESS;
}

static void vk_capture_setup();

static void alloc_reinject_resources(VkDevice device) {
    if (g_reinjectResAlloc) return;

    VkDeviceSize sz = static_cast<VkDeviceSize>(g_vkW) * g_vkH * 4;
    VkBufferCreateInfo bufCi{};
    bufCi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCi.size = sz;
    bufCi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufCi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = vkCreateBuffer(device, &bufCi, nullptr, &g_vkOutStaging);
    if (r != VK_SUCCESS) { ERROR("[VkReinject] createBuffer out failed %d", r); return; }

    VkMemoryRequirements memReqs;
    vkGetBufferMemoryRequirements(device, g_vkOutStaging, &memReqs);

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
    if (memIdx == UINT32_MAX) { ERROR("[VkReinject] no host-visible memory for out staging"); return; }

    VkMemoryAllocateInfo memAi{};
    memAi.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memAi.allocationSize = memReqs.size;
    memAi.memoryTypeIndex = memIdx;
    r = vkAllocateMemory(device, &memAi, nullptr, &g_vkOutStagingMem);
    if (r != VK_SUCCESS) { ERROR("[VkReinject] allocMemory out failed %d", r); return; }
    vkBindBufferMemory(device, g_vkOutStaging, g_vkOutStagingMem, 0);

    VkSemaphoreCreateInfo semCi{};
    semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    vkCreateSemaphore(device, &semCi, nullptr, &g_reinjectSem);

    VkCommandPoolCreateInfo poolCi{};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.queueFamilyIndex = g_vkFamily;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device, &poolCi, nullptr, &g_reinjectPool);

    VkCommandBufferAllocateInfo cmdAi{};
    cmdAi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAi.commandPool = g_reinjectPool;
    cmdAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAi.commandBufferCount = 1;
    vkAllocateCommandBuffers(device, &cmdAi, &g_reinjectCmd);

    VkFenceCreateInfo fenceCi{};
    fenceCi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(device, &fenceCi, nullptr, &g_reinjectFence);

    g_reinjectResAlloc = true;
    LOG("[VkReinject] reinject resources allocated %dx%d", g_vkW, g_vkH);
}

static void vk_virtualized_capture(VkImage virtImg, const SwapchainInfo& scInfo) {
    if (!g_vk_gen_ready.load()) {
        g_vkW = static_cast<int>(scInfo.extent.width);
        g_vkH = static_cast<int>(scInfo.extent.height);
        g_vkFmt = scInfo.format;
        vk_capture_setup();
        if (!g_vk_gen_ready.load()) return;
    }

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

    if (g_triggerPath[0] && access(g_triggerPath, F_OK) == 0) {
        dump_ahb(g_ahbIn0, "seifg_in0.png", false);
        dump_ahb(g_ahbIn1, "seifg_in1.png", false);
        dump_ahb(g_ahbOut, "seifg_out.png", false);
        LOG("[VkReinject] triggered dump at capture %llu",
            static_cast<unsigned long long>(g_captureCount.load()));
        unlink(g_triggerPath);
    }
}

static VkResult vk_reinject_present_interpolated(VkQueue q, const SwapchainInfo& scInfo, AHardwareBuffer* ahbSrc) {
    if (!g_reinjectResAlloc) alloc_reinject_resources(scInfo.device);
    if (!g_reinjectResAlloc) return VK_ERROR_INITIALIZATION_FAILED;

    void* ahbBase = nullptr;
    AHardwareBuffer_Desc dd{};
    AHardwareBuffer_describe(ahbSrc, &dd);
    int lr = AHardwareBuffer_lock(ahbSrc, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &ahbBase);
    if (lr != 0 || !ahbBase) return VK_ERROR_UNKNOWN;

    void* mapped = nullptr;
    vkMapMemory(scInfo.device, g_vkOutStagingMem, 0,
                static_cast<VkDeviceSize>(g_vkW) * g_vkH * 4, 0, &mapped);
    if (mapped) {
        for (int y = 0; y < g_vkH; ++y) {
            memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * g_vkW * 4,
                   static_cast<uint8_t*>(ahbBase) + static_cast<size_t>(y) * dd.stride * 4,
                   static_cast<size_t>(g_vkW) * 4);
        }
        vkUnmapMemory(scInfo.device, g_vkOutStagingMem);
    }
    AHardwareBuffer_unlock(g_ahbOut, nullptr);

    if (!mapped) return VK_ERROR_UNKNOWN;

    uint32_t realIdx = 0;
    VkResult acqRes = g_real_acquire(scInfo.device, scInfo.realSwapchain, UINT64_MAX,
                                     scInfo.realAcquireSem, VK_NULL_HANDLE, &realIdx);
    if (acqRes != VK_SUCCESS && acqRes != VK_SUBOPTIMAL_KHR) return acqRes;

    vkResetCommandBuffer(g_reinjectCmd, 0);
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_reinjectCmd, &beginInfo);

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = scInfo.realImages[realIdx];
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_reinjectCmd,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {static_cast<uint32_t>(g_vkW), static_cast<uint32_t>(g_vkH), 1};
    vkCmdCopyBufferToImage(g_reinjectCmd, g_vkOutStaging, scInfo.realImages[realIdx],
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = 0;
    vkCmdPipelineBarrier(g_reinjectCmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &bar);

    vkEndCommandBuffer(g_reinjectCmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &scInfo.realAcquireSem;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_reinjectCmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &g_reinjectSem;
    vkQueueSubmit(g_vkQueue, 1, &submit, g_reinjectFence);

    vkWaitForFences(scInfo.device, 1, &g_reinjectFence, VK_TRUE, UINT64_MAX);
    vkResetFences(scInfo.device, 1, &g_reinjectFence);

    VkPresentInfoKHR realPi{};
    realPi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    realPi.waitSemaphoreCount = 1;
    realPi.pWaitSemaphores = &g_reinjectSem;
    realPi.swapchainCount = 1;
    realPi.pSwapchains = &scInfo.realSwapchain;
    realPi.pImageIndices = &realIdx;
    return g_real_present(q, &realPi);
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
        int numOut = g_cfgMultiplier - 1;
        for (int i = 0; i < numOut; ++i)
            g_ahbOutN[i] = alloc_ahb(g_vkW, g_vkH);
        g_ahbOut = g_ahbOutN[0];
        if (!g_ahbIn0 || !g_ahbIn1 || !g_ahbOut) { LOG("[VkGen] ahb alloc failed"); return; }
    }

    int numOut = g_cfgMultiplier - 1;
    std::vector<AHardwareBuffer*> outVec(numOut);
    for (int i = 0; i < numOut; ++i)
        outVec[i] = g_ahbOutN[i];

    g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, outVec,
                                        VkExtent2D{extent.width, extent.height},
                                        VK_FORMAT_R8G8B8A8_UNORM);

    if (g_triggerPath[0] == '\0') {
        char pkg[256] = {0};
        FILE* cf = fopen("/proc/self/cmdline", "r");
        if (cf) { size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf); (void)rd; fclose(cf); }
        snprintf(g_triggerPath, sizeof(g_triggerPath), "/sdcard/Android/data/%s/files/seifg_trigger", pkg);
    }

    LOG("[VkGen] setup ctx=%d %dx%d fmt=%d", g_ctx, g_vkW, g_vkH, static_cast<int>(format));
    if (g_ctx >= 0) g_vk_gen_ready = true;
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

        if (g_cfgFps > 0) {
            static struct timespec s_lastPresent = {0, 0};
            if (s_lastPresent.tv_sec == 0 && s_lastPresent.tv_nsec == 0)
                clock_gettime(CLOCK_MONOTONIC, &s_lastPresent);
            int64_t intervalNs = 1000000000LL / g_cfgFps;
            struct timespec target;
            target.tv_sec = s_lastPresent.tv_sec;
            target.tv_nsec = s_lastPresent.tv_nsec + intervalNs;
            if (target.tv_nsec >= 1000000000LL) {
                target.tv_sec += target.tv_nsec / 1000000000LL;
                target.tv_nsec %= 1000000000LL;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &target, nullptr);
            s_lastPresent = target;
        }

        if (!g_real_acquire)
            g_real_acquire = reinterpret_cast<PFN_vkAcquireNextImageKHR>(resolve_real(scInfo.device, "vkAcquireNextImageKHR"));

        if (g_seifg_inited.load()) {
            vk_virtualized_capture(scInfo.virtualImages[virtIdx], scInfo);
        }

        bool havePrev = (g_captureCount.load() >= 2);

        if (havePrev && g_vk_gen_ready.load() && g_ctx >= 0) {
            seifg::presentContext(g_ctx, -1, {});
            seifg::waitIdle();

            if (!g_reinjectResAlloc) alloc_reinject_resources(scInfo.device);
            if (g_reinjectResAlloc) {
                int numInterp = g_cfgMultiplier - 1;
                for (int i = 0; i < numInterp; ++i) {
                    VkResult intRes = vk_reinject_present_interpolated(q, scInfo, g_ahbOutN[i]);
                    if (intRes != VK_SUCCESS && intRes != VK_SUBOPTIMAL_KHR) {
                        ERROR("[VkReinject] interpolated present [%d] failed %d", i, intRes);
                        break;
                    }
                }
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

            s_virtPairCount++;
            if (s_virtPairCount <= 3 || s_virtPairCount % 300 == 0) {
                LOG("[VkReinject] present pair interp+real #%llu",
                    static_cast<unsigned long long>(s_virtPairCount));
            }

            return presRes;
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
    g_cfgMultiplier = (multiplier < 2) ? 2 : (multiplier > 3) ? 3 : multiplier;
    g_cfgQuality = (quality < 0) ? 0 : (quality > 2) ? 2 : quality;
    LOG("[VkCapture] config fps=%d multiplier=%d quality=%d", g_cfgFps, g_cfgMultiplier, g_cfgQuality);
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
        LOG("[seifg] initialize done quality=%d multiplier=%d device=%p phys=%p",
            g_cfgQuality, g_cfgMultiplier,
            reinterpret_cast<void*>(seifg::getDevice()),
            reinterpret_cast<void*>(seifg::getPhysicalDevice()));
        g_seifg_inited = true;
    }).detach();
}

}
