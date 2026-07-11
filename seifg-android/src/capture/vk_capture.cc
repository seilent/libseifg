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

#include "shadowhook.h"
#include "utility/logger.hh"

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
        VkFormat fmt = VK_FORMAT_UNDEFINED;
        uint32_t w = 0, h = 0;

        if (pi->swapchainCount > 0) {
            std::lock_guard<std::mutex> lock(g_mu);
            auto it = g_swapchains.find(pi->pSwapchains[0]);
            if (it != g_swapchains.end()) {
                fmt = it->second.format;
                w = it->second.extent.width;
                h = it->second.extent.height;
            }
        }

        LOG("[VkCapture] present #%llu swapchains=%u imageIndex=%u fmt=%d %ux%u",
            static_cast<unsigned long long>(n),
            pi->swapchainCount,
            (pi->pImageIndices ? pi->pImageIndices[0] : 0),
            static_cast<int>(fmt), w, h);
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
    LOG("[Capture] wrote %s ok=%d", path, ok);
}

static void seifg_dump_gl_frame(EGLDisplay dpy, EGLSurface surface, uint64_t n) {
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return;

    GLint prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::vector<uint8_t> buf(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
    GLenum err = glGetError();

    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));
    LOG("[GlCapture] readpixels %dx%d glErr=0x%x", w, h, err);
    seifg_write_png("seifg_gl.png", w, h, buf.data());
}

static void seifg_capture_ahb(EGLDisplay dpy, EGLSurface surface, uint64_t n) {
    EGLint w = 0, h = 0;
    eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
    eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) { LOG("[Ahb] bad size %dx%d", w, h); return; }

    if (!p_eglGetNativeClientBufferANDROID) {
        p_eglGetNativeClientBufferANDROID =
            (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
        p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
        p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
        p_glEGLImageTargetTexture2DOES =
            (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    }
    if (!p_eglGetNativeClientBufferANDROID || !p_eglCreateImageKHR || !p_glEGLImageTargetTexture2DOES) {
        LOG("[Ahb] proc load failed ncb=%p cimg=%p tex=%p",
            (void*)p_eglGetNativeClientBufferANDROID,
            (void*)p_eglCreateImageKHR,
            (void*)p_glEGLImageTargetTexture2DOES);
        return;
    }

    AHardwareBuffer_Desc d = {};
    d.width = static_cast<uint32_t>(w);
    d.height = static_cast<uint32_t>(h);
    d.layers = 1;
    d.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    d.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
              AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
              AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
    AHardwareBuffer* ahb = nullptr;
    int ar = AHardwareBuffer_allocate(&d, &ahb);
    if (ar != 0 || !ahb) { LOG("[Ahb] allocate failed rc=%d", ar); return; }

    EGLClientBuffer cb = p_eglGetNativeClientBufferANDROID(ahb);
    EGLint imgAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = p_eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, imgAttrs);
    if (img == EGL_NO_IMAGE_KHR) {
        LOG("[Ahb] eglCreateImageKHR failed egl=0x%x", eglGetError());
        AHardwareBuffer_release(ahb);
        return;
    }

    GLint prevTex = 0, prevRead = 0, prevDraw = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLenum fbs = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);

    if (fbs != GL_FRAMEBUFFER_COMPLETE) {
        LOG("[Ahb] fbo incomplete 0x%x", fbs);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        GLenum blitErr = glGetError();
        glFinish();

        void* base = nullptr;
        int lr = AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &base);
        if (lr == 0 && base) {
            AHardwareBuffer_Desc dd = {};
            AHardwareBuffer_describe(ahb, &dd);
            uint32_t stride = dd.stride;
            std::vector<uint8_t> tight(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
            const uint8_t* src = static_cast<const uint8_t*>(base);
            for (int y = 0; y < h; ++y) {
                memcpy(tight.data() + static_cast<size_t>(y) * w * 4,
                       src + static_cast<size_t>(y) * stride * 4,
                       static_cast<size_t>(w) * 4);
            }
            AHardwareBuffer_unlock(ahb, nullptr);
            LOG("[Ahb] captured %dx%d stride=%u blitErr=0x%x", w, h, stride, blitErr);
            seifg_write_png("seifg_ahb.png", w, h, tight.data());
        } else {
            LOG("[Ahb] lock failed rc=%d", lr);
        }
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(prevRead));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(prevDraw));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prevTex));
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    if (p_eglDestroyImageKHR) p_eglDestroyImageKHR(dpy, img);
    AHardwareBuffer_release(ahb);
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

    if (n == 120) {
        static std::atomic_flag dumped = ATOMIC_FLAG_INIT;
        if (!dumped.test_and_set()) {
            seifg_dump_gl_frame(dpy, surface, n);
            seifg_capture_ahb(dpy, surface, n);
        }
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
}

}
