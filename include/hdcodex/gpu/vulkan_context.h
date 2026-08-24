#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hdcodex {

struct VulkanDeviceInfo {
    std::string name;
    std::uint32_t vendorId{0};
    std::uint32_t deviceId{0};
    std::uint32_t apiVersion{0};
    bool discrete{false};
    bool accelerationStructure{false};
    bool rayQuery{false};
    bool bufferDeviceAddress{false};
    bool descriptorIndexing{false};

    [[nodiscard]] bool IsPathTracingCapable() const noexcept
    {
        return accelerationStructure && rayQuery && bufferDeviceAddress
            && descriptorIndexing;
    }
};

/// Enumerates Vulkan devices without retaining an instance.
[[nodiscard]] std::vector<VulkanDeviceInfo> ProbeVulkanDevices();

/// Headless Vulkan context used by the GPU path tracer.
class VulkanContext final {
public:
    VulkanContext();
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;
    VulkanContext(VulkanContext&&) = delete;
    VulkanContext& operator=(VulkanContext&&) = delete;

    [[nodiscard]] const VulkanDeviceInfo& DeviceInfo() const noexcept;
    [[nodiscard]] std::uint32_t ComputeQueueFamily() const noexcept;

    /// Type-erased Vulkan handles keep Vulkan headers out of public Hydra files.
    [[nodiscard]] void* InstanceHandle() const noexcept;
    [[nodiscard]] void* PhysicalDeviceHandle() const noexcept;
    [[nodiscard]] void* DeviceHandle() const noexcept;
    [[nodiscard]] void* ComputeQueueHandle() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

} // namespace hdcodex

