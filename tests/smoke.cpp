#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#define VK_NO_PROTOTYPES
#include <volk.h>
#include <seifg_shaders.h>
#include <cstdio>

#define CHK(expr, step) do { VkResult r = (expr); if (r != VK_SUCCESS) { printf("FAIL: %s (%d)\n", step, r); return 1; } printf("PASS: %s\n", step); } while(0)

int main() {
    CHK(volkInitialize(), "volkInitialize");
    VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO}; ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo = &ai;
    VkInstance inst; CHK(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance");
    volkLoadInstance(inst);

    uint32_t dc = 0; vkEnumeratePhysicalDevices(inst, &dc, nullptr);
    if (!dc) { printf("FAIL: no devices\n"); return 1; }
    VkPhysicalDevice phys; dc = 1; vkEnumeratePhysicalDevices(inst, &dc, &phys);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    printf("PASS: device=%s vk=%u.%u.%u\n", props.deviceName,
        VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &qc, nullptr);
    VkQueueFamilyProperties qf[16]; vkGetPhysicalDeviceQueueFamilyProperties(phys, &qc, qf);
    uint32_t ci = UINT32_MAX;
    for (uint32_t i = 0; i < qc; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { ci = i; break; }
    if (ci == UINT32_MAX) { printf("FAIL: no compute queue\n"); return 1; }
    printf("PASS: compute qf=%u\n", ci);

    float pri = 1.0f;
    VkDeviceQueueCreateInfo dqci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    dqci.queueFamilyIndex = ci; dqci.queueCount = 1; dqci.pQueuePriorities = &pri;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &dqci;
    VkDevice dev; CHK(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");
    volkLoadDevice(dev);

    VkImageCreateInfo imci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imci.imageType = VK_IMAGE_TYPE_2D; imci.format = VK_FORMAT_R16_SFLOAT;
    imci.extent = {64,64,1}; imci.mipLevels = 1; imci.arrayLayers = 1;
    imci.samples = VK_SAMPLE_COUNT_1_BIT; imci.tiling = VK_IMAGE_TILING_OPTIMAL;
    imci.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VkImage img; CHK(vkCreateImage(dev, &imci, nullptr, &img), "vkCreateImage");

    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    uint32_t mi = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u<<i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mi = i; break; }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; mai.allocationSize = mr.size; mai.memoryTypeIndex = mi;
    VkDeviceMemory mem; CHK(vkAllocateMemory(dev, &mai, nullptr, &mem), "vkAllocateMemory");
    vkBindImageMemory(dev, img, mem, 0);

    VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivci.image = img; ivci.viewType = VK_IMAGE_VIEW_TYPE_2D; ivci.format = VK_FORMAT_R16_SFLOAT;
    ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view; CHK(vkCreateImageView(dev, &ivci, nullptr, &view), "vkCreateImageView");

    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = seifg::shaders::seifg_luma_convert_spv_size;
    smci.pCode = reinterpret_cast<const uint32_t*>(seifg::shaders::seifg_luma_convert_spv);
    VkShaderModule sm; CHK(vkCreateShaderModule(dev, &smci, nullptr, &sm), "vkCreateShaderModule");

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkPipelineLayout lay; CHK(vkCreatePipelineLayout(dev, &plci, nullptr, &lay), "vkCreatePipelineLayout");

    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; cpci.stage.module = sm; cpci.stage.pName = "main";
    cpci.layout = lay;
    VkPipeline pipe; CHK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &pipe), "vkCreateComputePipelines");

    VkCommandPoolCreateInfo cpoolci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpoolci.queueFamilyIndex = ci;
    VkCommandPool pool; CHK(vkCreateCommandPool(dev, &cpoolci, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cbai, &cb);
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbbi);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkEndCommandBuffer(cb);
    printf("PASS: command buffer recorded\n");

    vkDestroyCommandPool(dev, pool, nullptr); vkDestroyPipeline(dev, pipe, nullptr);
    vkDestroyPipelineLayout(dev, lay, nullptr); vkDestroyShaderModule(dev, sm, nullptr);
    vkDestroyImageView(dev, view, nullptr); vkFreeMemory(dev, mem, nullptr);
    vkDestroyImage(dev, img, nullptr); vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    printf("PASS: cleanup\nSUCCESS\n");
    return 0;
}
