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

struct SwapchainInfo {
    VkDevice device;
    VkFormat format;
    VkExtent2D extent;
    uint32_t imageCount;
    std::vector<VkImage> images;
};

static std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;
static std::mutex g_mu;

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
    g_ahbOut = alloc_ahb(w, h);
    if (!g_ahbIn0 || !g_ahbIn1 || !g_ahbOut) { LOG("[Gen] ahb alloc failed"); return; }

    GLint prevTex = 0, prevFbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    bool ok0 = make_ahb_fbo(dpy, g_ahbIn0, &g_texIn0, &g_fboIn0);
    bool ok1 = make_ahb_fbo(dpy, g_ahbIn1, &g_texIn1, &g_fboIn1);
    bool ok2 = make_ahb_fbo(dpy, g_ahbOut, &g_texOut, &g_fboOut);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (!ok0 || !ok1 || !ok2) { LOG("[Gen] fbo setup failed"); return; }

    g_genW = w;
    g_genH = h;
    g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, {g_ahbOut},
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

    GLint pr = 0, pd = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &pr);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &pd);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_fboIn1);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glFinish();

    seifg::presentContext(g_ctx, -1, {});
    seifg::waitIdle();

    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_fboOut);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, g_genW, g_genH, 0, 0, g_genW, g_genH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    g_orig_egl_swap(dpy, surf);

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
        LOG("[Reinject] frame %llu presented mid+real", static_cast<unsigned long long>(k));
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

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) {
        if (!g_real_present && g_real_gdpa)
            g_real_present = reinterpret_cast<PFN_vkQueuePresentKHR>(g_real_gdpa(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_QueuePresentKHR);
    }
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        if (!g_real_create_swapchain && g_real_gdpa)
            g_real_create_swapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(g_real_gdpa(device, pName));
        return reinterpret_cast<PFN_vkVoidFunction>(hooked_CreateSwapchainKHR);
    }
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(my_GetDeviceProcAddr);
    return g_real_gdpa ? g_real_gdpa(device, pName) : nullptr;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL my_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (!pName) return nullptr;

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

        if (g_real_gipa) {
            VkInstance inst = VK_NULL_HANDLE;
            g_real_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                g_real_gipa(inst, "vkGetDeviceProcAddr"));
        }

        LOG("[VkGen] device recorded dev=%p phys=%p family=%u",
            reinterpret_cast<void*>(g_vkDevice),
            reinterpret_cast<void*>(g_vkPhys),
            g_vkFamily);
    }
    return result;
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

    VkSwapchainCreateInfoKHR modCi = *ci;
    modCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    LOG("[VkCapture] adding TRANSFER_SRC to swapchain usage (was 0x%x now 0x%x)",
        ci->imageUsage, modCi.imageUsage);

    VkResult result = g_real_create_swapchain(device, &modCi, a, pSc);
    if (result == VK_SUCCESS && pSc) {
        uint32_t n = 0;
        vkGetSwapchainImagesKHR(device, *pSc, &n, nullptr);
        std::vector<VkImage> images(n);
        vkGetSwapchainImagesKHR(device, *pSc, &n, images.data());

        std::lock_guard<std::mutex> lock(g_mu);
        g_swapchains[*pSc] = SwapchainInfo{
            device, ci->imageFormat, ci->imageExtent, n, std::move(images)};

        LOG("[VkCapture] swapchain created %ux%u fmt=%d images=%u",
            ci->imageExtent.width, ci->imageExtent.height,
            static_cast<int>(ci->imageFormat), n);
    }
    return result;
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
        g_ahbOut = alloc_ahb(g_vkW, g_vkH);
        if (!g_ahbIn0 || !g_ahbIn1 || !g_ahbOut) { LOG("[VkGen] ahb alloc failed"); return; }
    }

    g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, {g_ahbOut},
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
        LOG("[VkGen] present intercepted, capture path %s", g_vk_gen_ready.load() ? "ready" : "warming");
    }

    static std::atomic<uint64_t> frame{0};
    uint64_t n = frame.fetch_add(1);

    if (n < 3 || n % 300 == 0) {
        uint32_t w = 0, h = 0;
        if (pi->swapchainCount > 0) {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_swapchains.find(pi->pSwapchains[0]);
            if (it != g_swapchains.end()) {
                w = it->second.extent.width;
                h = it->second.extent.height;
            }
        }
        LOG("[VkCapture] present #%llu swapchains=%u %ux%u",
            static_cast<unsigned long long>(n), pi->swapchainCount, w, h);
    }

    if (g_seifg_inited.load() && g_vkDevice) {
        if (!g_vk_gen_ready.load()) vk_capture_setup();
        if (g_vk_gen_ready.load() && pi->swapchainCount > 0) vk_capture_present(pi);
    }

    return g_real_present(q, pi);
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
        seifg::initialize(0, false, 0, 2, {});
        LOG("[seifg] initialize done device=%p phys=%p",
            reinterpret_cast<void*>(seifg::getDevice()),
            reinterpret_cast<void*>(seifg::getPhysicalDevice()));
        g_seifg_inited = true;
    }).detach();
}

}
