#pragma once

#include "api.h"

#include "pxr/imaging/hd/rendererPlugin.h"

PXR_NAMESPACE_OPEN_SCOPE

class HDCODEX_API HdCodexRendererPlugin final : public HdRendererPlugin {
public:
    HdCodexRendererPlugin() = default;
    ~HdCodexRendererPlugin() override = default;

    HdRenderDelegate* CreateRenderDelegate() override;
    HdRenderDelegate* CreateRenderDelegate(const HdRenderSettingsMap& settingsMap) override;
    void DeleteRenderDelegate(HdRenderDelegate* renderDelegate) override;
    bool IsSupported(const HdRendererCreateArgs& createArgs,
                     std::string* reasonWhyNot = nullptr) const override;
};

PXR_NAMESPACE_CLOSE_SCOPE

