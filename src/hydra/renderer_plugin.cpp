#include "renderer_plugin.h"

#include "render_delegate.h"

#include "pxr/imaging/hd/rendererPluginRegistry.h"

PXR_NAMESPACE_OPEN_SCOPE

TF_REGISTRY_FUNCTION(TfType)
{
    HdRendererPluginRegistry::Define<HdCodexRendererPlugin>();
}

HdRenderDelegate* HdCodexRendererPlugin::CreateRenderDelegate()
{
    return new HdCodexRenderDelegate();
}

HdRenderDelegate* HdCodexRendererPlugin::CreateRenderDelegate(
    const HdRenderSettingsMap& settingsMap)
{
    return new HdCodexRenderDelegate(settingsMap);
}

void HdCodexRendererPlugin::DeleteRenderDelegate(HdRenderDelegate* renderDelegate)
{
    delete renderDelegate;
}

bool HdCodexRendererPlugin::IsSupported(
    const HdRendererCreateArgs& /*createArgs*/, std::string* /*reasonWhyNot*/) const
{
    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

