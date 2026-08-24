#pragma once

#include "api.h"

#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/base/gf/vec3i.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <span>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexRenderBuffer final : public HdRenderBuffer {
public:
    explicit HdCodexRenderBuffer(const SdfPath& id);
    ~HdCodexRenderBuffer() override;

    bool Allocate(const GfVec3i& dimensions, HdFormat format, bool multiSampled) override;
    unsigned int GetWidth() const override;
    unsigned int GetHeight() const override;
    unsigned int GetDepth() const override;
    HdFormat GetFormat() const override;
    bool IsMultiSampled() const override;
    void* Map() override;
    void Unmap() override;
    bool IsMapped() const override;
    void Resolve() override;
    bool IsConverged() const override;

    void SetConverged(bool value) noexcept;
    void Clear(const VtValue& clearValue);
    void WriteFloat4(std::span<const float> rgba);

protected:
    void _Deallocate() override;

private:
    mutable std::mutex _mutex;
    unsigned int _width{0};
    unsigned int _height{0};
    unsigned int _depth{0};
    HdFormat _format{HdFormatInvalid};
    bool _multiSampled{false};
    std::vector<std::byte> _display;
    std::atomic_uint _mapCount{0};
    std::atomic_bool _converged{false};
};

PXR_NAMESPACE_CLOSE_SCOPE
