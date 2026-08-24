#include "renderer_plugin.h"

#include "render_delegate.h"

#include "pxr/imaging/hd/rendererPluginRegistry.h"

#if defined(HDCODEX_HAS_VULKAN)
#include "hdcodex/gpu/vulkan_context.h"
#endif

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
    const HdRendererCreateArgs& /*createArgs*/, std::string* reasonWhyNot) const
{
#if defined(HDCODEX_HAS_VULKAN)
    try {
        for (const hdcodex::VulkanDeviceInfo& device : hdcodex::ProbeVulkanDevices()) {
            if (device.IsPathTracingCapable()) {
                return true;
            }
        }
        if (reasonWhyNot) {
            *reasonWhyNot = "No Vulkan device supports hardware ray queries";
        }
        return false;
    } catch (const std::exception& error) {
        if (reasonWhyNot) {
            *reasonWhyNot = error.what();
        }
        return false;
    }
#else
    if (reasonWhyNot) {
        *reasonWhyNot = "hdCodex was built without the Vulkan backend";
    }
    return false;
#endif
}

PXR_NAMESPACE_CLOSE_SCOPE
