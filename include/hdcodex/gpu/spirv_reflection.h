#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace hdcodex {

enum class SpirvDescriptorKind {
    Unknown,
    UniformBuffer,
    StorageBuffer,
    SampledImage,
    StorageImage,
    AccelerationStructure,
};

struct SpirvBlockMember {
    std::string name;
    std::uint32_t offset{0};
};

struct SpirvDescriptor {
    std::string name;
    std::uint32_t set{0};
    std::uint32_t binding{0};
    SpirvDescriptorKind kind{SpirvDescriptorKind::Unknown};
    std::vector<SpirvBlockMember> members;
};

/// Reflects descriptor bindings and explicit uniform-block member offsets from
/// a SPIR-V module.  This deliberately small reader consumes standard SPIR-V
/// decorations and does not depend on driver reflection or source parsing.
[[nodiscard]] std::vector<SpirvDescriptor> ReflectSpirvDescriptors(
    std::span<const std::uint32_t> words);

} // namespace hdcodex
