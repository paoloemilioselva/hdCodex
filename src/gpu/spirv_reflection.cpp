#include "hdcodex/gpu/spirv_reflection.h"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace hdcodex {
namespace {

constexpr std::uint16_t kOpName = 5;
constexpr std::uint16_t kOpMemberName = 6;
constexpr std::uint16_t kOpTypeImage = 25;
constexpr std::uint16_t kOpTypeSampledImage = 27;
constexpr std::uint16_t kOpTypeStruct = 30;
constexpr std::uint16_t kOpTypePointer = 32;
constexpr std::uint16_t kOpTypeAccelerationStructure = 5341;
constexpr std::uint16_t kOpVariable = 59;
constexpr std::uint16_t kOpDecorate = 71;
constexpr std::uint16_t kOpMemberDecorate = 72;

constexpr std::uint32_t kDecorationBlock = 2;
constexpr std::uint32_t kDecorationBufferBlock = 3;
constexpr std::uint32_t kDecorationBinding = 33;
constexpr std::uint32_t kDecorationDescriptorSet = 34;
constexpr std::uint32_t kDecorationOffset = 35;

constexpr std::uint32_t kStorageUniformConstant = 0;
constexpr std::uint32_t kStorageUniform = 2;
constexpr std::uint32_t kStorageStorageBuffer = 12;

struct TypeInfo {
    std::uint16_t opcode{0};
    std::uint32_t pointee{0};
    std::uint32_t storageClass{0};
    std::uint32_t sampled{0};
    std::vector<std::uint32_t> members;
};

struct Decorations {
    bool block{false};
    bool bufferBlock{false};
    bool hasBinding{false};
    bool hasSet{false};
    std::uint32_t binding{0};
    std::uint32_t set{0};
};

std::string DecodeString(const std::uint32_t* words, std::size_t wordCount)
{
    const char* bytes = reinterpret_cast<const char*>(words);
    const std::size_t byteCount = wordCount * sizeof(std::uint32_t);
    const auto end = std::find(bytes, bytes + byteCount, '\0');
    return std::string(bytes, end);
}

} // namespace

std::vector<SpirvDescriptor> ReflectSpirvDescriptors(
    std::span<const std::uint32_t> words)
{
    if (words.size() < 5 || words[0] != 0x07230203U) {
        throw std::invalid_argument("SPIR-V reflection received an invalid module");
    }

    std::unordered_map<std::uint32_t, std::string> names;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::string> memberNames;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> memberOffsets;
    std::unordered_map<std::uint32_t, Decorations> decorations;
    std::unordered_map<std::uint32_t, TypeInfo> types;
    struct Variable {
        std::uint32_t type{0};
        std::uint32_t storageClass{0};
    };
    std::unordered_map<std::uint32_t, Variable> variables;

    for (std::size_t cursor = 5; cursor < words.size();) {
        const std::uint32_t instruction = words[cursor];
        const std::uint16_t wordCount = static_cast<std::uint16_t>(instruction >> 16U);
        const std::uint16_t opcode = static_cast<std::uint16_t>(instruction & 0xffffU);
        if (wordCount == 0 || cursor + wordCount > words.size()) {
            throw std::runtime_error("SPIR-V reflection found a malformed instruction");
        }
        const std::uint32_t* operands = words.data() + cursor + 1;
        const std::size_t operandCount = wordCount - 1U;
        switch (opcode) {
        case kOpName:
            if (operandCount >= 2) {
                names[operands[0]] = DecodeString(operands + 1, operandCount - 1);
            }
            break;
        case kOpMemberName:
            if (operandCount >= 3) {
                memberNames[{operands[0], operands[1]}] =
                    DecodeString(operands + 2, operandCount - 2);
            }
            break;
        case kOpDecorate:
            if (operandCount >= 2) {
                Decorations& value = decorations[operands[0]];
                if (operands[1] == kDecorationBlock) value.block = true;
                if (operands[1] == kDecorationBufferBlock) value.bufferBlock = true;
                if (operands[1] == kDecorationBinding && operandCount >= 3) {
                    value.hasBinding = true;
                    value.binding = operands[2];
                }
                if (operands[1] == kDecorationDescriptorSet && operandCount >= 3) {
                    value.hasSet = true;
                    value.set = operands[2];
                }
            }
            break;
        case kOpMemberDecorate:
            if (operandCount >= 4 && operands[2] == kDecorationOffset) {
                memberOffsets[{operands[0], operands[1]}] = operands[3];
            }
            break;
        case kOpTypeImage:
            if (operandCount >= 8) {
                types[operands[0]] = {
                    .opcode = opcode,
                    .sampled = operands[6],
                };
            }
            break;
        case kOpTypeSampledImage:
            if (operandCount >= 2) {
                types[operands[0]] = {.opcode = opcode, .pointee = operands[1]};
            }
            break;
        case kOpTypeStruct:
            if (operandCount >= 1) {
                TypeInfo& type = types[operands[0]];
                type.opcode = opcode;
                type.members.assign(operands + 1, operands + operandCount);
            }
            break;
        case kOpTypePointer:
            if (operandCount >= 3) {
                types[operands[0]] = {
                    .opcode = opcode,
                    .pointee = operands[2],
                    .storageClass = operands[1],
                };
            }
            break;
        case kOpTypeAccelerationStructure:
            if (operandCount >= 1) types[operands[0]] = {.opcode = opcode};
            break;
        case kOpVariable:
            if (operandCount >= 3) {
                variables[operands[1]] = {operands[0], operands[2]};
            }
            break;
        default:
            break;
        }
        cursor += wordCount;
    }

    std::vector<SpirvDescriptor> result;
    for (const auto& [variableId, variable] : variables) {
        const auto decorationIt = decorations.find(variableId);
        if (decorationIt == decorations.end() ||
            !decorationIt->second.hasBinding) {
            continue;
        }
        const Decorations& variableDecoration = decorationIt->second;
        const auto pointerIt = types.find(variable.type);
        if (pointerIt == types.end() || pointerIt->second.opcode != kOpTypePointer) {
            continue;
        }
        const std::uint32_t pointeeId = pointerIt->second.pointee;
        const auto pointeeIt = types.find(pointeeId);

        SpirvDescriptor descriptor;
        descriptor.name = names[variableId];
        descriptor.binding = variableDecoration.binding;
        descriptor.set = variableDecoration.hasSet ? variableDecoration.set : 0U;
        if (variable.storageClass == kStorageStorageBuffer) {
            descriptor.kind = SpirvDescriptorKind::StorageBuffer;
        } else if (variable.storageClass == kStorageUniform &&
                   pointeeIt != types.end() &&
                   (decorations[pointeeId].block || decorations[pointeeId].bufferBlock)) {
            descriptor.kind = decorations[pointeeId].bufferBlock
                ? SpirvDescriptorKind::StorageBuffer
                : SpirvDescriptorKind::UniformBuffer;
        } else if (variable.storageClass == kStorageUniformConstant &&
                   pointeeIt != types.end()) {
            if (pointeeIt->second.opcode == kOpTypeAccelerationStructure) {
                descriptor.kind = SpirvDescriptorKind::AccelerationStructure;
            } else if (pointeeIt->second.opcode == kOpTypeSampledImage) {
                descriptor.kind = SpirvDescriptorKind::SampledImage;
            } else if (pointeeIt->second.opcode == kOpTypeImage) {
                descriptor.kind = pointeeIt->second.sampled == 2U
                    ? SpirvDescriptorKind::StorageImage
                    : SpirvDescriptorKind::SampledImage;
            }
        }

        if (pointeeIt != types.end() && pointeeIt->second.opcode == kOpTypeStruct) {
            for (std::uint32_t member = 0;
                 member < pointeeIt->second.members.size(); ++member) {
                const auto offset = memberOffsets.find({pointeeId, member});
                if (offset == memberOffsets.end()) continue;
                descriptor.members.push_back({
                    .name = memberNames[{pointeeId, member}],
                    .offset = offset->second,
                });
            }
        }
        result.push_back(std::move(descriptor));
    }
    std::ranges::sort(result, [](const SpirvDescriptor& left,
                                const SpirvDescriptor& right) {
        return std::pair(left.set, left.binding) <
            std::pair(right.set, right.binding);
    });
    return result;
}

} // namespace hdcodex
