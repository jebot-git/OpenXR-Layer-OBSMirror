#include "vulkan_readback.h"

#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
static void check(VkResult result) {
    if (result != VK_SUCCESS)
        throw std::runtime_error("Vulkan result " + std::to_string(result));
}

static VKAPI_ATTR VkResult VKAPI_CALL failAllocation(VkDevice,
                                                     const VkMemoryAllocateInfo*,
                                                     const VkAllocationCallbacks*,
                                                     VkDeviceMemory*) {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

static VKAPI_ATTR VkResult VKAPI_CALL failSubmit(VkQueue, uint32_t, const VkSubmitInfo*, VkFence) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VKAPI_ATTR VkResult VKAPI_CALL notReady(VkDevice, VkFence) {
    return VK_NOT_READY;
}

struct Fixture {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::shared_ptr<Mirror::VulkanDispatch> dispatch = std::make_shared<Mirror::VulkanDispatch>();
    uint32_t validationErrors = 0;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    static VKAPI_ATTR VkBool32 VKAPI_CALL validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void* user) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            ++static_cast<Fixture*>(user)->validationErrors;
            std::cerr << data->pMessage << '\n';
        }
        return VK_FALSE;
    }

    Fixture() {
        uint32_t count = 0;
        check(vkEnumerateInstanceLayerProperties(&count, nullptr));
        std::vector<VkLayerProperties> layers(count);
        check(vkEnumerateInstanceLayerProperties(&count, layers.data()));
        bool validationAvailable = false;
        for (auto& layer : layers)
            validationAvailable |= std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        const char* debugExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.apiVersion = VK_API_VERSION_1_0;
        VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        create.pApplicationInfo = &app;
        if (validationAvailable) {
            create.enabledLayerCount = 1;
            create.ppEnabledLayerNames = &validationLayer;
            create.enabledExtensionCount = 1;
            create.ppEnabledExtensionNames = &debugExtension;
        }
        check(vkCreateInstance(&create, nullptr, &instance));
        if (validationAvailable) {
            VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            debug.pfnUserCallback = validation;
            debug.pUserData = this;
            check(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(
                instance, "vkCreateDebugUtilsMessengerEXT"))(instance, &debug, nullptr, &messenger));
        }
        std::cout << "Validation layer: " << (validationAvailable ? "enabled" : "unavailable") << '\n';
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        require(count > 0, "No Vulkan device (install a driver or Mesa lavapipe)");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        physical = devices[0];
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(physical, &properties);
        std::cout << "Device: " << properties.deviceName << '\n';
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
            ++family;
        require(family < count, "No graphics queue");
        const float priority = 1;
        VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue.queueFamilyIndex = family;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        VkDeviceCreateInfo logical{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        logical.queueCreateInfoCount = 1;
        logical.pQueueCreateInfos = &queue;
        // Intentionally no device extensions, including external memory.
        check(vkCreateDevice(physical, &logical, nullptr, &device));
        require(dispatch->initialize(vkGetInstanceProcAddr, instance, physical, device, family, 0), "dispatch load");
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    }

    ~Fixture() {
        if (device) {
            vkDeviceWaitIdle(device);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (messenger)
            reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"))(instance, messenger, nullptr);
        if (instance)
            vkDestroyInstance(instance, nullptr);
    }
};

struct Source {
    Fixture& f;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t mips, layers;
    bool used = false;

    Source(Fixture& fixture, VkFormat format, uint32_t mipCount, uint32_t layerCount, VkSampleCountFlagBits samples)
        : f(fixture), mips(mipCount), layers(layerCount) {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = format;
        create.extent = {17, 9, 1}; // Deliberately not row-aligned dimensions.
        create.mipLevels = mips;
        create.arrayLayers = layers;
        create.samples = samples;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(f.device, &create, nullptr, &image));
        VkMemoryRequirements requirements;
        vkGetImageMemoryRequirements(f.device, image, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = requirements.size;
        while (!(requirements.memoryTypeBits & (1u << allocate.memoryTypeIndex)))
            ++allocate.memoryTypeIndex;
        check(vkAllocateMemory(f.device, &allocate, nullptr, &memory));
        check(vkBindImageMemory(f.device, image, memory, 0));
    }
    ~Source() {
        vkDeviceWaitIdle(f.device);
        vkDestroyImage(f.device, image, nullptr);
        vkFreeMemory(f.device, memory, nullptr);
    }

    void render(bool alternate) {
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = f.pool;
        allocate.commandBufferCount = 1;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        VkCommandBuffer commands;
        check(vkAllocateCommandBuffers(f.device, &allocate, &commands));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(commands, &begin));
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = used ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        // Reusing COLOR_ATTACHMENT_OPTIMAL verifies capture restored the layout.
        barrier.oldLayout = used ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
        for (uint32_t layer = 0; layer < layers; ++layer) {
            VkClearColorValue color{};
            color.float32[(layer + (alternate ? 1 : 0)) % 3] = 1;
            color.float32[3] = 1;
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
            vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            if (mips > 1) {
                // Different colors in lower mips catch accidental subresource selection.
                VkClearColorValue other{{1, 1, 1, 1}};
                range.baseMipLevel = 1;
                range.levelCount = mips - 1;
                vkCmdClearColorImage(commands, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &other, 1, &range);
            }
        }
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(commands,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             0,
                             0,
                             nullptr,
                             0,
                             nullptr,
                             1,
                             &barrier);
        check(vkEndCommandBuffer(commands));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        check(vkQueueSubmit(f.dispatch->queue, 1, &submit, VK_NULL_HANDLE));
        // No queue wait: the capture's dependency must order the render writes.
        used = true;
    }
};

static void testTransfer(Fixture& f, VkFormat format, uint32_t mips, uint32_t layers, VkSampleCountFlagBits samples) {
    Source source(f, format, mips, layers, samples);
    Mirror::VulkanReadback capture(f.dispatch);
    check(capture.initialize(format, 17, 9, layers, samples));
    require(capture.rowPitch() == 68 && capture.slicePitch() == 612, "packed pitches");
    const void* pixels = reinterpret_cast<void*>(1);
    require(capture.poll(pixels) == VK_NOT_READY && pixels == nullptr, "no uninitialized pixels");
    for (unsigned frame = 0; frame < 3; ++frame) {
        const bool alternate = (frame % 2) != 0;
        source.render(alternate);
        check(capture.submit(source.image));
        require(capture.submit(source.image) == VK_NOT_READY, "must not overwrite pending readback");
        capture.markConsumed(); // Cannot discard work before it has completed.
        require(capture.submit(source.image) == VK_NOT_READY, "early consume must not reset pending work");
        check(capture.wait());
        check(capture.poll(pixels));
        for (uint32_t layer = 0; layer < layers; ++layer) {
            uint32_t channel = (layer + (alternate ? 1 : 0)) % 3;
            uint32_t expected;
            if (format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
                expected = 0xc0000000u | (0x3ffu << (channel * 10));
            } else {
                if (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB)
                    channel = 2 - channel;
                expected = 0xff000000u | (0xffu << (channel * 8));
            }
            for (uint32_t i = 0; i < 17 * 9; ++i) {
                uint32_t actual;
                std::memcpy(&actual, static_cast<const uint8_t*>(pixels) + layer * capture.slicePitch() + i * 4, 4);
                require(actual == expected, "pixel/channel/layer/mip mismatch");
            }
        }
        capture.markConsumed();
        require(capture.poll(pixels) == VK_NOT_READY && !pixels, "consumed frame must not be published twice");
    }
    // The destructor must finish queued work before destroying its resources.
    source.render(false);
    check(capture.submit(source.image));
}

static void testFailures(Fixture& f) {
    Mirror::VulkanDispatch dispatch;
    require(!dispatch.initialize(nullptr, f.instance, f.physical, f.device, 0, 0), "missing loader");
    Mirror::VulkanReadback invalid(f.dispatch);
    require(invalid.initialize(VK_FORMAT_D32_SFLOAT, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT) == VK_ERROR_FORMAT_NOT_SUPPORTED,
            "depth rejected");
    require(invalid.initialize(VK_FORMAT_R16G16B16A16_SFLOAT, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_FORMAT_NOT_SUPPORTED,
            "unsupported HDR rejected");
    require(invalid.initialize(VK_FORMAT_R8G8B8A8_UNORM, UINT32_MAX, UINT32_MAX, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_OUT_OF_HOST_MEMORY,
            "dimension overflow rejected");
    require(invalid.initialize(VK_FORMAT_R8G8B8A8_UNORM, 0, 9, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_FORMAT_NOT_SUPPORTED,
            "zero extent rejected");
    require(invalid.submit(VK_NULL_HANDLE) == VK_ERROR_INITIALIZATION_FAILED, "uninitialized submit rejected");

    // Exercise destruction with an allocated buffer but no memory/command pool.
    auto fault = std::make_shared<Mirror::VulkanDispatch>(*f.dispatch);
    fault->AllocateMemory = failAllocation;
    Mirror::VulkanReadback partial(fault);
    require(partial.initialize(VK_FORMAT_R8G8B8A8_UNORM, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT) ==
                VK_ERROR_OUT_OF_DEVICE_MEMORY,
            "allocation error propagated");

    Source source(f, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_SAMPLE_COUNT_1_BIT);
    auto recoverable = std::make_shared<Mirror::VulkanDispatch>(*f.dispatch);
    Mirror::VulkanReadback capture(recoverable);
    check(capture.initialize(VK_FORMAT_R8G8B8A8_UNORM, 17, 9, 1, VK_SAMPLE_COUNT_1_BIT));
    source.render(false);
    recoverable->QueueSubmit = failSubmit;
    require(capture.submit(source.image) == VK_ERROR_OUT_OF_HOST_MEMORY, "submission error propagated");
    const void* pixels = reinterpret_cast<void*>(1);
    require(capture.poll(pixels) == VK_NOT_READY && !pixels, "failed submission must not publish pixels");
    check(capture.wait()); // No work submitted: do not wait on the unsignalled fence.
    recoverable->QueueSubmit = f.dispatch->QueueSubmit;
    check(capture.submit(source.image));
    recoverable->GetFenceStatus = notReady;
    require(capture.poll(pixels) == VK_NOT_READY && !pixels, "pending GPU work must not expose host memory");
    recoverable->GetFenceStatus = f.dispatch->GetFenceStatus;
    check(capture.wait());
    check(capture.poll(pixels));
    require(pixels != nullptr, "submission recovers after failure");
    capture.markConsumed();
}

int main() {
    try {
        Fixture f;
        testFailures(f);
        for (auto format : {VK_FORMAT_R8G8B8A8_UNORM,
                            VK_FORMAT_R8G8B8A8_SRGB,
                            VK_FORMAT_B8G8R8A8_UNORM,
                            VK_FORMAT_B8G8R8A8_SRGB,
                            VK_FORMAT_A2B10G10R10_UNORM_PACK32}) {
            testTransfer(f, format, 1, 1, VK_SAMPLE_COUNT_1_BIT);
            testTransfer(f, format, 3, 2, VK_SAMPLE_COUNT_1_BIT);
            VkImageFormatProperties properties{};
            const auto supported = vkGetPhysicalDeviceImageFormatProperties(
                f.physical,
                format,
                VK_IMAGE_TYPE_2D,
                VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                0,
                &properties);
            if (supported == VK_SUCCESS && (properties.sampleCounts & VK_SAMPLE_COUNT_4_BIT))
                testTransfer(f, format, 1, 2, VK_SAMPLE_COUNT_4_BIT);
            else
                std::cout << "MSAA unsupported for format " << format << ", skipped\n";
        }
        require(f.validationErrors == 0, "Vulkan validation errors");
        std::cout << "Vulkan readback tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
