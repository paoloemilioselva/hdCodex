#include "hdcodex/gpu/vulkan_context.h"

#include <exception>
#include <iostream>

int main()
{
    try {
        const auto devices = hdcodex::ProbeVulkanDevices();
        bool capable = false;
        for (const auto& device : devices) {
            std::cout << device.name << " rayQuery=" << device.rayQuery
                      << " accelerationStructure=" << device.accelerationStructure
                      << " descriptorIndexing=" << device.descriptorIndexing << '\n';
            capable = capable || device.IsPathTracingCapable();
        }
        if (!capable) {
            std::cerr << "No Vulkan ray-query device found\n";
            return 1;
        }
        hdcodex::VulkanContext context;
        std::cout << "Selected: " << context.DeviceInfo().name << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

