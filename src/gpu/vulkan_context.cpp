#include "hdcodex/gpu/vulkan_context.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace hdcodex {
namespace {

[[noreturn]] void ThrowVk(const char* operation, VkResult result)
{
    throw std::runtime_error(std::string(operation) + " failed with VkResult "
                             + std::to_string(static_cast<int>(result)));
}

void Check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) {
        ThrowVk(operation, result);
    }
}

[[nodiscard]] bool HasExtension(
    const std::vector<VkExtensionProperties>& extensions, std::string_view name)
{
    return std::ranges::any_of(extensions, [name](const VkExtensionProperties& value) {
        return name == value.extensionName;
    });
}

[[nodiscard]] std::vector<VkExtensionProperties> DeviceExtensions(VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    Check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
          "vkEnumerateDeviceExtensionProperties(count)");
    std::vector<VkExtensionProperties> result(count);
    Check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, result.data()),
          "vkEnumerateDeviceExtensionProperties");
    result.resize(count);
    return result;
}

[[nodiscard]] VulkanDeviceInfo Inspect(VkPhysicalDevice device)
{
    const auto extensions = DeviceExtensions(device);

    VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
    VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    descriptorIndexing.pNext = &bufferAddress;
    bufferAddress.pNext = &acceleration;
    acceleration.pNext = &rayQuery;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &descriptorIndexing;
    vkGetPhysicalDeviceFeatures2(device, &features);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device, &properties);
    return {
        .name = properties.deviceName,
        .vendorId = properties.vendorID,
        .deviceId = properties.deviceID,
        .apiVersion = properties.apiVersion,
        .discrete = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
        .accelerationStructure = acceleration.accelerationStructure == VK_TRUE
            && HasExtension(extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME),
        .rayQuery = rayQuery.rayQuery == VK_TRUE
            && HasExtension(extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME),
        .bufferDeviceAddress = bufferAddress.bufferDeviceAddress == VK_TRUE,
        .descriptorIndexing = descriptorIndexing.runtimeDescriptorArray == VK_TRUE
            && descriptorIndexing.shaderSampledImageArrayNonUniformIndexing == VK_TRUE,
    };
}

[[nodiscard]] VkInstance CreateInstance()
{
    Check(volkInitialize(), "volkInitialize");
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "hdCodex",
        .applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .pEngineName = "hdCodex",
        .engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance instance = VK_NULL_HANDLE;
    Check(vkCreateInstance(&createInfo, nullptr, &instance), "vkCreateInstance");
    volkLoadInstance(instance);
    return instance;
}

[[nodiscard]] std::vector<VkPhysicalDevice> PhysicalDevices(VkInstance instance)
{
    std::uint32_t count = 0;
    Check(vkEnumeratePhysicalDevices(instance, &count, nullptr),
          "vkEnumeratePhysicalDevices(count)");
    std::vector<VkPhysicalDevice> devices(count);
    Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()),
          "vkEnumeratePhysicalDevices");
    devices.resize(count);
    return devices;
}

[[nodiscard]] std::uint32_t FindComputeQueue(VkPhysicalDevice device)
{
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());
    for (std::uint32_t index = 0; index < count; ++index) {
        if ((properties[index].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0U) {
            return index;
        }
    }
    throw std::runtime_error("Vulkan path-tracing device has no compute queue");
}

} // namespace

std::vector<VulkanDeviceInfo> ProbeVulkanDevices()
{
    VkInstance instance = CreateInstance();
    try {
        std::vector<VulkanDeviceInfo> result;
        for (VkPhysicalDevice device : PhysicalDevices(instance)) {
            result.push_back(Inspect(device));
        }
        vkDestroyInstance(instance, nullptr);
        return result;
    } catch (...) {
        vkDestroyInstance(instance, nullptr);
        throw;
    }
}

class VulkanContext::Impl final {
public:
    Impl()
    {
        instance = CreateInstance();
        auto devices = PhysicalDevices(instance);
        std::ranges::sort(devices, [](VkPhysicalDevice left, VkPhysicalDevice right) {
            return Inspect(left).discrete > Inspect(right).discrete;
        });
        for (VkPhysicalDevice candidate : devices) {
            VulkanDeviceInfo candidateInfo = Inspect(candidate);
            if (candidateInfo.IsPathTracingCapable()) {
                physicalDevice = candidate;
                info = std::move(candidateInfo);
                break;
            }
        }
        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error(
                "no Vulkan device supports acceleration structures, ray queries, "
                "buffer device addresses, and descriptor indexing");
        }

        queueFamily = FindComputeQueue(physicalDevice);
        constexpr float priority = 1.0F;
        const VkDeviceQueueCreateInfo queueInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = queueFamily,
            .queueCount = 1,
            .pQueuePriorities = &priority,
        };

        VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
        descriptorIndexing.runtimeDescriptorArray = VK_TRUE;
        descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
        VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
        bufferAddress.bufferDeviceAddress = VK_TRUE;
        VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR};
        acceleration.accelerationStructure = VK_TRUE;
        VkPhysicalDeviceRayQueryFeaturesKHR rayQuery{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
        rayQuery.rayQuery = VK_TRUE;
        descriptorIndexing.pNext = &bufferAddress;
        bufferAddress.pNext = &acceleration;
        acceleration.pNext = &rayQuery;

        const std::array extensions = {
            VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME,
            VK_KHR_RAY_QUERY_EXTENSION_NAME,
            VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME,
        };
        const VkDeviceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &descriptorIndexing,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo,
            .enabledExtensionCount = static_cast<std::uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data(),
        };
        Check(vkCreateDevice(physicalDevice, &createInfo, nullptr, &device),
              "vkCreateDevice");
        volkLoadDevice(device);
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
    }

    ~Impl()
    {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) {
            vkDestroyInstance(instance, nullptr);
        }
    }

    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{0};
    VulkanDeviceInfo info;
};

VulkanContext::VulkanContext() : _impl(std::make_unique<Impl>()) {}
VulkanContext::~VulkanContext() = default;
const VulkanDeviceInfo& VulkanContext::DeviceInfo() const noexcept { return _impl->info; }
std::uint32_t VulkanContext::ComputeQueueFamily() const noexcept { return _impl->queueFamily; }
void* VulkanContext::InstanceHandle() const noexcept { return _impl->instance; }
void* VulkanContext::PhysicalDeviceHandle() const noexcept { return _impl->physicalDevice; }
void* VulkanContext::DeviceHandle() const noexcept { return _impl->device; }
void* VulkanContext::ComputeQueueHandle() const noexcept { return _impl->queue; }

} // namespace hdcodex
