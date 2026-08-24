#include "hdcodex/gpu/vulkan_path_tracer.h"

#include "hdcodex/core/shader_cache.h"
#include "hdcodex/gpu/glsl_compiler.h"
#include "hdcodex/gpu/vulkan_context.h"

#include <volk.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>

namespace hdcodex {
namespace {

constexpr auto kPathTracerSource = R"glsl(
#version 460
#extension GL_EXT_ray_query : require

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(std430, set = 0, binding = 1) buffer OutputPixels { vec4 pixels[]; };
layout(std430, set = 0, binding = 2) readonly buffer Vertices { vec4 positions[]; };
layout(std430, set = 0, binding = 3) readonly buffer Indices { uint indices[]; };
layout(std140, set = 0, binding = 4) uniform CameraBlock
{
    vec4 origin;
    vec4 lowerLeft;
    vec4 horizontal;
    vec4 vertical;
    uvec4 frame;
} camera;

uint hashState(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float randomFloat(inout uint state)
{
    state = hashState(state);
    return float(state) * (1.0 / 4294967296.0);
}

vec3 cosineHemisphere(vec3 normal, inout uint state)
{
    float r1 = 6.28318530718 * randomFloat(state);
    float r2 = randomFloat(state);
    float r2s = sqrt(r2);
    vec3 tangent = normalize(abs(normal.z) < 0.999
        ? cross(normal, vec3(0.0, 0.0, 1.0))
        : cross(normal, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(normal, tangent);
    return normalize(tangent * cos(r1) * r2s +
                     bitangent * sin(r1) * r2s +
                     normal * sqrt(1.0 - r2));
}

vec3 environment(vec3 direction)
{
    float t = 0.5 * (direction.y + 1.0);
    return mix(vec3(0.035, 0.045, 0.065), vec3(0.38, 0.55, 0.85), t);
}

bool occluded(vec3 origin, vec3 direction)
{
    rayQueryEXT shadow;
    rayQueryInitializeEXT(shadow, scene,
        gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
        0xff, origin, 0.001, direction, 10000.0);
    while (rayQueryProceedEXT(shadow)) {}
    return rayQueryGetIntersectionTypeEXT(shadow, true) !=
        gl_RayQueryCommittedIntersectionNoneEXT;
}

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (pixel.x >= camera.frame.x || pixel.y >= camera.frame.y) return;
    uint index = pixel.y * camera.frame.x + pixel.x;
    uint rng = hashState(index ^ (camera.frame.z * 0x9e3779b9u) ^ 0xa511e9b3u);
    vec2 jitter = vec2(randomFloat(rng), randomFloat(rng));
    vec2 uv = (vec2(pixel) + jitter) / vec2(camera.frame.xy);

    vec3 rayOrigin = camera.origin.xyz;
    vec3 rayDirection = normalize(camera.lowerLeft.xyz +
        uv.x * camera.horizontal.xyz + uv.y * camera.vertical.xyz - rayOrigin);
    vec3 throughput = vec3(1.0);
    vec3 radiance = vec3(0.0);

    for (int bounce = 0; bounce < 5; ++bounce)
    {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT,
            0xff, rayOrigin, 0.001, rayDirection, 10000.0);
        while (rayQueryProceedEXT(query)) {}

        if (rayQueryGetIntersectionTypeEXT(query, true) ==
            gl_RayQueryCommittedIntersectionNoneEXT)
        {
            radiance += throughput * environment(rayDirection);
            break;
        }

        uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        uint i0 = indices[primitive * 3u + 0u];
        uint i1 = indices[primitive * 3u + 1u];
        uint i2 = indices[primitive * 3u + 2u];
        vec3 p0 = positions[i0].xyz;
        vec3 p1 = positions[i1].xyz;
        vec3 p2 = positions[i2].xyz;
        vec3 normal = normalize(cross(p1 - p0, p2 - p0));
        if (dot(normal, rayDirection) > 0.0) normal = -normal;

        float distance = rayQueryGetIntersectionTEXT(query, true);
        vec3 hit = rayOrigin + rayDirection * distance;
        vec3 baseColor = 0.35 + 0.55 * vec3(
            float(hashState(primitive + 11u) & 255u),
            float(hashState(primitive + 37u) & 255u),
            float(hashState(primitive + 71u) & 255u)) / 255.0;

        vec3 sunDirection = normalize(vec3(0.6, 0.85, 0.35));
        float nDotL = max(dot(normal, sunDirection), 0.0);
        if (nDotL > 0.0 && !occluded(hit + normal * 0.002, sunDirection)) {
            radiance += throughput * baseColor * vec3(3.5, 3.2, 2.8) * nDotL;
        }

        throughput *= baseColor;
        rayOrigin = hit + normal * 0.002;
        rayDirection = cosineHemisphere(normal, rng);
        if (bounce >= 2) {
            float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.1, 0.95);
            if (randomFloat(rng) > survival) break;
            throughput /= survival;
        }
    }

    vec3 previous = pixels[index].rgb;
    float weight = 1.0 / float(camera.frame.z + 1u);
    pixels[index] = vec4(mix(previous, radiance, weight), 1.0);
}
)glsl";

[[noreturn]] void ThrowVk(const char* operation, VkResult result)
{
    throw std::runtime_error(std::string(operation) + " failed with VkResult " +
        std::to_string(static_cast<int>(result)));
}

void Check(VkResult result, const char* operation)
{
    if (result != VK_SUCCESS) ThrowVk(operation, result);
}

struct GpuVertex { float x, y, z, w; };

struct CameraUniform {
    std::array<float, 4> origin;
    std::array<float, 4> lowerLeft;
    std::array<float, 4> horizontal;
    std::array<float, 4> vertical;
    std::array<std::uint32_t, 4> frame;
};

} // namespace

class VulkanPathTracer::Impl final {
public:
    Impl(VulkanContext& context, ShaderCache& cache)
        : physical(static_cast<VkPhysicalDevice>(context.PhysicalDeviceHandle())),
          device(static_cast<VkDevice>(context.DeviceHandle())),
          queue(static_cast<VkQueue>(context.ComputeQueueHandle())),
          queueFamily(context.ComputeQueueFamily())
    {
        CreateCommandPool();
        CreateDescriptors();
        CreatePipeline(cache);
        uniform = CreateBuffer(sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
    }

    ~Impl()
    {
        if (device == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(device);
        DestroyScene();
        DestroyBuffer(output);
        DestroyBuffer(uniform);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    }

    struct Buffer {
        VkBuffer handle{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
        void* mapped{nullptr};
    };

    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties, bool address)
    {
        Buffer result;
        result.size = size;
        const VkBufferCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        Check(vkCreateBuffer(device, &createInfo, nullptr, &result.handle), "vkCreateBuffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, result.handle, &requirements);

        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        std::uint32_t memoryType = UINT32_MAX;
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((requirements.memoryTypeBits & (1U << index)) != 0U &&
                (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
                memoryType = index;
                break;
            }
        }
        if (memoryType == UINT32_MAX) throw std::runtime_error("no compatible Vulkan memory type");

        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0U;
        const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = address ? &flags : nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = memoryType,
        };
        Check(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(device, result.handle, result.memory, 0), "vkBindBufferMemory");
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
            Check(vkMapMemory(device, result.memory, 0, size, 0, &result.mapped), "vkMapMemory");
        }
        return result;
    }

    void DestroyBuffer(Buffer& buffer)
    {
        if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
        if (buffer.handle) vkDestroyBuffer(device, buffer.handle, nullptr);
        if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
        buffer = {};
    }

    VkDeviceAddress Address(const Buffer& buffer) const
    {
        const VkBufferDeviceAddressInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = buffer.handle,
        };
        return vkGetBufferDeviceAddress(device, &info);
    }

    void Submit(const std::function<void(VkCommandBuffer)>& record)
    {
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer command = VK_NULL_HANDLE;
        Check(vkAllocateCommandBuffers(device, &allocate, &command), "vkAllocateCommandBuffers");
        const VkCommandBufferBeginInfo begin{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        Check(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer");
        record(command);
        Check(vkEndCommandBuffer(command), "vkEndCommandBuffer");
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &command,
        };
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        Check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        vkFreeCommandBuffers(device, commandPool, 1, &command);
    }

    void CreateCommandPool()
    {
        const VkCommandPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
            .queueFamilyIndex = queueFamily,
        };
        Check(vkCreateCommandPool(device, &info, nullptr, &commandPool), "vkCreateCommandPool");
    }

    void CreateDescriptors()
    {
        const std::array bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        Check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout),
              "vkCreateDescriptorSetLayout");
        const std::array poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        };
        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1,
            .poolSizeCount = static_cast<std::uint32_t>(poolSizes.size()),
            .pPoolSizes = poolSizes.data(),
        };
        Check(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool),
              "vkCreateDescriptorPool");
        const VkDescriptorSetAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &descriptorLayout,
        };
        Check(vkAllocateDescriptorSets(device, &allocate, &descriptorSet),
              "vkAllocateDescriptorSets");
    }

    void CreatePipeline(ShaderCache& cache)
    {
        GlslCompileOptions options;
        options.generatorVersion = "hdCodex.pathtracer.v1";
        options.materialAbi = "hdcodex.pathtracer.v1";
        const auto spirv = GlslCompiler(cache).Compile(
            kPathTracerSource, GlslShaderStage::Compute, "path_tracer.comp", options);
        const VkShaderModuleCreateInfo moduleInfo{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = spirv.words.size() * sizeof(std::uint32_t),
            .pCode = spirv.words.data(),
        };
        VkShaderModule module = VK_NULL_HANDLE;
        Check(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "vkCreateShaderModule");
        try {
            const VkPipelineLayoutCreateInfo layoutInfo{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                .setLayoutCount = 1,
                .pSetLayouts = &descriptorLayout,
            };
            Check(vkCreatePipelineLayout(device, &layoutInfo, nullptr, &pipelineLayout),
                  "vkCreatePipelineLayout");
            const VkPipelineShaderStageCreateInfo stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main",
            };
            const VkComputePipelineCreateInfo pipelineInfo{
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage = stage,
                .layout = pipelineLayout,
            };
            Check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo,
                                           nullptr, &pipeline),
                  "vkCreateComputePipelines");
            vkDestroyShaderModule(device, module, nullptr);
        } catch (...) {
            vkDestroyShaderModule(device, module, nullptr);
            throw;
        }
    }

    void DestroyAcceleration(VkAccelerationStructureKHR& acceleration, Buffer& storage)
    {
        if (acceleration) vkDestroyAccelerationStructureKHR(device, acceleration, nullptr);
        acceleration = VK_NULL_HANDLE;
        DestroyBuffer(storage);
    }

    void DestroyScene()
    {
        DestroyAcceleration(tlas, tlasStorage);
        DestroyAcceleration(blas, blasStorage);
        DestroyBuffer(instanceBuffer);
        DestroyBuffer(vertexBuffer);
        DestroyBuffer(indexBuffer);
        geometryReady = false;
    }

    VkAccelerationStructureKHR CreateAcceleration(
        VkAccelerationStructureTypeKHR type, VkDeviceSize size, Buffer& storage)
    {
        storage = CreateBuffer(size,
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        const VkAccelerationStructureCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = storage.handle,
            .size = size,
            .type = type,
        };
        VkAccelerationStructureKHR result = VK_NULL_HANDLE;
        Check(vkCreateAccelerationStructureKHR(device, &info, nullptr, &result),
              "vkCreateAccelerationStructureKHR");
        return result;
    }

    void BuildScene(const std::shared_ptr<const SceneSnapshot>& snapshot)
    {
        vkDeviceWaitIdle(device);
        DestroyScene();
        if (!snapshot) return;

        std::vector<GpuVertex> vertices;
        std::vector<std::uint32_t> indices;
        for (const SceneMesh& mesh : snapshot->meshes) {
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
                vertices.push_back({mesh.positions[i], mesh.positions[i + 1],
                                    mesh.positions[i + 2], 1.0F});
            }
            for (std::uint32_t index : mesh.indices) {
                if (index < vertices.size() - base) indices.push_back(base + index);
            }
        }
        indices.resize(indices.size() - (indices.size() % 3U));
        if (vertices.empty() || indices.empty()) return;

        const VkBufferUsageFlags inputUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        vertexBuffer = CreateBuffer(vertices.size() * sizeof(GpuVertex), inputUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        indexBuffer = CreateBuffer(indices.size() * sizeof(std::uint32_t), inputUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        std::memcpy(vertexBuffer.mapped, vertices.data(), static_cast<std::size_t>(vertexBuffer.size));
        std::memcpy(indexBuffer.mapped, indices.data(), static_cast<std::size_t>(indexBuffer.size));

        VkAccelerationStructureGeometryKHR geometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
        geometry.geometry.triangles = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData = {.deviceAddress = Address(vertexBuffer)},
            .vertexStride = sizeof(GpuVertex),
            .maxVertex = static_cast<std::uint32_t>(vertices.size() - 1U),
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData = {.deviceAddress = Address(indexBuffer)},
        };
        const std::uint32_t primitiveCount = static_cast<std::uint32_t>(indices.size() / 3U);
        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
        buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        buildInfo.geometryCount = 1;
        buildInfo.pGeometries = &geometry;
        VkAccelerationStructureBuildSizesInfoKHR sizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &buildInfo, &primitiveCount, &sizes);
        blas = CreateAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                                  sizes.accelerationStructureSize, blasStorage);
        Buffer scratch = CreateBuffer(sizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        buildInfo.dstAccelerationStructure = blas;
        buildInfo.scratchData.deviceAddress = Address(scratch);
        const VkAccelerationStructureBuildRangeInfoKHR range{
            .primitiveCount = primitiveCount,
        };
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = &range;
        Submit([&](VkCommandBuffer command) {
            vkCmdBuildAccelerationStructuresKHR(command, 1, &buildInfo, &rangePointer);
        });
        DestroyBuffer(scratch);

        const VkAccelerationStructureDeviceAddressInfoKHR addressInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = blas,
        };
        VkAccelerationStructureInstanceKHR instance{};
        instance.transform.matrix[0][0] = 1.0F;
        instance.transform.matrix[1][1] = 1.0F;
        instance.transform.matrix[2][2] = 1.0F;
        instance.mask = 0xff;
        instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
        instance.accelerationStructureReference =
            vkGetAccelerationStructureDeviceAddressKHR(device, &addressInfo);
        instanceBuffer = CreateBuffer(sizeof(instance), inputUsage,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true);
        std::memcpy(instanceBuffer.mapped, &instance, sizeof(instance));

        VkAccelerationStructureGeometryKHR tlasGeometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        tlasGeometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        tlasGeometry.geometry.instances = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            .data = {.deviceAddress = Address(instanceBuffer)},
        };
        VkAccelerationStructureBuildGeometryInfoKHR tlasBuild{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        tlasBuild.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
        tlasBuild.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
        tlasBuild.geometryCount = 1;
        tlasBuild.pGeometries = &tlasGeometry;
        constexpr std::uint32_t instanceCount = 1;
        VkAccelerationStructureBuildSizesInfoKHR tlasSizes{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(device,
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
            &tlasBuild, &instanceCount, &tlasSizes);
        tlas = CreateAcceleration(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                                  tlasSizes.accelerationStructureSize, tlasStorage);
        scratch = CreateBuffer(tlasSizes.buildScratchSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true);
        tlasBuild.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        tlasBuild.dstAccelerationStructure = tlas;
        tlasBuild.scratchData.deviceAddress = Address(scratch);
        const VkAccelerationStructureBuildRangeInfoKHR tlasRange{
            .primitiveCount = instanceCount,
        };
        const VkAccelerationStructureBuildRangeInfoKHR* tlasRangePointer = &tlasRange;
        Submit([&](VkCommandBuffer command) {
            vkCmdBuildAccelerationStructuresKHR(command, 1, &tlasBuild, &tlasRangePointer);
        });
        DestroyBuffer(scratch);
        geometryReady = true;
        UpdateDescriptors();
    }

    void EnsureOutput(std::uint32_t width, std::uint32_t height)
    {
        const VkDeviceSize required = static_cast<VkDeviceSize>(width) * height * 4U * sizeof(float);
        if (output.size == required) return;
        vkDeviceWaitIdle(device);
        DestroyBuffer(output);
        output = CreateBuffer(required, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
        std::memset(output.mapped, 0, static_cast<std::size_t>(required));
        UpdateDescriptors();
    }

    void UpdateDescriptors()
    {
        if (!geometryReady || !output.handle || !uniform.handle) return;
        VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite{
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        accelerationWrite.accelerationStructureCount = 1;
        accelerationWrite.pAccelerationStructures = &tlas;
        const VkDescriptorBufferInfo outputInfo{output.handle, 0, output.size};
        const VkDescriptorBufferInfo vertexInfo{vertexBuffer.handle, 0, vertexBuffer.size};
        const VkDescriptorBufferInfo indexInfo{indexBuffer.handle, 0, indexBuffer.size};
        const VkDescriptorBufferInfo uniformInfo{uniform.handle, 0, uniform.size};
        std::array<VkWriteDescriptorSet, 5> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &accelerationWrite,
            descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr};
        const std::array infos = {&outputInfo, &vertexInfo, &indexInfo, &uniformInfo};
        for (std::uint32_t index = 0; index < infos.size(); ++index) {
            writes[index + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                descriptorSet, index + 1, 0, 1,
                index == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, infos[index], nullptr};
        }
        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    std::vector<float> Trace(const PathTracerCamera& camera, std::uint32_t width,
                             std::uint32_t height, std::uint32_t sample)
    {
        if (!geometryReady || width == 0 || height == 0) return {};
        EnsureOutput(width, height);
        CameraUniform data{
            {camera.origin[0], camera.origin[1], camera.origin[2], 0.0F},
            {camera.lowerLeft[0], camera.lowerLeft[1], camera.lowerLeft[2], 0.0F},
            {camera.horizontal[0], camera.horizontal[1], camera.horizontal[2], 0.0F},
            {camera.vertical[0], camera.vertical[1], camera.vertical[2], 0.0F},
            {width, height, sample, 0U},
        };
        std::memcpy(uniform.mapped, &data, sizeof(data));
        Submit([&](VkCommandBuffer command) {
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            vkCmdDispatch(command, (width + 7U) / 8U, (height + 7U) / 8U, 1);
            const VkMemoryBarrier barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr);
        });
        std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4U);
        std::memcpy(pixels.data(), output.mapped, pixels.size() * sizeof(float));
        return pixels;
    }

    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{0};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    Buffer uniform;
    Buffer output;
    Buffer vertexBuffer;
    Buffer indexBuffer;
    Buffer instanceBuffer;
    Buffer blasStorage;
    Buffer tlasStorage;
    VkAccelerationStructureKHR blas{VK_NULL_HANDLE};
    VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};
    bool geometryReady{false};
};

VulkanPathTracer::VulkanPathTracer(VulkanContext& context, ShaderCache& cache)
    : _impl(std::make_unique<Impl>(context, cache)) {}
VulkanPathTracer::~VulkanPathTracer() = default;
void VulkanPathTracer::SetScene(const std::shared_ptr<const SceneSnapshot>& scene)
{
    _impl->BuildScene(scene);
}
std::vector<float> VulkanPathTracer::Render(
    const PathTracerCamera& camera, std::uint32_t width, std::uint32_t height,
    std::uint32_t sampleIndex)
{
    return _impl->Trace(camera, width, height, sampleIndex);
}
bool VulkanPathTracer::HasGeometry() const noexcept { return _impl->geometryReady; }

} // namespace hdcodex
