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

#include "shadowhook.h"
#include "utility/logger.hh"
#include "seifg.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace seifg_capture {

static PFN_vkQueuePresentKHR g_orig_present = nullptr;
static PFN_vkCreateSwapchainKHR g_orig_create_swapchain = nullptr;

typedef EGLBoolean (*PFN_eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static PFN_eglSwapBuffers_t g_orig_egl_swap = nullptr;

static PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC p_eglGetNativeClientBufferANDROID = nullptr;
static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES = nullptr;

static std::atomic<bool> g_seifg_inited{false};
static std::atomic<bool> g_gen_ready{false};
static int32_t g_ctx = -1;
static AHardwareBuffer* g_ahbIn0 = nullptr;
static AHardwareBuffer* g_ahbIn1 = nullptr;
static AHardwareBuffer* g_ahbOut = nullptr;
static GLuint g_texIn0 = 0, g_fboIn0 = 0, g_texIn1 = 0, g_fboIn1 = 0;
static int g_genW = 0, g_genH = 0;
static char g_triggerPath[512] = {0};

struct SwapchainInfo {
    VkDevice device;
    VkFormat format;
    VkExtent2D extent;
    uint32_t imageCount;
    std::vector<VkImage> images;
};

static std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchains;
static std::mutex g_mu;

static VkResult VKAPI_CALL hooked_CreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* ci,
    const VkAllocationCallbacks* a,
    VkSwapchainKHR* pSc) {

    VkResult result = g_orig_create_swapchain(device, ci, a, pSc);
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

static VkResult VKAPI_CALL hooked_QueuePresentKHR(
    VkQueue q,
    const VkPresentInfoKHR* pi) {

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

    return g_orig_present(q, pi);
}

static void seifg_write_png(const char* name, int w, int h, const uint8_t* rgba) {
    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) {
        size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf);
        (void)rd;
        fclose(cf);
    }
    char path[512];
    snprintf(path, sizeof(path), "/sdcard/Android/data/%s/files/%s", pkg, name);
    stbi_flip_vertically_on_write(1);
    int ok = stbi_write_png(path, w, h, 4, rgba, w * 4);
    LOG("[Gen] wrote %s ok=%d", path, ok);
}

static void dump_ahb(AHardwareBuffer* ahb, const char* name) {
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
    seifg_write_png(name, w, h, tight.data());
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
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    if (!ok0 || !ok1) { LOG("[Gen] fbo setup failed"); return; }

    g_genW = w;
    g_genH = h;
    g_ctx = seifg::createContextFromAHB(g_ahbIn0, g_ahbIn1, {g_ahbOut},
                                        VkExtent2D{static_cast<uint32_t>(w), static_cast<uint32_t>(h)},
                                        VK_FORMAT_R8G8B8A8_UNORM);

    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) { size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf); (void)rd; fclose(cf); }
    snprintf(g_triggerPath, sizeof(g_triggerPath), "/sdcard/Android/data/%s/files/seifg_trigger", pkg);

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
        dump_ahb(g_ahbIn0, "seifg_in0.png");
        dump_ahb(g_ahbIn1, "seifg_in1.png");
        dump_ahb(g_ahbOut, "seifg_out.png");
        LOG("[Gen] triggered dump at frame %llu", static_cast<unsigned long long>(n));
        unlink(g_triggerPath);
    }
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
        if (g_gen_ready.load()) seifg_gen_frame(dpy, surface, n);
    }

    return g_orig_egl_swap(dpy, surface);
}

void Install() {
    static std::atomic_flag done = ATOMIC_FLAG_INIT;
    if (done.test_and_set()) return;

    int init_ret = shadowhook_init(SHADOWHOOK_MODE_UNIQUE, false);
    LOG("[VkCapture] shadowhook_init returned %d", init_ret);

    void* stub_create = shadowhook_hook_sym_name(
        "libvulkan.so", "vkCreateSwapchainKHR",
        reinterpret_cast<void*>(hooked_CreateSwapchainKHR),
        reinterpret_cast<void**>(&g_orig_create_swapchain));
    if (stub_create) {
        LOG("[VkCapture] hooked vkCreateSwapchainKHR stub=%p", stub_create);
    } else {
        ERROR("[VkCapture] hook vkCreateSwapchainKHR failed: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));
    }

    void* stub_present = shadowhook_hook_sym_name(
        "libvulkan.so", "vkQueuePresentKHR",
        reinterpret_cast<void*>(hooked_QueuePresentKHR),
        reinterpret_cast<void**>(&g_orig_present));
    if (stub_present) {
        LOG("[VkCapture] hooked vkQueuePresentKHR stub=%p", stub_present);
    } else {
        ERROR("[VkCapture] hook vkQueuePresentKHR failed: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));
    }

    void* stub_egl = shadowhook_hook_sym_name(
        "libEGL.so", "eglSwapBuffers",
        reinterpret_cast<void*>(hooked_eglSwapBuffers),
        reinterpret_cast<void**>(&g_orig_egl_swap));
    if (stub_egl) {
        LOG("[GlCapture] hooked eglSwapBuffers stub=%p", stub_egl);
    } else {
        ERROR("[GlCapture] hook eglSwapBuffers failed: %s",
              shadowhook_to_errmsg(shadowhook_get_errno()));
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
