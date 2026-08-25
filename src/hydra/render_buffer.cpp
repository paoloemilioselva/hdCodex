#include "render_buffer.h"

#include "pxr/base/gf/half.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/vt/value.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

void WritePixel(HdFormat format, std::byte* destination, const float* rgba)
{
    const std::size_t count = HdGetComponentCount(format);
    const HdFormat component = HdGetComponentFormat(format);
    for (std::size_t channel = 0; channel < count; ++channel) {
        const float value = channel < 4U ? rgba[channel] : 0.0F;
        if (component == HdFormatFloat32) {
            reinterpret_cast<float*>(destination)[channel] = value;
        } else if (component == HdFormatFloat16) {
            reinterpret_cast<GfHalf*>(destination)[channel] = GfHalf(value);
        } else if (component == HdFormatUNorm8) {
            reinterpret_cast<std::uint8_t*>(destination)[channel] =
                static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F);
        } else if (component == HdFormatInt32) {
            reinterpret_cast<std::int32_t*>(destination)[channel] =
                static_cast<std::int32_t>(value);
        }
    }
}

} // namespace

HdCodexRenderBuffer::HdCodexRenderBuffer(const SdfPath& id) : HdRenderBuffer(id) {}
HdCodexRenderBuffer::~HdCodexRenderBuffer() = default;

bool HdCodexRenderBuffer::Allocate(
    const GfVec3i& dimensions, HdFormat format, bool multiSampled)
{
    if (dimensions[0] < 0 || dimensions[1] < 0 || dimensions[2] < 0
        || format == HdFormatInvalid) {
        return false;
    }
    const std::scoped_lock lock(_mutex);
    _width = static_cast<unsigned int>(dimensions[0]);
    _height = static_cast<unsigned int>(dimensions[1]);
    _depth = static_cast<unsigned int>(dimensions[2]);
    _format = format;
    _multiSampled = multiSampled;
    _display.assign(static_cast<std::size_t>(_width) * _height * _depth
                        * HdDataSizeOfFormat(_format),
                    std::byte{0});
    _converged.store(false, std::memory_order_release);
    return true;
}

unsigned int HdCodexRenderBuffer::GetWidth() const { return _width; }
unsigned int HdCodexRenderBuffer::GetHeight() const { return _height; }
unsigned int HdCodexRenderBuffer::GetDepth() const { return _depth; }
HdFormat HdCodexRenderBuffer::GetFormat() const { return _format; }
bool HdCodexRenderBuffer::IsMultiSampled() const { return _multiSampled; }

void* HdCodexRenderBuffer::Map()
{
    _mapCount.fetch_add(1, std::memory_order_acq_rel);
    return _display.empty() ? nullptr : _display.data();
}

void HdCodexRenderBuffer::Unmap()
{
    const unsigned int previous = _mapCount.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0U) {
        _mapCount.store(0, std::memory_order_release);
    }
}

bool HdCodexRenderBuffer::IsMapped() const
{
    return _mapCount.load(std::memory_order_acquire) != 0U;
}

void HdCodexRenderBuffer::Resolve() {}

bool HdCodexRenderBuffer::IsConverged() const
{
    return _converged.load(std::memory_order_acquire);
}

void HdCodexRenderBuffer::SetConverged(bool value) noexcept
{
    _converged.store(value, std::memory_order_release);
}

void HdCodexRenderBuffer::Clear(const VtValue& clearValue)
{
    GfVec4f value(0.0F, 0.0F, 0.0F, 1.0F);
    if (clearValue.IsHolding<GfVec4f>()) {
        value = clearValue.UncheckedGet<GfVec4f>();
    } else if (clearValue.IsHolding<float>()) {
        value = GfVec4f(clearValue.UncheckedGet<float>());
    }
    std::array<float, 4> rgba = {value[0], value[1], value[2], value[3]};
    const std::scoped_lock lock(_mutex);
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    for (std::size_t offset = 0; offset < _display.size(); offset += pixelSize) {
        WritePixel(_format, _display.data() + offset, rgba.data());
    }
}

void HdCodexRenderBuffer::WriteFloat4(std::span<const float> rgba)
{
    const std::size_t pixelCount = static_cast<std::size_t>(_width) * _height * _depth;
    if (rgba.size() != pixelCount * 4U) {
        throw std::invalid_argument("render buffer input does not match dimensions");
    }
    const std::scoped_lock lock(_mutex);
    if (_format == HdFormatFloat32Vec4) {
        std::memcpy(_display.data(), rgba.data(), rgba.size_bytes());
        return;
    }
    const std::size_t pixelSize = HdDataSizeOfFormat(_format);
    for (std::size_t pixel = 0; pixel < pixelCount; ++pixel) {
        WritePixel(_format, _display.data() + pixel * pixelSize, rgba.data() + pixel * 4U);
    }
}

void HdCodexRenderBuffer::_Deallocate()
{
    const std::scoped_lock lock(_mutex);
    _display.clear();
    _width = _height = _depth = 0;
    _format = HdFormatInvalid;
}

PXR_NAMESPACE_CLOSE_SCOPE
