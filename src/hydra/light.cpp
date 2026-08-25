#include "light.h"

#include "render_param.h"
#include "texture_loader.h"

#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/usdLux/blackbody.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

PXR_NAMESPACE_OPEN_SCOPE
namespace {

float FloatValue(const VtValue& value, float fallback)
{
    if (value.IsHolding<float>()) return value.UncheckedGet<float>();
    if (value.IsHolding<double>()) return static_cast<float>(value.UncheckedGet<double>());
    if (value.IsHolding<int>()) return static_cast<float>(value.UncheckedGet<int>());
    return fallback;
}

bool BoolValue(const VtValue& value, bool fallback)
{
    if (value.IsHolding<bool>()) return value.UncheckedGet<bool>();
    if (value.IsHolding<int>()) return value.UncheckedGet<int>() != 0;
    return fallback;
}

std::array<float, 3> ColorValue(
    const VtValue& value, std::array<float, 3> fallback)
{
    if (value.IsHolding<GfVec3f>()) {
        const GfVec3f color = value.UncheckedGet<GfVec3f>();
        return {color[0], color[1], color[2]};
    }
    if (value.IsHolding<GfVec3d>()) {
        const GfVec3d color = value.UncheckedGet<GfVec3d>();
        return {static_cast<float>(color[0]), static_cast<float>(color[1]),
                static_cast<float>(color[2])};
    }
    return fallback;
}

std::optional<std::string> AssetValue(const VtValue& value)
{
    if (value.IsHolding<SdfAssetPath>()) {
        const SdfAssetPath& path = value.UncheckedGet<SdfAssetPath>();
        return path.GetResolvedPath().empty() ? path.GetAssetPath() : path.GetResolvedPath();
    }
    if (value.IsHolding<std::string>()) return value.UncheckedGet<std::string>();
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>().GetString();
    return std::nullopt;
}

TfToken TokenValue(const VtValue& value)
{
    if (value.IsHolding<TfToken>()) return value.UncheckedGet<TfToken>();
    if (value.IsHolding<std::string>()) return TfToken(value.UncheckedGet<std::string>());
    return {};
}

std::array<float, 3> ToArray(const GfVec3d& value)
{
    return {static_cast<float>(value[0]), static_cast<float>(value[1]),
            static_cast<float>(value[2])};
}

hdcodex::DomeTextureFormat DomeFormat(const TfToken& token)
{
    const std::string value = token.GetString();
    if (value == "latlong") return hdcodex::DomeTextureFormat::LatLong;
    if (value == "mirroredBall") return hdcodex::DomeTextureFormat::MirroredBall;
    if (value == "angular") return hdcodex::DomeTextureFormat::Angular;
    if (value == "cubeMapVerticalCross") {
        return hdcodex::DomeTextureFormat::CubeMapVerticalCross;
    }
    return hdcodex::DomeTextureFormat::Automatic;
}

} // namespace

HdCodexLight::HdCodexLight(const SdfPath& id, const TfToken& lightType)
    : HdLight(id), _lightType(lightType)
{
    _light.id = id.GetString();
    _light.type = lightType == HdPrimTypeTokens->rectLight
        ? hdcodex::SceneLightType::Rect : hdcodex::SceneLightType::Dome;
}

HdCodexLight::~HdCodexLight() = default;

void HdCodexLight::_UpdateGeometry()
{
    GfMatrix4d transform = _transform;
    if (_light.type == hdcodex::SceneLightType::Dome) {
        transform = _domeOffset * transform;
    }
    const GfVec3d origin = transform.Transform(GfVec3d(0.0));
    GfVec3d x = transform.TransformDir(GfVec3d(1.0, 0.0, 0.0));
    GfVec3d y = transform.TransformDir(GfVec3d(0.0, 1.0, 0.0));
    GfVec3d z = transform.TransformDir(GfVec3d(0.0, 0.0, 1.0));
    if (x.GetLengthSq() > 1e-20) x.Normalize();
    if (y.GetLengthSq() > 1e-20) y.Normalize();
    if (z.GetLengthSq() > 1e-20) z.Normalize();
    _light.position = ToArray(origin);
    _light.basisX = ToArray(x);
    _light.basisY = ToArray(y);
    _light.basisZ = ToArray(z);

    const GfVec3d axisU = _transform.TransformDir(
        GfVec3d(static_cast<double>(_light.width), 0.0, 0.0));
    const GfVec3d axisV = _transform.TransformDir(
        GfVec3d(0.0, static_cast<double>(_light.height), 0.0));
    _light.axisU = ToArray(axisU);
    _light.axisV = ToArray(axisV);
    _light.area = std::max(
        static_cast<float>(GfCross(axisU, axisV).GetLength()), 1e-8F);
}

void HdCodexLight::Sync(
    HdSceneDelegate* sceneDelegate,
    HdRenderParam* renderParam,
    HdDirtyBits* dirtyBits)
{
    if (!sceneDelegate || !renderParam || !dirtyBits) return;
    auto* codexRenderParam = static_cast<HdCodexRenderParam*>(renderParam);
    hdcodex::VersionedScene* scene = codexRenderParam->GetScene();
    const SdfPath& id = GetId();
    const HdDirtyBits bits = *dirtyBits;

    if (bits & DirtyTransform) _transform = sceneDelegate->GetTransform(id);

    if (bits & (DirtyParams | DirtyResource | DirtyShadowParams)) {
        const auto param = [&](const TfToken& token) {
            return sceneDelegate->GetLightParamValue(id, token);
        };
        _light.visible = sceneDelegate->GetVisible(id);
        _light.color = ColorValue(param(HdLightTokens->color), _light.color);
        _light.intensity = FloatValue(param(HdLightTokens->intensity), _light.intensity);
        _light.exposure = FloatValue(param(HdLightTokens->exposure), _light.exposure);
        _light.diffuse = FloatValue(param(HdLightTokens->diffuse), _light.diffuse);
        _light.specular = FloatValue(param(HdLightTokens->specular), _light.specular);
        _light.normalize = BoolValue(param(HdLightTokens->normalize), _light.normalize);
        const bool enableTemperature = BoolValue(
            param(HdLightTokens->enableColorTemperature), false);
        const float temperature = std::clamp(FloatValue(
            param(HdLightTokens->colorTemperature), 6500.0F), 1000.0F, 10000.0F);
        const GfVec3f temperatureColor = enableTemperature
            ? UsdLuxBlackbodyTemperatureAsRgb(temperature) : GfVec3f(1.0F);
        _light.temperatureColor = {
            temperatureColor[0], temperatureColor[1], temperatureColor[2]};

        _light.shadowEnable = BoolValue(
            param(HdLightTokens->shadowEnable), _light.shadowEnable);
        _light.shadowColor = ColorValue(
            param(HdLightTokens->shadowColor), _light.shadowColor);
        _light.shadowDistance = FloatValue(
            param(HdLightTokens->shadowDistance), _light.shadowDistance);
        _light.shadowFalloff = FloatValue(
            param(HdLightTokens->shadowFalloff), _light.shadowFalloff);
        _light.shadowFalloffGamma = std::max(0.0F, FloatValue(
            param(HdLightTokens->shadowFalloffGamma), _light.shadowFalloffGamma));

        _light.shapingFocus = std::max(0.0F, FloatValue(
            param(HdLightTokens->shapingFocus), _light.shapingFocus));
        _light.shapingFocusTint = ColorValue(
            param(HdLightTokens->shapingFocusTint), _light.shapingFocusTint);
        _light.shapingConeAngle = std::clamp(FloatValue(
            param(HdLightTokens->shapingConeAngle), _light.shapingConeAngle),
            0.0F, 180.0F);
        _light.shapingConeSoftness = std::clamp(FloatValue(
            param(HdLightTokens->shapingConeSoftness), _light.shapingConeSoftness),
            0.0F, 1.0F);

        if (_light.type == hdcodex::SceneLightType::Dome) {
            const VtValue offset = param(HdLightTokens->domeOffset);
            _domeOffset = offset.IsHolding<GfMatrix4d>()
                ? offset.UncheckedGet<GfMatrix4d>() : GfMatrix4d(1.0);
            _light.textureFormat = DomeFormat(TokenValue(
                param(HdLightTokens->textureFormat)));
        } else {
            _light.width = std::max(0.0F, FloatValue(
                param(HdLightTokens->width), _light.width));
            _light.height = std::max(0.0F, FloatValue(
                param(HdLightTokens->height), _light.height));
        }

        const std::optional<std::string> texturePath = AssetValue(
            param(HdLightTokens->textureFile));
        _light.texture = hdcodex::LoadSceneTexture(
            scene, texturePath, hdcodex::TextureColorSpace::Auto, true);
    }

    if (bits & (DirtyTransform | DirtyParams | DirtyResource)) _UpdateGeometry();
    scene->UpsertLight(_light);
    *dirtyBits &= ~HdLight::AllDirty;
}

HdDirtyBits HdCodexLight::GetInitialDirtyBitsMask() const
{
    return HdLight::AllDirty;
}

PXR_NAMESPACE_CLOSE_SCOPE
