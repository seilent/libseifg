#include "capture/vk_capture.hh"

#include <vulkan/vulkan.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <atomic>
#include <cstdio>

#include "shadowhook.h"
#include "utility/logger.hh"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace seifg_capture {

static PFN_vkQueuePresentKHR g_orig_present = nullptr;
static PFN_vkCreateSwapchainKHR g_orig_create_swapchain = nullptr;

typedef EGLBoolean (*PFN_eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static PFN_eglSwapBuffers_t g_orig_egl_swap = nullptr;

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

    char pkg[256] = {0};
    FILE* cf = fopen("/proc/self/cmdline", "r");
    if (cf) {
        size_t rd = fread(pkg, 1, sizeof(pkg) - 1, cf);
        (void)rd;
        fclose(cf);
    }
    char path[512];
    snprintf(path, sizeof(path), "/sdcard/Android/data/%s/files/seifg_gl.png", pkg);

    stbi_flip_vertically_on_write(1);
    int ok = stbi_write_png(path, w, h, 4, buf.data(), w * 4);
    LOG("[GlCapture] dump frame=%llu %dx%d glErr=0x%x ok=%d path=%s",
        static_cast<unsigned long long>(n), w, h, err, ok, path);
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
        if (!dumped.test_and_set()) seifg_dump_gl_frame(dpy, surface, n);
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
