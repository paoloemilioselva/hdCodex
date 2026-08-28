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
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace hdcodex {
namespace {

constexpr std::uint32_t kMaxMaterialTextures = 256U;
constexpr std::uint32_t kMissingTexture = UINT32_MAX;
constexpr std::uint32_t kUdimTextureFlag = 0x80000000U;
constexpr std::uint32_t kUdimTileMask = 0x7fffffU;
constexpr std::uint32_t kMaxPathBounces = 12U;
constexpr std::uint32_t kMaxLights = 64U;
constexpr std::uint32_t kOpaqueSceneFlag = 1U << 8U;
constexpr std::uint32_t kLightCountShift = 16U;

constexpr auto kPathTracerSource = R"glsl(
#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;
layout(std430, set = 0, binding = 1) buffer OutputPixels { vec4 pixels[]; };
layout(std430, set = 0, binding = 2) readonly buffer Vertices { vec4 positions[]; };
layout(std430, set = 0, binding = 3) readonly buffer Indices { uint indices[]; };
layout(std430, set = 0, binding = 5) readonly buffer TriangleMaterials { uint triangleMaterials[]; };
struct GpuMaterial {
    vec4 baseColorMetalness;
    vec4 emissionRoughness;
    vec4 transmissionOpacityIor;
    vec4 transmissionColorThinWalled;
    vec4 subsurfaceWeightScale;
    vec4 subsurfaceColor;
    vec4 subsurfaceRadius;
    vec4 specularWeightColor;
    vec4 coatWeightRoughnessIor;
    vec4 coatColor;
    vec4 transmissionMedium;
    vec4 transmissionScatter;
    vec4 sheenColorMode;
    vec4 diffuseProperties;
    uvec4 textureIndices0;
    uvec4 textureIndices1;
    uvec4 textureIndices2;
    uvec4 textureIndices3;
    uvec4 textureIndices4;
};
layout(std430, set = 0, binding = 6) readonly buffer Materials { GpuMaterial materials[]; };
layout(std430, set = 0, binding = 7) readonly buffer TextureCoordinates { vec2 texcoords[]; };
layout(set = 0, binding = 8) uniform sampler2D materialTextures[256];
layout(std430, set = 0, binding = 9) readonly buffer ShadingNormals { vec4 shadingNormals[]; };
struct GpuLight {
    vec4 colorIntensity;
    vec4 controls;
    vec4 position;
    vec4 axisU;
    vec4 axisV;
    vec4 basisX;
    vec4 basisY;
    vec4 basisZ;
    vec4 shaping;
    vec4 focusTint;
    vec4 shadow;
    vec4 shadowColor;
    vec4 geometry;
    uvec4 textureInfo;
};
layout(std430, set = 0, binding = 10) readonly buffer Lights { GpuLight lights[]; };
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

// Non-negative RGB-to-spectrum reconstruction. Equal RGB values reconstruct
// to a flat reflectance while saturated colors become smooth, overlapping
// spectral lobes rather than three independent transport channels.
float reconstructRgb(vec3 rgb, float wavelength)
{
    vec3 basis = vec3(
        exp(-0.5 * pow((wavelength - 620.0) / 30.0, 2.0)),
        exp(-0.5 * pow((wavelength - 535.0) / 25.0, 2.0)),
        exp(-0.5 * pow((wavelength - 450.0) / 20.0, 2.0)));
    return dot(max(rgb, vec3(0.0)), basis) /
        max(dot(vec3(1.0), basis), 1e-5);
}

float reflectanceSpectrum(vec3 rgb, float wavelength)
{
    return clamp(reconstructRgb(rgb, wavelength), 0.0, 1.0);
}

float illuminantSpectrum(vec3 rgb, float wavelength)
{
    // A 6504 K daylight-like reference makes authored neutral RGB lights
    // integrate to the white point expected by linear sRGB output.
    const float temperature = 6504.0;
    const float c2 = 1.4387769e7; // nm K
    const float referenceWavelength = 560.0;
    float planck = pow(referenceWavelength / wavelength, 5.0) *
        (exp(c2 / (referenceWavelength * temperature)) - 1.0) /
        max(exp(c2 / (wavelength * temperature)) - 1.0, 1e-5);
    return max(reconstructRgb(rgb, wavelength) * planck, 0.0);
}

vec3 cieXyz(float wavelength)
{
    float tx1 = (wavelength - 442.0) *
        (wavelength < 442.0 ? 0.0624 : 0.0374);
    float tx2 = (wavelength - 599.8) *
        (wavelength < 599.8 ? 0.0264 : 0.0323);
    float tx3 = (wavelength - 501.1) *
        (wavelength < 501.1 ? 0.0490 : 0.0382);
    float ty1 = (wavelength - 568.8) *
        (wavelength < 568.8 ? 0.0213 : 0.0247);
    float ty2 = (wavelength - 530.9) *
        (wavelength < 530.9 ? 0.0613 : 0.0322);
    float tz1 = (wavelength - 437.0) *
        (wavelength < 437.0 ? 0.0845 : 0.0278);
    float tz2 = (wavelength - 459.0) *
        (wavelength < 459.0 ? 0.0385 : 0.0725);
    return vec3(
        0.362 * exp(-0.5 * tx1 * tx1) +
        1.056 * exp(-0.5 * tx2 * tx2) -
        0.065 * exp(-0.5 * tx3 * tx3),
        0.821 * exp(-0.5 * ty1 * ty1) +
        0.286 * exp(-0.5 * ty2 * ty2),
        1.217 * exp(-0.5 * tz1 * tz1) +
        0.681 * exp(-0.5 * tz2 * tz2));
}

vec3 spectralSensor(float wavelength)
{
    vec3 xyz = cieXyz(wavelength) * (400.0 / 106.856895);
    return vec3(
         3.2404542 * xyz.x - 1.5371385 * xyz.y - 0.4985314 * xyz.z,
        -0.9692660 * xyz.x + 1.8760108 * xyz.y + 0.0415560 * xyz.z,
         0.0556434 * xyz.x - 0.2040259 * xyz.y + 1.0572252 * xyz.z);
}
)glsl"
R"glsl(

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

float luminance(vec3 value)
{
    return dot(value, vec3(0.2126, 0.7152, 0.0722));
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (vec3(1.0) - f0) * pow(1.0 - clamp(cosTheta, 0.0, 1.0), 5.0);
}

float distributionGGX(float nDotH, float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator = nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    return alpha2 / max(3.14159265359 * denominator * denominator, 1e-6);
}

float geometrySchlickGGX(float nDotDirection, float roughness)
{
    float r = roughness + 1.0;
    float k = r * r * 0.125;
    return nDotDirection / max(nDotDirection * (1.0 - k) + k, 1e-6);
}

vec3 evaluateGGX(vec3 normal, vec3 viewDirection, vec3 lightDirection,
                 float roughness, vec3 f0)
{
    vec3 halfway = normalize(viewDirection + lightDirection);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotH = max(dot(normal, halfway), 0.0);
    float vDotH = max(dot(viewDirection, halfway), 0.0);
    float distribution = distributionGGX(nDotH, roughness);
    float geometry = geometrySchlickGGX(nDotV, roughness) *
                     geometrySchlickGGX(nDotL, roughness);
    vec3 fresnel = fresnelSchlick(vDotH, f0);
    return distribution * geometry * fresnel /
           max(4.0 * nDotV * nDotL, 1e-5);
}

vec3 sampleGGXReflection(vec3 incident, vec3 normal, float roughness,
                         inout uint state)
{
    float alpha = max(roughness * roughness, 0.001);
    float u1 = randomFloat(state);
    float u2 = randomFloat(state);
    float phi = 6.28318530718 * u1;
    float cosTheta = sqrt((1.0 - u2) /
        max(1.0 + (alpha * alpha - 1.0) * u2, 1e-6));
    float sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    vec3 tangent = normalize(abs(normal.z) < 0.999
        ? cross(normal, vec3(0.0, 0.0, 1.0))
        : cross(normal, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(normal, tangent);
    vec3 halfway = normalize(tangent * (cos(phi) * sinTheta) +
                             bitangent * (sin(phi) * sinTheta) +
                             normal * cosTheta);
    vec3 reflected = reflect(incident, halfway);
    return dot(reflected, normal) > 0.0
        ? normalize(reflected) : normalize(reflect(incident, normal));
}

vec3 ggxSampleWeight(vec3 incident, vec3 outgoing, vec3 normal,
                     float roughness, vec3 f0)
{
    vec3 viewDirection = normalize(-incident);
    vec3 halfway = normalize(viewDirection + outgoing);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, outgoing), 0.0);
    float nDotH = max(dot(normal, halfway), 0.0);
    float vDotH = max(dot(viewDirection, halfway), 0.0);
    if (nDotV <= 0.0 || nDotL <= 0.0 || nDotH <= 0.0) return vec3(0.0);
    float geometry = geometrySchlickGGX(nDotV, roughness) *
                     geometrySchlickGGX(nDotL, roughness);
    return fresnelSchlick(vDotH, f0) * geometry * vDotH /
           max(nDotV * nDotH, 1e-5);
}

vec3 orthogonalTangent(vec3 normal, vec3 tangent)
{
    vec3 result = tangent - normal * dot(normal, tangent);
    if (dot(result, result) < 1e-8) {
        result = abs(normal.z) < 0.999
            ? cross(normal, vec3(0.0, 0.0, 1.0))
            : cross(normal, vec3(0.0, 1.0, 0.0));
    }
    return normalize(result);
}

float distributionGGXAnisotropic(vec3 halfway, vec3 normal, vec3 tangent,
                                 vec2 roughness)
{
    vec3 axisX = orthogonalTangent(normal, tangent);
    vec3 axisY = cross(normal, axisX);
    vec2 alpha = clamp(roughness * roughness, vec2(0.001), vec2(1.0));
    vec3 localHalf = vec3(dot(halfway, axisX), dot(halfway, axisY),
                          max(dot(halfway, normal), 0.0));
    float denominator = localHalf.x * localHalf.x / (alpha.x * alpha.x) +
        localHalf.y * localHalf.y / (alpha.y * alpha.y) +
        localHalf.z * localHalf.z;
    return 1.0 / max(3.14159265359 * alpha.x * alpha.y *
                     denominator * denominator, 1e-6);
}

float geometryGGXAnisotropic(vec3 direction, vec3 normal, vec3 tangent,
                             vec2 roughness)
{
    float nDotDirection = max(dot(normal, direction), 0.0);
    if (nDotDirection <= 0.0) return 0.0;
    vec3 axisX = orthogonalTangent(normal, tangent);
    vec3 axisY = cross(normal, axisX);
    vec2 alpha = clamp(roughness * roughness, vec2(0.001), vec2(1.0));
    float projected = pow(alpha.x * dot(direction, axisX), 2.0) +
                      pow(alpha.y * dot(direction, axisY), 2.0);
    float lambda = (-1.0 + sqrt(1.0 + projected /
        max(nDotDirection * nDotDirection, 1e-8))) * 0.5;
    return 1.0 / (1.0 + lambda);
}

vec3 evaluateGGXAnisotropic(vec3 normal, vec3 tangent, vec3 viewDirection,
                            vec3 lightDirection, vec2 roughness, vec3 f0)
{
    vec3 halfway = normalize(viewDirection + lightDirection);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float vDotH = max(dot(viewDirection, halfway), 0.0);
    if (nDotV <= 0.0 || nDotL <= 0.0 || vDotH <= 0.0) return vec3(0.0);
    float distribution = distributionGGXAnisotropic(
        halfway, normal, tangent, roughness);
    float geometry = geometryGGXAnisotropic(
        viewDirection, normal, tangent, roughness) *
        geometryGGXAnisotropic(lightDirection, normal, tangent, roughness);
    return distribution * geometry * fresnelSchlick(vDotH, f0) /
           max(4.0 * nDotV * nDotL, 1e-5);
}

vec3 sampleGGXReflectionAnisotropic(vec3 incident, vec3 normal, vec3 tangent,
                                    vec2 roughness, inout uint state)
{
    vec3 axisX = orthogonalTangent(normal, tangent);
    vec3 axisY = cross(normal, axisX);
    vec2 alpha = clamp(roughness * roughness, vec2(0.001), vec2(1.0));
    float phi = 6.28318530718 * randomFloat(state);
    float elevationSample = randomFloat(state);
    float radius = sqrt(elevationSample /
        max(1.0 - elevationSample, 1e-6));
    vec3 halfway = normalize(axisX * (alpha.x * radius * cos(phi)) +
                             axisY * (alpha.y * radius * sin(phi)) + normal);
    if (dot(halfway, -incident) < 0.0) halfway = -halfway;
    vec3 reflected = reflect(incident, halfway);
    return dot(reflected, normal) > 0.0
        ? normalize(reflected) : normalize(reflect(incident, normal));
}

vec3 ggxSampleWeightAnisotropic(vec3 incident, vec3 outgoing, vec3 normal,
                                vec3 tangent, vec2 roughness, vec3 f0)
{
    vec3 viewDirection = normalize(-incident);
    vec3 halfway = normalize(viewDirection + outgoing);
    float nDotL = max(dot(normal, outgoing), 0.0);
    float nDotH = max(dot(normal, halfway), 0.0);
    float vDotH = max(dot(viewDirection, halfway), 0.0);
    if (nDotL <= 0.0 || nDotH <= 0.0 || vDotH <= 0.0) return vec3(0.0);
    float distribution = distributionGGXAnisotropic(
        halfway, normal, tangent, roughness);
    float pdf = distribution * nDotH / max(4.0 * vDotH, 1e-6);
    return evaluateGGXAnisotropic(
        normal, tangent, viewDirection, outgoing, roughness, f0) *
        nDotL / max(pdf, 1e-6);
}
)glsl"
R"glsl(

float ggxReflectionPdfAnisotropic(vec3 normal, vec3 tangent,
                                  vec3 viewDirection, vec3 lightDirection,
                                  vec2 roughness)
{
    if (dot(normal, viewDirection) <= 0.0 ||
        dot(normal, lightDirection) <= 0.0) return 0.0;
    vec3 halfway = normalize(viewDirection + lightDirection);
    float nDotH = max(dot(normal, halfway), 0.0);
    float vDotH = max(dot(viewDirection, halfway), 0.0);
    return distributionGGXAnisotropic(
        halfway, normal, tangent, roughness) * nDotH /
        max(4.0 * vDotH, 1e-6);
}

float powerHeuristic(float firstPdf, float secondPdf)
{
    float firstSquared = firstPdf * firstPdf;
    float secondSquared = secondPdf * secondPdf;
    return firstSquared / max(firstSquared + secondSquared, 1e-12);
}

float surfaceReflectionPdf(
    vec3 normal, vec3 tangent, vec3 viewDirection, vec3 lightDirection,
    vec2 roughness, vec2 coatRoughness,
    float sheenProbability, float coatProbability, float specularProbability)
{
    float cosinePdf = max(dot(normal, lightDirection), 0.0) /
        3.14159265359;
    float coatPdf = ggxReflectionPdfAnisotropic(
        normal, tangent, viewDirection, lightDirection, coatRoughness);
    float specularPdf = ggxReflectionPdfAnisotropic(
        normal, tangent, viewDirection, lightDirection, roughness);
    float basePdf = coatProbability * coatPdf +
        (1.0 - coatProbability) *
        (specularProbability * specularPdf +
         (1.0 - specularProbability) * cosinePdf);
    return sheenProbability * cosinePdf +
        (1.0 - sheenProbability) * basePdf;
}

float sheenDirectionalAlbedo(float nDotV, float roughness)
{
    float x = clamp(nDotV, 0.0, 1.0);
    float y = clamp(roughness, 0.01, 1.0);
    vec2 fit = vec2(13.67300, 1.0) + vec2(-68.78018, 61.57746) * x +
        vec2(799.08825, 442.78211) * y +
        vec2(-905.00061, 2597.49308) * x * y +
        vec2(60.28956, 121.81241) * x * x +
        vec2(1086.96473, 3045.55075) * y * y;
    return clamp(fit.x / max(fit.y, 1e-6), 0.0, 1.0);
}

float sheenImageworksResponse(vec3 normal, vec3 viewDirection,
                              vec3 lightDirection, float roughness)
{
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    if (nDotL <= 0.0 || nDotV <= 0.0) return 0.0;
    vec3 halfway = normalize(viewDirection + lightDirection);
    float nDotH = max(dot(normal, halfway), 0.0);
    float inverseRoughness = 1.0 / max(roughness, 0.005);
    float distribution = (2.0 + inverseRoughness) *
        pow(max(1.0 - nDotH * nDotH, 0.0), inverseRoughness * 0.5) /
        (2.0 * 3.14159265359);
    float brdf = distribution /
        max(4.0 * (nDotL + nDotV - nDotL * nDotV), 1e-6);
    return brdf * nDotL;
}

float sheenZeltnerDirectionalAlbedo(float nDotV, float roughness)
{
    float x = clamp(nDotV, 0.0, 1.0);
    float y = clamp(roughness, 0.01, 1.0);
    float s = y * (0.0206607 + 1.58491 * y) /
        (0.0379424 + y * (1.32227 + y));
    float m = y * (-0.193854 + y * (-1.14885 +
        y * (1.7932 - 0.95943 * y * y))) / (0.046391 + y);
    float o = y * (0.000654023 + (-0.0207818 + 0.119681 * y) * y) /
        (1.26264 + y * (-1.92021 + y));
    return clamp(exp(-0.5 * pow((x - m) / max(s, 1e-6), 2.0)) /
        (max(s, 1e-6) * sqrt(2.0 * 3.14159265359)) + o, 0.0, 1.0);
}

float sheenResponse(vec3 normal, vec3 viewDirection, vec3 lightDirection,
                    float roughness, bool zeltner)
{
    if (!zeltner) return sheenImageworksResponse(
        normal, viewDirection, lightDirection, roughness);
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    return sheenZeltnerDirectionalAlbedo(nDotV, roughness) *
        nDotL / 3.14159265359;
}

vec2 directionToVerticalCross(vec3 direction)
{
    vec3 absoluteDirection = abs(direction);
    vec2 faceUv;
    vec2 tile;
    if (absoluteDirection.x >= absoluteDirection.y &&
        absoluteDirection.x >= absoluteDirection.z) {
        if (direction.x >= 0.0) {
            faceUv = vec2(-direction.z, direction.y) / absoluteDirection.x;
            tile = vec2(2.0, 1.0);
        } else {
            faceUv = vec2(direction.z, direction.y) / absoluteDirection.x;
            tile = vec2(0.0, 1.0);
        }
    } else if (absoluteDirection.y >= absoluteDirection.z) {
        if (direction.y >= 0.0) {
            faceUv = vec2(direction.x, -direction.z) / absoluteDirection.y;
            tile = vec2(1.0, 0.0);
        } else {
            faceUv = vec2(direction.x, direction.z) / absoluteDirection.y;
            tile = vec2(1.0, 2.0);
        }
    } else if (direction.z >= 0.0) {
        faceUv = vec2(direction.x, direction.y) / absoluteDirection.z;
        tile = vec2(1.0, 1.0);
    } else {
        faceUv = vec2(-direction.x, direction.y) / absoluteDirection.z;
        tile = vec2(1.0, 3.0);
    }
    return (tile + faceUv * 0.5 + 0.5) / vec2(3.0, 4.0);
}

vec2 domeTextureCoordinates(vec3 direction, uint format)
{
    const float pi = 3.14159265359;
    if (format == 2u) {
        float denominator = 2.0 * sqrt(max(direction.x * direction.x +
            direction.y * direction.y + (direction.z + 1.0) *
            (direction.z + 1.0), 1e-8));
        return vec2(direction.x / denominator + 0.5,
                    direction.y / denominator + 0.5);
    }
    if (format == 3u) {
        float radius = acos(clamp(-direction.z, -1.0, 1.0)) / pi;
        vec2 radial = length(direction.xy) > 1e-6
            ? normalize(direction.xy) : vec2(0.0);
        return vec2(0.5) + radial * (0.5 * radius);
    }
    if (format == 4u) return directionToVerticalCross(direction);
    return vec2(fract(atan(direction.x, -direction.z) /
                      (2.0 * pi) + 1.0),
                1.0 - acos(clamp(direction.y, -1.0, 1.0)) / pi);
}

vec3 sampleDome(GpuLight light, vec3 worldDirection)
{
    vec3 localDirection = normalize(vec3(
        dot(worldDirection, light.basisX.xyz),
        dot(worldDirection, light.basisY.xyz),
        dot(worldDirection, light.basisZ.xyz)));
    vec3 textureValue = light.textureInfo.x == 0xffffffffu ? vec3(1.0) :
        texture(materialTextures[nonuniformEXT(light.textureInfo.x)],
                domeTextureCoordinates(localDirection, light.textureInfo.y)).rgb;
    return light.colorIntensity.rgb * light.colorIntensity.a * textureValue;
}

vec3 environment(vec3 direction)
{
    vec3 result = vec3(0.0);
    uint lightCount = (camera.frame.w >> 16u) & 0xffu;
    if (lightCount == 0u) {
        vec3 up = camera.vertical.w > 0.5
            ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
        float t = 0.5 * (dot(direction, up) + 1.0);
        return mix(vec3(0.035, 0.045, 0.065),
                   vec3(0.38, 0.55, 0.85), t);
    }
    for (uint index = 0u; index < lightCount; ++index) {
        if (lights[index].textureInfo.z == 0u) {
            result += sampleDome(lights[index], direction);
        }
    }
    return result;
}

vec3 latLongDirection(vec2 uv)
{
    const float pi = 3.14159265359;
    float theta = pi * (1.0 - uv.y);
    float phi = 2.0 * pi * uv.x;
    float sineTheta = sin(theta);
    return vec3(sineTheta * sin(phi), cos(theta),
                -sineTheta * cos(phi));
}

vec3 uniformSphere(inout uint state);

float domeLightPdf(GpuLight light, vec3 worldDirection)
{
    if (light.textureInfo.w == 0xffffffffu) return 1.0 / 12.5663706144;
    vec3 localDirection = normalize(vec3(
        dot(worldDirection, light.basisX.xyz),
        dot(worldDirection, light.basisY.xyz),
        dot(worldDirection, light.basisZ.xyz)));
    vec2 uv = domeTextureCoordinates(localDirection, light.textureInfo.y);
    uint importanceIndex = light.textureInfo.w;
    ivec2 dimensions = textureSize(
        materialTextures[nonuniformEXT(importanceIndex)], 0);
    ivec2 texel = clamp(ivec2(uv * vec2(dimensions)),
                        ivec2(0), dimensions - ivec2(1));
    return max(texelFetch(materialTextures[nonuniformEXT(importanceIndex)],
                          texel, 0).b, 0.0);
}

void sampleDomeLight(GpuLight light, inout uint rng,
                     out vec3 worldDirection, out float pdf)
{
    if (light.textureInfo.w == 0xffffffffu) {
        worldDirection = uniformSphere(rng);
        pdf = 1.0 / 12.5663706144;
        return;
    }
    uint importanceIndex = light.textureInfo.w;
    ivec2 dimensions = textureSize(
        materialTextures[nonuniformEXT(importanceIndex)], 0);
    float rowTarget = randomFloat(rng);
    int low = 0;
    int high = dimensions.y - 1;
    while (low < high) {
        int middle = (low + high) / 2;
        float cdf = texelFetch(
            materialTextures[nonuniformEXT(importanceIndex)],
            ivec2(0, middle), 0).g;
        if (rowTarget <= cdf) high = middle;
        else low = middle + 1;
    }
    int row = low;
    float columnTarget = randomFloat(rng);
    low = 0;
    high = dimensions.x - 1;
    while (low < high) {
        int middle = (low + high) / 2;
        float cdf = texelFetch(
            materialTextures[nonuniformEXT(importanceIndex)],
            ivec2(middle, row), 0).r;
        if (columnTarget <= cdf) high = middle;
        else low = middle + 1;
    }
    int column = low;
    const float pi = 3.14159265359;
    float v0 = float(row) / float(dimensions.y);
    float v1 = float(row + 1) / float(dimensions.y);
    float cosine0 = cos(pi * (1.0 - v0));
    float cosine1 = cos(pi * (1.0 - v1));
    float cosineTheta = mix(cosine0, cosine1, randomFloat(rng));
    float v = 1.0 - acos(clamp(cosineTheta, -1.0, 1.0)) / pi;
    float u = (float(column) + randomFloat(rng)) / float(dimensions.x);
    vec3 localDirection = latLongDirection(vec2(u, v));
    worldDirection = normalize(
        light.basisX.xyz * localDirection.x +
        light.basisY.xyz * localDirection.y +
        light.basisZ.xyz * localDirection.z);
    pdf = max(texelFetch(materialTextures[nonuniformEXT(importanceIndex)],
                         ivec2(column, row), 0).b, 0.0);
}

float environmentLightPdf(vec3 direction)
{
    uint lightCount = (camera.frame.w >> 16u) & 0xffu;
    if (lightCount == 0u) return 0.0;
    float pdf = 0.0;
    for (uint index = 0u; index < lightCount; ++index) {
        if (lights[index].textureInfo.z == 0u) {
            pdf += domeLightPdf(lights[index], direction);
        }
    }
    return pdf / float(lightCount);
}

vec4 sampleMaterialTexture(uint textureIndex, vec2 uv, vec4 fallbackValue)
{
    if (textureIndex == 0xffffffffu) return fallbackValue;
    if ((textureIndex & 0x80000000u) != 0u) {
        ivec2 tile = ivec2(floor(uv));
        int tileOffset = tile.x + tile.y * 10;
        if (tileOffset < 0 || tileOffset >= 23) return fallbackValue;
        uint presentTiles = (textureIndex >> 8u) & 0x7fffffu;
        uint tileBit = 1u << uint(tileOffset);
        if ((presentTiles & tileBit) == 0u) return fallbackValue;
        uint descriptorIndex = (textureIndex & 0xffu) +
            uint(bitCount(presentTiles & (tileBit - 1u)));
        return texture(materialTextures[nonuniformEXT(descriptorIndex)], fract(uv));
    }
    return texture(materialTextures[nonuniformEXT(textureIndex)], uv);
}

vec2 surfaceTextureCoordinates(uint primitive, vec2 barycentrics)
{
    vec3 barycentric = vec3(1.0 - barycentrics.x - barycentrics.y,
                            barycentrics.x, barycentrics.y);
    vec2 uv0 = texcoords[primitive * 3u + 0u];
    vec2 uv1 = texcoords[primitive * 3u + 1u];
    vec2 uv2 = texcoords[primitive * 3u + 2u];
    return uv0 * barycentric.x + uv1 * barycentric.y + uv2 * barycentric.z;
}

float surfaceOpacity(uint primitive, vec2 barycentrics)
{
    GpuMaterial material = materials[triangleMaterials[primitive]];
    vec2 uv = surfaceTextureCoordinates(primitive, barycentrics);
    return clamp(sampleMaterialTexture(material.textureIndices1.x, uv,
        vec4(material.transmissionOpacityIor.y)).r, 0.0, 1.0);
}
)glsl"
R"glsl(

vec3 applyNormalMap(uint textureIndex, bool flipY, float scale,
                    vec2 uv, vec3 geometricNormal,
                    vec3 p0, vec3 p1, vec3 p2, vec2 uv0, vec2 uv1, vec2 uv2)
{
    if (textureIndex == 0xffffffffu) return geometricNormal;
    vec2 deltaUv1 = uv1 - uv0;
    vec2 deltaUv2 = uv2 - uv0;
    float determinant = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
    if (abs(determinant) < 1e-8) return geometricNormal;
    vec3 edge1 = p1 - p0;
    vec3 edge2 = p2 - p0;
    vec3 tangent = normalize((edge1 * deltaUv2.y - edge2 * deltaUv1.y) / determinant);
    tangent = normalize(tangent - geometricNormal * dot(geometricNormal, tangent));
    vec3 bitangent = normalize(cross(geometricNormal, tangent));
    if (determinant < 0.0) bitangent = -bitangent;
    vec3 mapped = sampleMaterialTexture(textureIndex, uv, vec4(0.5, 0.5, 1.0, 1.0)).xyz;
    mapped = mapped * 2.0 - 1.0;
    if (flipY) mapped.y = -mapped.y;
    mapped.xy *= scale;
    vec3 result = normalize(tangent * mapped.x + bitangent * mapped.y +
                            geometricNormal * mapped.z);
    return dot(result, geometricNormal) < 0.0 ? -result : result;
}

vec3 surfaceTangent(vec3 normal, vec3 p0, vec3 p1, vec3 p2,
                    vec2 uv0, vec2 uv1, vec2 uv2)
{
    vec2 deltaUv1 = uv1 - uv0;
    vec2 deltaUv2 = uv2 - uv0;
    float determinant = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
    if (abs(determinant) < 1e-8) return orthogonalTangent(normal, vec3(1.0, 0.0, 0.0));
    vec3 tangent = ((p1 - p0) * deltaUv2.y -
                    (p2 - p0) * deltaUv1.y) / determinant;
    return orthogonalTangent(normal, tangent);
}

vec3 surfaceShadingNormal(uint primitive, vec2 barycentrics, vec3 geometricNormal)
{
    vec3 barycentric = vec3(1.0 - barycentrics.x - barycentrics.y,
                            barycentrics.x, barycentrics.y);
    vec3 result = shadingNormals[primitive * 3u + 0u].xyz * barycentric.x +
                  shadingNormals[primitive * 3u + 1u].xyz * barycentric.y +
                  shadingNormals[primitive * 3u + 2u].xyz * barycentric.z;
    if (dot(result, result) < 1e-12) return geometricNormal;
    result = normalize(result);
    return dot(result, geometricNormal) < 0.0 ? -result : result;
}

vec3 shadowVisibility(vec3 origin, vec3 direction, float maximumDistance,
                      GpuLight light)
{
    if (light.shadow.x < 0.5) return vec3(1.0);
    float traceDistance = maximumDistance;
    if (light.shadow.y >= 0.0) traceDistance = min(traceDistance, light.shadow.y);
    if (traceDistance <= 0.001) return vec3(1.0);
    rayQueryEXT shadow;
    uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT;
    if ((camera.frame.w & 0x100u) != 0u) rayFlags |= gl_RayFlagsOpaqueEXT;
    rayQueryInitializeEXT(shadow, scene,
        rayFlags,
        0xff, origin, 0.001, direction, traceDistance);
    while (rayQueryProceedEXT(shadow)) {
        if (rayQueryGetIntersectionTypeEXT(shadow, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT) {
            uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(shadow, false);
            vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(shadow, false);
            if (surfaceOpacity(primitive, barycentrics) >= 0.5) {
                rayQueryConfirmIntersectionEXT(shadow);
            }
        }
    }
    if (rayQueryGetIntersectionTypeEXT(shadow, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT) return vec3(1.0);
    float strength = 1.0;
    float hitDistance = rayQueryGetIntersectionTEXT(shadow, true);
    if (light.shadow.y >= 0.0 && light.shadow.z > 0.0) {
        float falloff = min(light.shadow.z, max(light.shadow.y, 0.0));
        strength = clamp((light.shadow.y - hitDistance) /
                         max(falloff, 1e-6), 0.0, 1.0);
        strength = pow(strength, max(light.shadow.w, 0.0));
    }
    return mix(vec3(1.0), light.shadowColor.rgb, strength);
}

float eonDirectionalAlbedo(float nDotDirection, float roughness)
{
    const float c1 = 0.2877934092;
    float cosine = clamp(nDotDirection, 0.0, 1.0);
    float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    float theta = acos(cosine);
    float sineCubed = sine * sine * sine;
    float g = sine * (theta - sine * cosine) + (2.0 / 3.0) *
        (sine * (1.0 - sineCubed) / max(cosine, 1e-5) - sine);
    float a = 1.0 / (1.0 + c1 * roughness);
    return clamp(a + roughness * a * g / 3.14159265359, 0.0, 1.0);
}

vec3 evaluateDiffuseReflection(
    vec3 normal, vec3 viewDirection, vec3 lightDirection,
    vec3 diffuseColor, float roughness, uint model)
{
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float nDotV = max(dot(normal, viewDirection), 0.0);
    if (nDotL <= 0.0 || nDotV <= 0.0) return vec3(0.0);
    if (model == 1u) {
        float sigma = clamp(roughness, 0.0, 1.0);
        float sigmaSquared = sigma * sigma;
        float a = 1.0 - 0.5 * sigmaSquared /
            (sigmaSquared + 0.33);
        float b = 0.45 * sigmaSquared /
            (sigmaSquared + 0.09);
        vec3 projectedLight = lightDirection - normal * nDotL;
        vec3 projectedView = viewDirection - normal * nDotV;
        float azimuth = 0.0;
        if (dot(projectedLight, projectedLight) > 1e-8 &&
            dot(projectedView, projectedView) > 1e-8) {
            azimuth = max(dot(normalize(projectedLight),
                              normalize(projectedView)), 0.0);
        }
        float sinLight = sqrt(max(1.0 - nDotL * nDotL, 0.0));
        float sinView = sqrt(max(1.0 - nDotV * nDotV, 0.0));
        float sinAlpha = max(sinLight, sinView);
        float tanBeta = min(sinLight / max(nDotL, 1e-5),
                            sinView / max(nDotV, 1e-5));
        return vec3(nDotL * (a + b * azimuth * sinAlpha * tanBeta) /
            3.14159265359);
    }
    if (model == 2u) {
        vec3 halfway = normalize(viewDirection + lightDirection);
        float lDotH = max(dot(lightDirection, halfway), 0.0);
        float grazing = 0.5 + 2.0 * clamp(roughness, 0.0, 1.0) *
            lDotH * lDotH;
        float lightScatter = 1.0 + (grazing - 1.0) *
            pow(1.0 - nDotL, 5.0);
        float viewScatter = 1.0 + (grazing - 1.0) *
            pow(1.0 - nDotV, 5.0);
        return vec3(nDotL * lightScatter * viewScatter / 3.14159265359);
    }
    if (model == 3u) {
        const float c1 = 0.2877934092;
        const float c2 = 0.0726760455;
        float sigma = clamp(roughness, 0.0, 1.0);
        float a = 1.0 / (1.0 + c1 * sigma);
        float s = dot(lightDirection, viewDirection) - nDotL * nDotV;
        float sOverT = s > 0.0 ? s / max(nDotL, nDotV) : s;
        float singleScatter = a * (1.0 + sigma * sOverT);
        float averageAlbedo = (1.0 + c2 * sigma) /
            (1.0 + c1 * sigma);
        float outgoingAlbedo = eonDirectionalAlbedo(nDotV, sigma);
        float incidentAlbedo = eonDirectionalAlbedo(nDotL, sigma);
        float directionalFactor =
            (1.0 - outgoingAlbedo) * (1.0 - incidentAlbedo) /
            max(1.0 - averageAlbedo, 1e-5);
        vec3 rho = clamp(diffuseColor, vec3(0.0), vec3(0.9999));
        vec3 multipleScatterOverRho = rho * averageAlbedo /
            max(vec3(1.0) - rho * (1.0 - averageAlbedo), vec3(1e-5));
        return nDotL / 3.14159265359 *
            (vec3(singleScatter) + multipleScatterOverRho * directionalFactor);
    }
    return vec3(nDotL / 3.14159265359);
}

vec3 evaluateDirectSurface(
    vec3 normal, vec3 tangent, vec3 viewDirection, vec3 lightDirection,
    vec3 diffuseColor, float transmission, float subsurface,
    float diffuseWeight, float diffuseRoughness, uint diffuseModel,
    float metalness, vec2 roughness, vec3 f0,
    float coat, vec2 coatRoughness, vec3 coatF0,
    float sheen, vec3 sheenColor, float sheenRoughness, bool sheenZeltner,
    float diffuseScale, float specularScale)
{
    float nDotL = max(dot(normal, lightDirection), 0.0);
    float wrappedNdotL = max((dot(normal, lightDirection) + 0.5 * subsurface) /
                             (1.0 + 0.5 * subsurface), 0.0);
    if (wrappedNdotL <= 0.0 && nDotL <= 0.0) return vec3(0.0);
    vec3 halfway = normalize(viewDirection + lightDirection);
    vec3 baseFresnel = fresnelSchlick(
        max(dot(viewDirection, halfway), 0.0), f0);
    vec3 diffuseResponse = mix(evaluateDiffuseReflection(
        normal, viewDirection, lightDirection, diffuseColor,
        diffuseRoughness, diffuseModel),
        vec3(wrappedNdotL / 3.14159265359), subsurface);
    vec3 diffuse = diffuseScale * (vec3(1.0) - baseFresnel) *
        diffuseWeight * (1.0 - metalness) * diffuseColor *
        diffuseResponse * (1.0 - transmission);
    vec3 specular = specularScale * nDotL * evaluateGGXAnisotropic(
        normal, tangent, viewDirection, lightDirection, roughness, f0);
    vec3 coatFresnel = vec3(0.0);
    vec3 coatSpecular = vec3(0.0);
    if (coat > 0.0) {
        coatFresnel = fresnelSchlick(
            max(dot(viewDirection, halfway), 0.0), coatF0);
        coatSpecular = specularScale * coat * nDotL * evaluateGGXAnisotropic(
            normal, tangent, viewDirection, lightDirection,
            coatRoughness, coatF0);
    }
    vec3 baseLayer = (vec3(1.0) - coat * coatFresnel) *
        (diffuse + specular) + coatSpecular;
    float sheenAlbedo = sheenZeltner
        ? sheenZeltnerDirectionalAlbedo(
            max(dot(normal, viewDirection), 0.0), sheenRoughness)
        : sheenDirectionalAlbedo(
            max(dot(normal, viewDirection), 0.0), sheenRoughness);
    vec3 sheenLayer = specularScale * sheen * sheenColor * sheenResponse(
        normal, viewDirection, lightDirection, sheenRoughness, sheenZeltner);
    return baseLayer * (1.0 - clamp(sheen * sheenAlbedo, 0.0, 1.0)) +
        sheenLayer;
}

vec3 uniformSphere(inout uint state)
{
    float y = 1.0 - 2.0 * randomFloat(state);
    float radius = sqrt(max(1.0 - y * y, 0.0));
    float phi = 6.28318530718 * randomFloat(state);
    return vec3(radius * cos(phi), y, radius * sin(phi));
}

float shapingConeFactor(GpuLight light, float cosine)
{
    float angle = acos(clamp(cosine, -1.0, 1.0));
    float outer = radians(clamp(light.shaping.y, 0.0, 180.0));
    float inner = outer * (1.0 - clamp(light.shaping.z, 0.0, 1.0));
    float cone = outer >= 3.1415925 ? 1.0 :
        (outer <= inner + 1e-6 ? (angle <= outer ? 1.0 : 0.0) :
         1.0 - smoothstep(inner, outer, angle));
    return cone;
}

float dispersedIor(float iorAtD, float scale, float abbeNumber,
                   float wavelength)
{
    if (scale <= 0.0 || abbeNumber <= 1.0) return iorAtD;
    const float lambdaF = 486.13;
    const float lambdaD = 587.56;
    const float lambdaC = 656.27;
    float reciprocalDelta = 1.0 / (lambdaF * lambdaF) -
        1.0 / (lambdaC * lambdaC);
    float b = (iorAtD - 1.0) /
        max(abbeNumber * reciprocalDelta, 1e-8);
    float a = iorAtD - b / (lambdaD * lambdaD);
    float cauchyIor = a + b / (wavelength * wavelength);
    return mix(iorAtD, cauchyIor, clamp(scale, 0.0, 1.0));
}

vec3 sampleHenyeyGreenstein(vec3 forward, float anisotropy, inout uint state)
{
    float g = clamp(anisotropy, -0.95, 0.95);
    float u = randomFloat(state);
    float cosine = abs(g) < 1e-3 ? 1.0 - 2.0 * u :
        (1.0 + g * g - pow((1.0 - g * g) /
         (1.0 - g + 2.0 * g * u), 2.0)) / (2.0 * g);
    float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    float phi = 6.28318530718 * randomFloat(state);
    vec3 w = normalize(forward);
    vec3 tangent = normalize(abs(w.z) < 0.999
        ? cross(w, vec3(0.0, 0.0, 1.0))
        : cross(w, vec3(0.0, 1.0, 0.0)));
    vec3 bitangent = cross(w, tangent);
    return normalize(tangent * (cos(phi) * sine) +
                     bitangent * (sin(phi) * sine) + w * cosine);
}

bool traceOpaqueBoundary(vec3 origin, vec3 direction,
                         out vec3 boundary, out vec3 boundaryNormal,
                         out float boundaryDistance)
{
    rayQueryEXT boundaryQuery;
    rayQueryInitializeEXT(boundaryQuery, scene, gl_RayFlagsOpaqueEXT,
        0xff, origin, 0.001, direction, 10000.0);
    while (rayQueryProceedEXT(boundaryQuery)) {}
    if (rayQueryGetIntersectionTypeEXT(boundaryQuery, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT) return false;
    uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(boundaryQuery, true);
    uint i0 = indices[primitive * 3u + 0u];
    uint i1 = indices[primitive * 3u + 1u];
    uint i2 = indices[primitive * 3u + 2u];
    boundaryDistance = rayQueryGetIntersectionTEXT(boundaryQuery, true);
    boundary = origin + direction * boundaryDistance;
    boundaryNormal = normalize(cross(
        positions[i1].xyz - positions[i0].xyz,
        positions[i2].xyz - positions[i0].xyz));
    if (dot(boundaryNormal, direction) > 0.0) boundaryNormal = -boundaryNormal;
    return true;
}

bool randomWalkSubsurface(vec3 entry, vec3 outwardNormal,
                          float meanFreePath, float albedo,
                          float anisotropy, inout uint state,
                          out vec3 exitOrigin, out vec3 exitDirection,
                          out float attenuation)
{
    float radius = max(meanFreePath, 0.001);
    float sigmaS = 1.0 / radius;
    float sigmaA = -log(clamp(albedo, 0.001, 0.9999)) / radius;
    vec3 origin = entry - outwardNormal * 0.003;
    vec3 direction = cosineHemisphere(-outwardNormal, state);
    attenuation = 1.0;
    for (int step = 0; step < 8; ++step) {
        vec3 boundary;
        vec3 boundaryNormal;
        float boundaryDistance;
        if (!traceOpaqueBoundary(origin, direction, boundary,
                                 boundaryNormal, boundaryDistance)) return false;
        float scatterDistance = -log(max(1.0 - randomFloat(state), 1e-6)) /
            sigmaS;
        bool forceExit = step == 7;
        float travel = forceExit ? boundaryDistance :
            min(scatterDistance, boundaryDistance);
        attenuation *= exp(-sigmaA * travel);
        if (forceExit || scatterDistance >= boundaryDistance) {
            exitDirection = direction;
            exitOrigin = boundary + direction * 0.003;
            return true;
        }
        origin += direction * scatterDistance;
        direction = sampleHenyeyGreenstein(direction, anisotropy, state);
    }
    return false;
}
)glsl"
R"glsl(
void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (pixel.x >= camera.frame.x || pixel.y >= camera.frame.y) return;
    uint index = pixel.y * camera.frame.x + pixel.x;
    uint sampleCount = max(uint(camera.origin.w + 0.5), 1u);
    vec3 batchRadiance = vec3(0.0);
    for (uint sampleOffset = 0u; sampleOffset < sampleCount; ++sampleOffset)
    {
    uint sampleNumber = camera.frame.z + sampleOffset;
    vec3 spectralSampleRadiance = vec3(0.0);
    for (uint spectralLane = 0u; spectralLane < 3u; ++spectralLane)
    {
    uint rng = hashState(index ^ (sampleNumber * 0x9e3779b9u) ^ 0xa511e9b3u);
    float wavelengthShift = float(hashState(index ^ 0x68bc21ebu)) *
        (1.0 / 4294967296.0);
    float wavelength = 380.0 + 400.0 * fract(
        wavelengthShift + (float(sampleNumber & 31u) + 0.5) / 32.0 +
        float(spectralLane) / 3.0);
    vec2 jitter = vec2(randomFloat(rng), randomFloat(rng));
    vec2 uv = (vec2(pixel) + jitter) / vec2(camera.frame.xy);

    vec3 rayOrigin = camera.origin.xyz;
    vec3 rayDirection = normalize(camera.lowerLeft.xyz +
        uv.x * camera.horizontal.xyz + uv.y * camera.vertical.xyz - rayOrigin);
    vec3 throughput = vec3(1.0);
    vec3 radiance = vec3(0.0);
    bool environmentOnMiss = true;
    bool previousWasDelta = true;
    float previousBsdfPdf = 0.0;
    bool insideMedium = false;
    float mediumAbsorption = 0.0;
    float mediumScattering = 0.0;
    float mediumAnisotropy = 0.0;

    int maxBounces = int(clamp(camera.frame.w & 0xffu, 1u, 12u));
    for (int bounce = 0; bounce < maxBounces; ++bounce)
    {
        rayQueryEXT query;
        uint rayFlags = (camera.frame.w & 0x100u) != 0u
            ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT;
        rayQueryInitializeEXT(query, scene, rayFlags,
            0xff, rayOrigin, 0.001, rayDirection, 10000.0);
        while (rayQueryProceedEXT(query)) {
            if (rayQueryGetIntersectionTypeEXT(query, false) ==
                gl_RayQueryCandidateIntersectionTriangleEXT) {
                uint candidate = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
                vec2 candidateBarycentrics =
                    rayQueryGetIntersectionBarycentricsEXT(query, false);
                if (randomFloat(rng) <= surfaceOpacity(candidate, candidateBarycentrics)) {
                    rayQueryConfirmIntersectionEXT(query);
                }
            }
        }

        if (rayQueryGetIntersectionTypeEXT(query, true) ==
            gl_RayQueryCommittedIntersectionNoneEXT)
        {
            if (environmentOnMiss) {
                float misWeight = previousWasDelta ? 1.0 : powerHeuristic(
                    previousBsdfPdf, environmentLightPdf(rayDirection));
                radiance += throughput * misWeight * vec3(illuminantSpectrum(
                    environment(rayDirection), wavelength));
            }
            break;
        }

        uint primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        uint i0 = indices[primitive * 3u + 0u];
        uint i1 = indices[primitive * 3u + 1u];
        uint i2 = indices[primitive * 3u + 2u];
        vec3 p0 = positions[i0].xyz;
        vec3 p1 = positions[i1].xyz;
        vec3 p2 = positions[i2].xyz;
        vec3 geometricNormal = normalize(cross(p1 - p0, p2 - p0));
        bool frontFace = dot(geometricNormal, rayDirection) < 0.0;
        vec3 orientedGeometricNormal = frontFace ? geometricNormal : -geometricNormal;

        vec2 barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
        vec3 authoredNormal = surfaceShadingNormal(
            primitive, barycentrics, geometricNormal);
        vec3 normal = frontFace ? authoredNormal : -authoredNormal;
        vec2 uv0 = texcoords[primitive * 3u + 0u];
        vec2 uv1 = texcoords[primitive * 3u + 1u];
        vec2 uv2 = texcoords[primitive * 3u + 2u];
        vec2 surfaceUv = surfaceTextureCoordinates(primitive, barycentrics);

        float distance = rayQueryGetIntersectionTEXT(query, true);
        vec3 hit = rayOrigin + rayDirection * distance;
        if (insideMedium) {
            if (mediumScattering > 0.0) {
                float scatterDistance = -log(max(
                    1.0 - randomFloat(rng), 1e-6)) / mediumScattering;
                if (scatterDistance < distance) {
                    throughput *= exp(-mediumAbsorption * scatterDistance);
                    rayOrigin += rayDirection * scatterDistance;
                    rayDirection = sampleHenyeyGreenstein(
                        rayDirection, mediumAnisotropy, rng);
                    environmentOnMiss = true;
                    previousWasDelta = true;
                    previousBsdfPdf = 0.0;
                    continue;
                }
            }
            throughput *= exp(-mediumAbsorption * distance);
        }
        GpuMaterial material = materials[triangleMaterials[primitive]];
        vec3 baseColorRgb = sampleMaterialTexture(
            material.textureIndices0.x, surfaceUv,
            vec4(material.baseColorMetalness.rgb, 1.0)).rgb;
        vec3 baseColor = vec3(reflectanceSpectrum(baseColorRgb, wavelength));
        float metalness = clamp(sampleMaterialTexture(
            material.textureIndices0.y, surfaceUv,
            vec4(material.baseColorMetalness.a)).r, 0.0, 1.0);
        vec2 roughness = clamp(sampleMaterialTexture(
            material.textureIndices0.z, surfaceUv,
            vec4(material.emissionRoughness.a, material.subsurfaceColor.w,
                 0.0, 1.0)).rg, vec2(0.02), vec2(1.0));
        float diffuseRoughness = clamp(sampleMaterialTexture(
            material.textureIndices4.z, surfaceUv,
            vec4(material.diffuseProperties.x)).r, 0.0, 1.0);
        float diffuseWeight = clamp(sampleMaterialTexture(
            material.textureIndices4.w, surfaceUv,
            vec4(material.diffuseProperties.z)).r, 0.0, 1.0);
        uint diffuseModel = uint(material.diffuseProperties.y + 0.5);
        normal = applyNormalMap(material.textureIndices1.y,
                                material.coatWeightRoughnessIor.w > 0.5,
                                material.subsurfaceWeightScale.w,
                                surfaceUv, normal,
                                p0, p1, p2, uv0, uv1, uv2);
        vec3 tangent = surfaceTangent(normal, p0, p1, p2, uv0, uv1, uv2);
        vec3 emissionRgb = material.textureIndices0.w == 0xffffffffu
            ? material.emissionRoughness.rgb
            : sampleMaterialTexture(material.textureIndices0.w, surfaceUv, vec4(0.0)).rgb *
                material.transmissionOpacityIor.w;
        vec3 emission = vec3(illuminantSpectrum(emissionRgb, wavelength));
        radiance += throughput * emission;

        float transmission = clamp(material.transmissionOpacityIor.x, 0.0, 1.0);
        vec3 transmissionColorRgb = material.transmissionColorThinWalled.rgb;
        if (transmission > 0.0) {
            transmissionColorRgb = sampleMaterialTexture(
                material.textureIndices1.z, surfaceUv,
                vec4(transmissionColorRgb, 1.0)).rgb;
        }
        vec3 transmissionColor = vec3(reflectanceSpectrum(
            transmissionColorRgb, wavelength));
        float subsurface = 0.0;
        vec3 diffuseColor = baseColor;
        vec3 subsurfaceColorRgb = material.subsurfaceColor.rgb;
        vec3 subsurfaceRadiusRgb = max(
            material.subsurfaceRadius.rgb, vec3(0.001));
        if (material.subsurfaceWeightScale.x > 0.0 ||
            material.textureIndices2.x != 0xffffffffu) {
            subsurface = clamp(sampleMaterialTexture(
                material.textureIndices2.x, surfaceUv,
                vec4(material.subsurfaceWeightScale.x)).r, 0.0, 1.0);
            if (subsurface > 0.0) {
                subsurfaceColorRgb = sampleMaterialTexture(
                    material.textureIndices2.y, surfaceUv,
                    vec4(material.subsurfaceColor.rgb, 1.0)).rgb;
                subsurfaceRadiusRgb = max(sampleMaterialTexture(
                    material.textureIndices2.z, surfaceUv,
                    vec4(material.subsurfaceRadius.rgb, 1.0)).rgb, vec3(0.001));
                vec3 subsurfaceColor = vec3(reflectanceSpectrum(
                    subsurfaceColorRgb, wavelength));
                diffuseColor = mix(baseColor, subsurfaceColor, subsurface);
            }
        }
        float ior = dispersedIor(
            max(material.transmissionOpacityIor.z, 1.0001),
            material.transmissionMedium.z,
            material.transmissionMedium.w, wavelength);
        float specularWeight = clamp(sampleMaterialTexture(
            material.textureIndices3.x, surfaceUv,
            vec4(material.specularWeightColor.x)).r, 0.0, 1.0);
        vec3 specularColorRgb = sampleMaterialTexture(
            material.textureIndices3.y, surfaceUv,
            vec4(material.specularWeightColor.yzw, 1.0)).rgb;
        vec3 specularColor = vec3(reflectanceSpectrum(
            specularColorRgb, wavelength));
        float dielectricReflectance = (ior - 1.0) / (ior + 1.0);
        dielectricReflectance *= dielectricReflectance;
        vec3 f0 = mix(clamp(vec3(dielectricReflectance * specularWeight) *
                            specularColor, vec3(0.0), vec3(1.0)),
                      baseColor, metalness);
        float coat = 0.0;
        vec2 coatRoughness = vec2(
            material.coatWeightRoughnessIor.y, material.subsurfaceRadius.w);
        vec3 coatF0 = vec3(0.0);
        if (material.coatWeightRoughnessIor.x > 0.0 ||
            material.textureIndices3.z != 0xffffffffu) {
            coat = clamp(sampleMaterialTexture(
                material.textureIndices3.z, surfaceUv,
                vec4(material.coatWeightRoughnessIor.x)).r, 0.0, 1.0);
            if (coat > 0.0) {
                vec3 coatColorRgb = sampleMaterialTexture(
                    material.textureIndices3.w, surfaceUv,
                    vec4(material.coatColor.rgb, 1.0)).rgb;
                vec3 coatColor = vec3(reflectanceSpectrum(
                    coatColorRgb, wavelength));
                coatRoughness = clamp(sampleMaterialTexture(
                    material.textureIndices4.x, surfaceUv,
                    vec4(coatRoughness, 0.0, 1.0)).rg,
                    vec2(0.02), vec2(1.0));
                float coatIor = max(material.coatWeightRoughnessIor.z, 1.0001);
                float coatReflectance = (coatIor - 1.0) / (coatIor + 1.0);
                coatReflectance *= coatReflectance;
                coatF0 = clamp(vec3(coatReflectance) * coatColor,
                               vec3(0.0), vec3(1.0));
            }
        }

        float sheen = clamp(sampleMaterialTexture(
            material.textureIndices1.w, surfaceUv,
            vec4(material.coatColor.w)).r, 0.0, 1.0);
        vec3 sheenColorRgb = sampleMaterialTexture(
            material.textureIndices2.w, surfaceUv,
            vec4(material.sheenColorMode.rgb, 1.0)).rgb;
        vec3 sheenColor = vec3(reflectanceSpectrum(
            sheenColorRgb, wavelength));
        float sheenRoughness = clamp(sampleMaterialTexture(
            material.textureIndices4.y, surfaceUv,
            vec4(material.transmissionScatter.w)).r, 0.01, 1.0);
        bool sheenZeltner = material.sheenColorMode.w > 0.5;

        vec3 viewDirection = normalize(-rayDirection);
        float sheenAlbedo = sheenZeltner
            ? sheenZeltnerDirectionalAlbedo(
                max(dot(normal, viewDirection), 0.0), sheenRoughness)
            : sheenDirectionalAlbedo(
                max(dot(normal, viewDirection), 0.0), sheenRoughness);
        float sheenProbability = clamp(
            sheen * sheenAlbedo * luminance(sheenColor), 0.0, 0.95);
        vec3 coatContribution = coat > 0.0
            ? coat * fresnelSchlick(
                max(dot(normal, viewDirection), 0.0), coatF0)
            : vec3(0.0);
        float coatProbability = clamp(luminance(coatContribution), 0.0, 0.95);
        vec3 baseFresnel = fresnelSchlick(
            max(dot(normal, viewDirection), 0.0), f0);
        vec3 specularContribution = baseFresnel;
        vec3 diffuseContribution = (vec3(1.0) - baseFresnel) *
            diffuseWeight * (1.0 - metalness) * diffuseColor;
        float specularEnergy = luminance(specularContribution);
        float diffuseEnergy = luminance(diffuseContribution);
        float specularProbability = clamp(
            specularEnergy / max(specularEnergy + diffuseEnergy, 1e-5),
            0.05, 0.95);
        uint lightCount = (camera.frame.w >> 16u) & 0xffu;
)glsl"
R"glsl(
        if (lightCount > 0u) {
            uint lightIndex = min(uint(randomFloat(rng) * float(lightCount)),
                                  lightCount - 1u);
            GpuLight light = lights[lightIndex];
            vec3 lightDirection = vec3(0.0, 1.0, 0.0);
            vec3 lightRadiance = vec3(0.0);
            float inversePdf = 0.0;
            float maximumShadowDistance = 10000.0;
            if (light.textureInfo.z == 0u) {
                float lightPdf = 0.0;
                sampleDomeLight(light, rng, lightDirection, lightPdf);
                lightRadiance = sampleDome(light, lightDirection);
                inversePdf = lightPdf > 0.0 ? 1.0 / lightPdf : 0.0;
            } else if (light.textureInfo.z == 5u) {
                float halfAngle = 0.5 * radians(
                    clamp(light.geometry.z, 0.0, 359.999));
                if (halfAngle <= 1e-6) {
                    lightDirection = normalize(light.basisZ.xyz);
                    inversePdf = 1.0;
                } else {
                    float cosineMaximum = cos(halfAngle);
                    float cosineTheta = mix(
                        cosineMaximum, 1.0, randomFloat(rng));
                    float sineTheta = sqrt(max(
                        1.0 - cosineTheta * cosineTheta, 0.0));
                    float phi = 6.28318530718 * randomFloat(rng);
                    lightDirection = normalize(
                        light.basisX.xyz * (cos(phi) * sineTheta) +
                        light.basisY.xyz * (sin(phi) * sineTheta) +
                        light.basisZ.xyz * cosineTheta);
                    // UsdLux distant intensity is an integrated directional
                    // source; angle changes softness without changing energy.
                    inversePdf = 1.0;
                }
                lightRadiance = light.colorIntensity.rgb *
                    light.colorIntensity.a;
            } else {
                vec2 lightUv = vec2(randomFloat(rng), randomFloat(rng));
                vec3 lightPosition = light.position.xyz;
                vec3 lightNormal = -normalize(light.basisZ.xyz);
                if (light.textureInfo.z == 1u) {
                    lightPosition += (lightUv.x - 0.5) * light.axisU.xyz +
                        (lightUv.y - 0.5) * light.axisV.xyz;
                } else if (light.textureInfo.z == 2u) {
                    float diskRadius = sqrt(lightUv.x);
                    float diskPhi = 6.28318530718 * lightUv.y;
                    lightPosition += diskRadius *
                        (cos(diskPhi) * light.axisU.xyz +
                         sin(diskPhi) * light.axisV.xyz);
                } else if (light.textureInfo.z == 3u) {
                    lightNormal = uniformSphere(rng);
                    lightPosition += light.geometry.x * lightNormal;
                } else if (light.textureInfo.z == 4u) {
                    float cylinderPhi = 6.28318530718 * lightUv.y;
                    lightNormal = normalize(
                        cos(cylinderPhi) * light.basisY.xyz +
                        sin(cylinderPhi) * light.basisZ.xyz);
                    lightPosition += (lightUv.x - 0.5) * light.axisU.xyz +
                        light.geometry.x * lightNormal;
                }
                vec3 toLight = lightPosition - hit;
                float distanceSquared = dot(toLight, toLight);
                float lightDistance = sqrt(max(distanceSquared, 1e-8));
                lightDirection = toLight / lightDistance;
                float emissionCosine = max(
                    dot(lightNormal, -lightDirection), 0.0);
                if (emissionCosine > 0.0 && light.controls.w > 0.0) {
                    float shapingCosine = max(dot(
                        normalize(light.basisZ.xyz), lightDirection), 0.0);
                    float focus = pow(shapingCosine,
                                      max(light.shaping.x, 0.0));
                    vec3 focusColor = mix(light.focusTint.rgb, vec3(1.0), focus);
                    vec3 textureValue = light.textureInfo.z != 1u ||
                        light.textureInfo.x == 0xffffffffu
                        ? vec3(1.0)
                        : texture(materialTextures[
                            nonuniformEXT(light.textureInfo.x)], lightUv).rgb;
                    lightRadiance = light.colorIntensity.rgb *
                        light.colorIntensity.a * textureValue * focusColor *
                        shapingConeFactor(light, shapingCosine);
                    inversePdf = light.controls.w * emissionCosine /
                        max(distanceSquared, 1e-8);
                    maximumShadowDistance = max(lightDistance - 0.003, 0.001);
                }
            }
            if (inversePdf > 0.0) {
                vec3 direct = evaluateDirectSurface(
                    normal, tangent, viewDirection, lightDirection,
                    diffuseColor, transmission, subsurface,
                    diffuseWeight, diffuseRoughness, diffuseModel, metalness,
                    roughness, f0, coat, coatRoughness, coatF0,
                    sheen, sheenColor, sheenRoughness, sheenZeltner,
                    light.controls.x, light.controls.y);
                if (any(greaterThan(direct, vec3(0.0)))) {
                    vec3 visibility = shadowVisibility(
                        hit + orientedGeometricNormal * 0.002,
                        lightDirection, maximumShadowDistance, light);
                    vec3 spectralLight = vec3(illuminantSpectrum(
                        lightRadiance, wavelength));
                    vec3 spectralVisibility = vec3(reflectanceSpectrum(
                        visibility, wavelength));
                    float misWeight = light.textureInfo.z == 0u
                        ? powerHeuristic(environmentLightPdf(lightDirection),
                            surfaceReflectionPdf(
                                normal, tangent, viewDirection, lightDirection,
                                roughness, coatRoughness, sheenProbability,
                                coatProbability, specularProbability))
                        : 1.0;
                    radiance += throughput * direct * spectralLight *
                        spectralVisibility * inversePdf * float(lightCount) *
                        misWeight;
                }
            }
        } else {
            // Default to an oblique 75-degree sun when usdrecord's camera
            // light is disabled and the stage authors no lights of its own.
            bool zUp = camera.vertical.w > 0.5;
            vec3 up = zUp ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
            vec3 azimuth = zUp
                ? normalize(vec3(1.0, 1.0, 0.0))
                : normalize(vec3(1.0, 0.0, 1.0));
            const vec3 sunDirection = normalize(
                up * 0.9659258 + azimuth * 0.2588190);
            vec3 direct = evaluateDirectSurface(
                normal, tangent, viewDirection, sunDirection, diffuseColor,
                transmission, subsurface, diffuseWeight, diffuseRoughness,
                diffuseModel, metalness, roughness, f0,
                coat, coatRoughness, coatF0,
                sheen, sheenColor, sheenRoughness, sheenZeltner, 1.0, 1.0);
            if (any(greaterThan(direct, vec3(0.0)))) {
                vec3 visibility = shadowVisibility(
                    hit + orientedGeometricNormal * 0.002,
                    sunDirection, 10000.0, lights[0]);
                const vec3 sunRadiance = vec3(3.45575, 3.14159, 2.82743);
                vec3 spectralSun = vec3(illuminantSpectrum(
                    sunRadiance, wavelength));
                vec3 spectralVisibility = vec3(reflectanceSpectrum(
                    visibility, wavelength));
                radiance += throughput * direct * spectralSun * spectralVisibility;
            }
            vec3 skyDirection = cosineHemisphere(normal, rng);
            vec3 skyDirect = evaluateDirectSurface(
                normal, tangent, viewDirection, skyDirection, diffuseColor,
                transmission, subsurface, diffuseWeight, diffuseRoughness,
                diffuseModel, metalness, roughness, f0,
                coat, coatRoughness, coatF0,
                sheen, sheenColor, sheenRoughness, sheenZeltner, 1.0, 0.0);
            if (any(greaterThan(skyDirect, vec3(0.0)))) {
                vec3 visibility = shadowVisibility(
                    hit + orientedGeometricNormal * 0.002,
                    skyDirection, 10000.0, lights[0]);
                vec3 spectralSky = vec3(illuminantSpectrum(
                    environment(skyDirection), wavelength));
                vec3 spectralVisibility = vec3(reflectanceSpectrum(
                    visibility, wavelength));
                radiance += throughput * skyDirect * spectralSky *
                    spectralVisibility * (3.14159265359 /
                    max(dot(normal, skyDirection), 1e-4));
            }
        }

        if (randomFloat(rng) < transmission) {
            float eta = frontFace ? 1.0 / ior : ior;
            float cosTheta = min(dot(-rayDirection, normal), 1.0);
            float r0 = (1.0 - ior) / (1.0 + ior);
            r0 *= r0;
            float fresnel = r0 + (1.0 - r0) * pow(1.0 - cosTheta, 5.0);
            vec3 refracted = refract(rayDirection, normal, eta);
            bool totalInternalReflection = dot(refracted, refracted) < 1e-8;
            bool thinWalled = material.transmissionColorThinWalled.w > 0.5;
            bool refractedThroughBoundary = false;
            if (totalInternalReflection || randomFloat(rng) < fresnel) {
                rayDirection = reflect(rayDirection, normal);
            } else {
                rayDirection = thinWalled ? rayDirection : normalize(refracted);
                refractedThroughBoundary = !thinWalled;
            }
            float transmissionDepth = max(material.transmissionMedium.x, 0.0);
            throughput *= transmissionDepth > 0.0
                ? vec3(1.0) : transmissionColor;
            if (refractedThroughBoundary) {
                if (frontFace) {
                    float spectralTransmission = max(
                        transmissionColor.r, 0.001);
                    mediumAbsorption = transmissionDepth > 0.0
                        ? -log(spectralTransmission) / transmissionDepth : 0.0;
                    float spectralScatter = max(reconstructRgb(
                        material.transmissionScatter.rgb, wavelength), 0.0);
                    mediumScattering = transmissionDepth > 0.0
                        ? spectralScatter / transmissionDepth : spectralScatter;
                    mediumAnisotropy = material.transmissionMedium.y;
                    insideMedium = true;
                } else {
                    insideMedium = false;
                    mediumAbsorption = 0.0;
                    mediumScattering = 0.0;
                }
            }
            rayOrigin = hit + rayDirection * 0.002;
            environmentOnMiss = true;
            previousWasDelta = true;
            previousBsdfPdf = 0.0;
            continue;
        }

        environmentOnMiss = false;
        rayOrigin = hit + orientedGeometricNormal * 0.002;
        if (sheenProbability > 0.0 && randomFloat(rng) < sheenProbability) {
            rayDirection = cosineHemisphere(normal, rng);
            float sheenPdf = max(dot(normal, rayDirection), 0.0) /
                3.14159265359;
            float response = sheenResponse(
                normal, viewDirection, rayDirection,
                sheenRoughness, sheenZeltner);
            throughput *= sheen * sheenColor * response /
                max(sheenProbability * sheenPdf, 1e-6);
            environmentOnMiss = true;
        } else {
            throughput *= (1.0 - sheen * sheenAlbedo) /
                max(1.0 - sheenProbability, 1e-4);
            if (coatProbability > 0.0 && randomFloat(rng) < coatProbability) {
                vec3 incident = rayDirection;
                rayDirection = sampleGGXReflectionAnisotropic(
                    rayDirection, normal, tangent, coatRoughness, rng);
                throughput *= coat * ggxSampleWeightAnisotropic(
                    incident, rayDirection, normal, tangent,
                    coatRoughness, coatF0) / coatProbability;
                environmentOnMiss = max(coatRoughness.x, coatRoughness.y) < 0.2;
            } else {
                throughput *= (vec3(1.0) - coatContribution) /
                    max(1.0 - coatProbability, 1e-4);
                if (randomFloat(rng) < specularProbability) {
                    vec3 incident = rayDirection;
                    rayDirection = sampleGGXReflectionAnisotropic(
                        rayDirection, normal, tangent, roughness, rng);
                    throughput *= ggxSampleWeightAnisotropic(
                        incident, rayDirection, normal, tangent,
                        roughness, f0) / specularProbability;
                    environmentOnMiss = max(roughness.x, roughness.y) < 0.2;
                } else {
                    throughput *= diffuseContribution /
                        max(1.0 - specularProbability, 1e-4);
                    if (subsurface > 0.0 && randomFloat(rng) < subsurface) {
                        float radius = max(reconstructRgb(
                            subsurfaceRadiusRgb, wavelength) *
                            max(material.subsurfaceWeightScale.y, 0.0), 0.001);
                        float albedo = reflectanceSpectrum(
                            subsurfaceColorRgb, wavelength);
                        vec3 exitOrigin;
                        vec3 exitDirection;
                        float attenuation;
                        if (randomWalkSubsurface(
                            hit, orientedGeometricNormal, radius, albedo,
                            material.subsurfaceWeightScale.z, rng,
                            exitOrigin, exitDirection, attenuation)) {
                            throughput *= attenuation;
                            rayOrigin = exitOrigin;
                            rayDirection = exitDirection;
                            environmentOnMiss = true;
                            previousWasDelta = true;
                            previousBsdfPdf = 0.0;
                            continue;
                        }
                    }
                    rayDirection = cosineHemisphere(normal, rng);
                    float lambertResponse = max(dot(normal, rayDirection), 0.0) /
                        3.14159265359;
                    throughput *= evaluateDiffuseReflection(
                        normal, viewDirection, rayDirection, diffuseColor,
                        diffuseRoughness, diffuseModel) /
                        max(lambertResponse, 1e-6);
                }
            }
        }
        if (lightCount > 0u) {
            previousBsdfPdf = surfaceReflectionPdf(
                normal, tangent, viewDirection, rayDirection,
                roughness, coatRoughness, sheenProbability,
                coatProbability, specularProbability);
            previousWasDelta = false;
            environmentOnMiss = true;
        }
        if (bounce >= 2) {
            float survival = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.1, 0.95);
            if (randomFloat(rng) > survival) break;
            throughput /= survival;
        }
    }

    spectralSampleRadiance += spectralSensor(wavelength) * radiance.r;
    }
    batchRadiance += spectralSampleRadiance / 3.0;
    }
    vec3 previous = pixels[index].rgb;
    float previousWeight = float(camera.frame.z);
    float totalWeight = previousWeight + float(sampleCount);
    pixels[index] = vec4(
        (previous * previousWeight + batchRadiance) / totalWeight, 1.0);
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

struct GpuMaterial {
    std::array<float, 4> baseColorMetalness;
    std::array<float, 4> emissionRoughness;
    std::array<float, 4> transmissionOpacityIor;
    std::array<float, 4> transmissionColorThinWalled;
    std::array<float, 4> subsurfaceWeightScale;
    std::array<float, 4> subsurfaceColor;
    std::array<float, 4> subsurfaceRadius;
    std::array<float, 4> specularWeightColor;
    std::array<float, 4> coatWeightRoughnessIor;
    std::array<float, 4> coatColor;
    std::array<float, 4> transmissionMedium;
    std::array<float, 4> transmissionScatter;
    std::array<float, 4> sheenColorMode;
    std::array<float, 4> diffuseProperties;
    std::array<std::uint32_t, 4> textureIndices0;
    std::array<std::uint32_t, 4> textureIndices1;
    std::array<std::uint32_t, 4> textureIndices2;
    std::array<std::uint32_t, 4> textureIndices3;
    std::array<std::uint32_t, 4> textureIndices4;
};

struct GpuLight {
    std::array<float, 4> colorIntensity;
    std::array<float, 4> controls;
    std::array<float, 4> position;
    std::array<float, 4> axisU;
    std::array<float, 4> axisV;
    std::array<float, 4> basisX;
    std::array<float, 4> basisY;
    std::array<float, 4> basisZ;
    std::array<float, 4> shaping;
    std::array<float, 4> focusTint;
    std::array<float, 4> shadow;
    std::array<float, 4> shadowColor;
    std::array<float, 4> geometry;
    std::array<std::uint32_t, 4> textureInfo;
};

static_assert(sizeof(GpuLight) == 14U * 16U);

float SrgbToLinear(float value)
{
    return value <= 0.04045F
        ? value / 12.92F
        : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

SceneTexture BuildLatLongImportanceTexture(const SceneTexture& source)
{
    SceneTexture result;
    if (source.width == 0U || source.height == 0U ||
        (source.rgba.empty() && source.rgbaFloat.empty())) {
        return result;
    }

    const std::size_t pixelCount = static_cast<std::size_t>(source.width) *
        static_cast<std::size_t>(source.height);
    std::vector<float> luminance(pixelCount, 0.0F);
    std::vector<double> weights(pixelCount, 0.0);
    std::vector<double> rowWeights(source.height, 0.0);
    constexpr double pi = 3.14159265358979323846;
    const double deltaPhi = 2.0 * pi / static_cast<double>(source.width);
    double totalWeight = 0.0;
    for (std::uint32_t y = 0U; y < source.height; ++y) {
        const double v0 = static_cast<double>(y) / source.height;
        const double v1 = static_cast<double>(y + 1U) / source.height;
        const double cosine0 = std::cos(pi * (1.0 - v0));
        const double cosine1 = std::cos(pi * (1.0 - v1));
        const double solidAngle = deltaPhi * std::abs(cosine1 - cosine0);
        for (std::uint32_t x = 0U; x < source.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * source.width + x;
            const std::size_t offset = pixel * 4U;
            std::array<float, 3> rgb{};
            for (std::size_t channel = 0U; channel < 3U; ++channel) {
                float value = !source.rgbaFloat.empty()
                    ? source.rgbaFloat[offset + channel]
                    : static_cast<float>(source.rgba[offset + channel]) / 255.0F;
                if (source.rgbaFloat.empty() && source.srgb) value = SrgbToLinear(value);
                rgb[channel] = std::isfinite(value) ? std::max(value, 0.0F) : 0.0F;
            }
            const float value = 0.2126F * rgb[0] + 0.7152F * rgb[1] +
                0.0722F * rgb[2];
            luminance[pixel] = value;
            weights[pixel] = static_cast<double>(value) * solidAngle;
            rowWeights[y] += weights[pixel];
            totalWeight += weights[pixel];
        }
    }
    if (!(totalWeight > 0.0) || !std::isfinite(totalWeight)) return result;

    result.id = source.id + "#latlong-importance";
    result.sourcePath = source.sourcePath;
    result.width = source.width;
    result.height = source.height;
    result.rgbaFloat.resize(pixelCount * 4U);
    double marginalCdf = 0.0;
    for (std::uint32_t y = 0U; y < source.height; ++y) {
        marginalCdf += rowWeights[y] / totalWeight;
        double conditionalCdf = 0.0;
        for (std::uint32_t x = 0U; x < source.width; ++x) {
            const std::size_t pixel = static_cast<std::size_t>(y) * source.width + x;
            conditionalCdf += rowWeights[y] > 0.0
                ? weights[pixel] / rowWeights[y]
                : 1.0 / static_cast<double>(source.width);
            const std::size_t offset = pixel * 4U;
            result.rgbaFloat[offset] = static_cast<float>(
                x + 1U == source.width ? 1.0 : conditionalCdf);
            result.rgbaFloat[offset + 1U] = static_cast<float>(
                y + 1U == source.height ? 1.0 : marginalCdf);
            result.rgbaFloat[offset + 2U] =
                static_cast<float>(static_cast<double>(luminance[pixel]) / totalWeight);
            result.rgbaFloat[offset + 3U] = 1.0F;
        }
    }
    return result;
}

struct GpuTexcoord { float u, v; };

using GpuNormal = GpuVertex;

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
          queueFamily(context.ComputeQueueFamily()), shaderCache(&cache)
    {
        CreateCommandPool();
        CreateSubmissionResources();
        CreateSampler();
        CreateDescriptors();
        CreatePipeline(cache, ShadingMode::Fused);
        uniform = CreateBuffer(sizeof(CameraUniform), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
    }

    ~Impl()
    {
        if (device == VK_NULL_HANDLE) return;
        vkDeviceWaitIdle(device);
        DestroyScene();
        DestroyBuffer(output);
        DestroyBuffer(readback);
        DestroyBuffer(uniform);
        if (textureSampler) vkDestroySampler(device, textureSampler, nullptr);
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        if (descriptorPool) vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        if (descriptorLayout) vkDestroyDescriptorSetLayout(device, descriptorLayout, nullptr);
        if (submissionFence) vkDestroyFence(device, submissionFence, nullptr);
        if (commandPool) vkDestroyCommandPool(device, commandPool, nullptr);
    }

    struct Buffer {
        VkBuffer handle{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkDeviceSize size{0};
        void* mapped{nullptr};
    };

    struct Texture {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        VkImageView view{VK_NULL_HANDLE};
    };

    std::uint32_t FindMemoryType(
        std::uint32_t typeBits, VkMemoryPropertyFlags properties) const
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memoryProperties);
        for (std::uint32_t index = 0; index < memoryProperties.memoryTypeCount; ++index) {
            if ((typeBits & (1U << index)) != 0U &&
                (memoryProperties.memoryTypes[index].propertyFlags & properties) == properties) {
                return index;
            }
        }
        throw std::runtime_error("no compatible Vulkan memory type");
    }

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

        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = address ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT : 0U;
        const VkMemoryAllocateInfo allocation{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = address ? &flags : nullptr,
            .allocationSize = requirements.size,
            .memoryTypeIndex = FindMemoryType(requirements.memoryTypeBits, properties),
        };
        Check(vkAllocateMemory(device, &allocation, nullptr, &result.memory), "vkAllocateMemory");
        Check(vkBindBufferMemory(device, result.handle, result.memory, 0), "vkBindBufferMemory");
        if ((properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0U) {
            Check(vkMapMemory(device, result.memory, 0, size, 0, &result.mapped), "vkMapMemory");
        }
        return result;
    }

    Buffer CreateDeviceBufferWithData(
        const void* data,
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        bool address,
        VkPipelineStageFlags destinationStages,
        VkAccessFlags destinationAccess)
    {
        Buffer destination = CreateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, address);
        Buffer staging;
        try {
            staging = CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                false);
            std::memcpy(staging.mapped, data, static_cast<std::size_t>(size));
            Submit([&](VkCommandBuffer currentCommand) {
                const VkBufferCopy copy{.size = size};
                vkCmdCopyBuffer(currentCommand, staging.handle, destination.handle, 1, &copy);
                const VkBufferMemoryBarrier ready{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = destinationAccess,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = destination.handle,
                    .offset = 0,
                    .size = destination.size,
                };
                vkCmdPipelineBarrier(currentCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    destinationStages, 0, 0, nullptr, 1, &ready, 0, nullptr);
            });
            DestroyBuffer(staging);
            return destination;
        } catch (...) {
            DestroyBuffer(staging);
            DestroyBuffer(destination);
            throw;
        }
    }

    Texture CreateTexture(const SceneTexture& source)
    {
        Texture result;
        const bool floatingPoint = !source.rgbaFloat.empty();
        const VkFormat format = floatingPoint
            ? VK_FORMAT_R32G32B32A32_SFLOAT
            : (source.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {source.width, source.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        Check(vkCreateImage(device, &imageInfo, nullptr, &result.image), "vkCreateImage");
        try {
            VkMemoryRequirements requirements{};
            vkGetImageMemoryRequirements(device, result.image, &requirements);
            const VkMemoryAllocateInfo allocation{
                .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                .allocationSize = requirements.size,
                .memoryTypeIndex = FindMemoryType(
                    requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
            };
            Check(vkAllocateMemory(device, &allocation, nullptr, &result.memory),
                  "vkAllocateMemory(texture)");
            Check(vkBindImageMemory(device, result.image, result.memory, 0),
                  "vkBindImageMemory");

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = result.image,
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = format,
                .components = {
                    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY},
                .subresourceRange = {
                    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            Check(vkCreateImageView(device, &viewInfo, nullptr, &result.view),
                  "vkCreateImageView");

            const void* pixelData = floatingPoint
                ? static_cast<const void*>(source.rgbaFloat.data())
                : static_cast<const void*>(source.rgba.data());
            const VkDeviceSize pixelBytes = floatingPoint
                ? source.rgbaFloat.size() * sizeof(float)
                : source.rgba.size();
            Buffer staging = CreateBuffer(pixelBytes,
                VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                false);
            std::memcpy(staging.mapped, pixelData, static_cast<std::size_t>(pixelBytes));
            try {
                Submit([&](VkCommandBuffer command) {
                VkImageMemoryBarrier toTransfer{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = result.image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                };
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                    1, &toTransfer);
                const VkBufferImageCopy copy{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {source.width, source.height, 1},
                };
                vkCmdCopyBufferToImage(command, staging.handle, result.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                VkImageMemoryBarrier toShader = toTransfer;
                toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                    1, &toShader);
                });
            } catch (...) {
                DestroyBuffer(staging);
                throw;
            }
            DestroyBuffer(staging);
            return result;
        } catch (...) {
            if (result.view) vkDestroyImageView(device, result.view, nullptr);
            if (result.image) vkDestroyImage(device, result.image, nullptr);
            if (result.memory) vkFreeMemory(device, result.memory, nullptr);
            throw;
        }
    }

    void DestroyTexture(Texture& texture)
    {
        if (texture.view) vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image) vkDestroyImage(device, texture.image, nullptr);
        if (texture.memory) vkFreeMemory(device, texture.memory, nullptr);
        texture = {};
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
        Check(vkWaitForFences(device, 1, &submissionFence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences");
        Check(vkResetFences(device, 1, &submissionFence), "vkResetFences");
        Check(vkResetCommandBuffer(command, 0), "vkResetCommandBuffer");
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
        Check(vkQueueSubmit(queue, 1, &submit, submissionFence), "vkQueueSubmit");
        Check(vkWaitForFences(device, 1, &submissionFence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences");
    }

    void CreateCommandPool()
    {
        const VkCommandPoolCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                     VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = queueFamily,
        };
        Check(vkCreateCommandPool(device, &info, nullptr, &commandPool), "vkCreateCommandPool");
    }

    void CreateSubmissionResources()
    {
        const VkCommandBufferAllocateInfo allocate{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = commandPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        Check(vkAllocateCommandBuffers(device, &allocate, &command),
              "vkAllocateCommandBuffers");
        const VkFenceCreateInfo fenceInfo{
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        Check(vkCreateFence(device, &fenceInfo, nullptr, &submissionFence),
              "vkCreateFence");
    }

    void CreateSampler()
    {
        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .mipLodBias = 0.0F,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 1.0F,
            .compareEnable = VK_FALSE,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .minLod = 0.0F,
            .maxLod = 0.0F,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        Check(vkCreateSampler(device, &info, nullptr, &textureSampler),
              "vkCreateSampler");
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
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                kMaxMaterialTextures, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        };
        std::array<VkDescriptorBindingFlags, 11> bindingFlags{};
        bindingFlags[8] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        const VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount = static_cast<std::uint32_t>(bindingFlags.size()),
            .pBindingFlags = bindingFlags.data(),
        };
        const VkDescriptorSetLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = &flagsInfo,
            .bindingCount = static_cast<std::uint32_t>(bindings.size()),
            .pBindings = bindings.data(),
        };
        Check(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorLayout),
              "vkCreateDescriptorSetLayout");
        const std::array poolSizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 kMaxMaterialTextures},
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

    void CreatePipeline(ShaderCache& cache, ShadingMode mode)
    {
        if (mode == ShadingMode::RasterPreview) {
            throw std::invalid_argument(
                "raster preview requires the MaterialX graphics backend");
        }
        GlslCompileOptions options;
        const std::string modeName(ShadingModeName(mode));
        options.generatorVersion = "hdCodex.pathtracer.spectral.v16." + modeName;
        options.materialAbi = "hdcodex.pathtracer.materialx-surface.v10." + modeName;
        if (mode == ShadingMode::Modular) {
            options.generateDebugInfo = true;
            options.optimization = GlslCompileOptions::Optimization::None;
        }
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

    void SetShadingMode(ShadingMode mode)
    {
        if (mode == shadingMode) return;
        if (!shaderCache) throw std::runtime_error("shader cache is unavailable");
        Check(vkDeviceWaitIdle(device), "vkDeviceWaitIdle(shading mode)");
        if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (pipelineLayout) vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        pipeline = VK_NULL_HANDLE;
        pipelineLayout = VK_NULL_HANDLE;
        CreatePipeline(*shaderCache, mode);
        shadingMode = mode;
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
        DestroyBuffer(triangleMaterialBuffer);
        DestroyBuffer(materialBuffer);
        DestroyBuffer(texcoordBuffer);
        DestroyBuffer(normalBuffer);
        DestroyBuffer(lightBuffer);
        for (Texture& texture : textures) DestroyTexture(texture);
        textures.clear();
        lightCount = 0U;
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
        sceneOpaque = std::all_of(snapshot->materials.begin(), snapshot->materials.end(),
            [](const SceneMaterial& material) {
                return material.opacity >= 0.999F && material.opacityTexture.empty();
            });

        std::vector<GpuVertex> vertices;
        std::vector<std::uint32_t> indices;
        std::vector<std::uint32_t> triangleMaterials;
        std::vector<GpuTexcoord> texcoords;
        std::vector<GpuNormal> normals;
        std::vector<GpuMaterial> materials;
        std::vector<GpuLight> lights;
        std::map<std::string, std::uint32_t, std::less<>> textureIndices;
        std::map<std::string, const SceneTexture*, std::less<>> textureSources;
        std::map<std::string, std::vector<const SceneTexture*>, std::less<>> udimSets;
        textures.reserve(std::min<std::size_t>(
            snapshot->textures.size(), kMaxMaterialTextures));
        for (const SceneTexture& texture : snapshot->textures) {
            if (!texture.udimSetId.empty() && texture.udimTile != 0U) {
                udimSets[texture.udimSetId].push_back(&texture);
                continue;
            }
            if (textures.size() >= kMaxMaterialTextures) continue;
            textureSources[texture.id] = &texture;
            textureIndices[texture.id] = static_cast<std::uint32_t>(textures.size());
            textures.push_back(CreateTexture(texture));
        }
        for (auto& [setId, tiles] : udimSets) {
            std::sort(tiles.begin(), tiles.end(),
                [](const SceneTexture* left, const SceneTexture* right) {
                    return left->udimTile < right->udimTile;
                });
            const std::uint32_t base = static_cast<std::uint32_t>(textures.size());
            std::uint32_t presentTiles = 0U;
            for (const SceneTexture* tile : tiles) {
                if (textures.size() >= kMaxMaterialTextures) break;
                if (tile->udimTile < 1001U || tile->udimTile > 1023U) continue;
                presentTiles |= 1U << (tile->udimTile - 1001U);
                textures.push_back(CreateTexture(*tile));
            }
            if (presentTiles != 0U) {
                textureIndices[setId] = kUdimTextureFlag |
                    ((presentTiles & kUdimTileMask) << 8U) | base;
            }
        }
        const auto textureIndex = [&textureIndices](const std::string& id) {
            const auto found = textureIndices.find(id);
            return found == textureIndices.end() ? kMissingTexture : found->second;
        };
        std::map<std::string, std::uint32_t, std::less<>> domeImportanceIndices;
        const auto domeImportanceIndex = [&](const SceneLight& light) {
            if (light.type != SceneLightType::Dome ||
                (light.textureFormat != DomeTextureFormat::Automatic &&
                 light.textureFormat != DomeTextureFormat::LatLong) ||
                textureIndex(light.texture) == kMissingTexture) {
                return kMissingTexture;
            }
            const auto cached = domeImportanceIndices.find(light.texture);
            if (cached != domeImportanceIndices.end()) return cached->second;
            const auto source = textureSources.find(light.texture);
            if (source == textureSources.end() ||
                textures.size() >= kMaxMaterialTextures) {
                return kMissingTexture;
            }
            SceneTexture importance = BuildLatLongImportanceTexture(*source->second);
            if (importance.rgbaFloat.empty()) return kMissingTexture;
            const std::uint32_t index = static_cast<std::uint32_t>(textures.size());
            textures.push_back(CreateTexture(importance));
            domeImportanceIndices.emplace(light.texture, index);
            return index;
        };

        lights.reserve(std::min<std::size_t>(snapshot->lights.size(), kMaxLights));
        for (const SceneLight& light : snapshot->lights) {
            if (!light.visible || lights.size() >= kMaxLights) continue;
            float radianceScale = light.intensity * std::exp2(light.exposure);
            if (light.type != SceneLightType::Dome &&
                light.type != SceneLightType::Distant && light.normalize) {
                radianceScale /= std::max(light.area, 1e-8F);
            }
            lights.push_back({
                {light.color[0] * light.temperatureColor[0],
                 light.color[1] * light.temperatureColor[1],
                 light.color[2] * light.temperatureColor[2], radianceScale},
                {light.diffuse, light.specular,
                 static_cast<float>(light.type),
                 std::max(light.area, 0.0F)},
                {light.position[0], light.position[1], light.position[2], 0.0F},
                {light.axisU[0], light.axisU[1], light.axisU[2], 0.0F},
                {light.axisV[0], light.axisV[1], light.axisV[2], 0.0F},
                {light.basisX[0], light.basisX[1], light.basisX[2], 0.0F},
                {light.basisY[0], light.basisY[1], light.basisY[2], 0.0F},
                {light.basisZ[0], light.basisZ[1], light.basisZ[2], 0.0F},
                {light.shapingFocus, light.shapingConeAngle,
                 light.shapingConeSoftness, 0.0F},
                {light.shapingFocusTint[0], light.shapingFocusTint[1],
                 light.shapingFocusTint[2], 0.0F},
                {light.shadowEnable ? 1.0F : 0.0F, light.shadowDistance,
                 light.shadowFalloff, light.shadowFalloffGamma},
                {light.shadowColor[0], light.shadowColor[1],
                 light.shadowColor[2], 0.0F},
                {light.radius, light.length, light.angle, 0.0F},
                {textureIndex(light.texture),
                 static_cast<std::uint32_t>(light.textureFormat),
                 static_cast<std::uint32_t>(light.type),
                 domeImportanceIndex(light)},
            });
        }
        lightCount = static_cast<std::uint32_t>(lights.size());
        if (lights.empty()) {
            lights.emplace_back();
            lights.back().shadow = {1.0F, -1.0F, -1.0F, 1.0F};
        }

        const auto defaultMaterial = [](const std::array<float, 3>& color) {
            return GpuMaterial{
                {color[0], color[1], color[2], 0.0F},
                {0.0F, 0.0F, 0.0F, 0.5F},
                {0.0F, 1.0F, 1.5F, 0.0F},
                {1.0F, 1.0F, 1.0F, 0.0F},
                {0.0F, 1.0F, 0.0F, 0.0F},
                {color[0], color[1], color[2], 0.0F},
                {1.0F, 0.2F, 0.1F, 0.0F},
                {1.0F, 1.0F, 1.0F, 1.0F},
                {0.0F, 0.1F, 1.5F, 0.0F},
                {1.0F, 1.0F, 1.0F, 0.0F},
                {0.0F, 0.0F, 0.0F, 20.0F},
                {0.0F, 0.0F, 0.0F, 0.0F},
                {1.0F, 1.0F, 1.0F, 0.0F},
                {0.0F, 0.0F, 1.0F, 0.0F},
                {kMissingTexture, kMissingTexture,
                 kMissingTexture, kMissingTexture},
                {kMissingTexture, kMissingTexture,
                 kMissingTexture, kMissingTexture},
                {kMissingTexture, kMissingTexture,
                 kMissingTexture, kMissingTexture},
                {kMissingTexture, kMissingTexture,
                 kMissingTexture, kMissingTexture},
                {kMissingTexture, kMissingTexture,
                 kMissingTexture, kMissingTexture},
            };
        };
        materials.push_back(defaultMaterial({0.8F, 0.8F, 0.8F}));
        std::map<std::string, std::uint32_t, std::less<>> materialIndices;
        for (const SceneMaterial& material : snapshot->materials) {
            const std::uint32_t index = static_cast<std::uint32_t>(materials.size());
            materialIndices[material.id] = index;
            materials.push_back({
                {material.baseColor[0], material.baseColor[1], material.baseColor[2],
                 material.metalness},
                {material.emission[0], material.emission[1], material.emission[2],
                 material.roughness},
                {material.transmission, material.opacity,
                 material.indexOfRefraction, material.emissionWeight},
                {material.transmissionColor[0], material.transmissionColor[1],
                 material.transmissionColor[2], material.thinWalled ? 1.0F : 0.0F},
                {material.subsurface, material.subsurfaceScale,
                 material.subsurfaceScatterAnisotropy,
                 material.normalTextureScale},
                {material.subsurfaceColor[0], material.subsurfaceColor[1],
                 material.subsurfaceColor[2], material.roughnessV},
                {material.subsurfaceRadius[0], material.subsurfaceRadius[1],
                 material.subsurfaceRadius[2], material.coatRoughnessV},
                {material.specularWeight, material.specularColor[0],
                 material.specularColor[1], material.specularColor[2]},
                {material.coat, material.coatRoughness,
                 material.coatIndexOfRefraction,
                 material.normalTextureFlipY ? 1.0F : 0.0F},
                {material.coatColor[0], material.coatColor[1],
                 material.coatColor[2], material.sheen},
                {material.transmissionDepth,
                 material.transmissionScatterAnisotropy,
                 material.transmissionDispersionScale,
                 material.transmissionDispersionAbbeNumber},
                {material.transmissionScatter[0], material.transmissionScatter[1],
                 material.transmissionScatter[2], material.sheenRoughness},
                {material.sheenColor[0], material.sheenColor[1],
                 material.sheenColor[2], static_cast<float>(material.sheenMode)},
                {material.diffuseRoughness,
                 static_cast<float>(material.diffuseModel),
                 material.diffuseWeight, 0.0F},
                {textureIndex(material.baseColorTexture),
                 textureIndex(material.metalnessTexture),
                 textureIndex(material.roughnessTexture),
                 textureIndex(material.emissionTexture)},
                {textureIndex(material.opacityTexture),
                 textureIndex(material.normalTexture),
                 textureIndex(material.transmissionTexture),
                 textureIndex(material.sheenTexture)},
                {textureIndex(material.subsurfaceTexture),
                 textureIndex(material.subsurfaceColorTexture),
                 textureIndex(material.subsurfaceRadiusTexture),
                 textureIndex(material.sheenColorTexture)},
                {textureIndex(material.specularTexture),
                 textureIndex(material.specularColorTexture),
                 textureIndex(material.coatTexture),
                 textureIndex(material.coatColorTexture)},
                {textureIndex(material.coatRoughnessTexture),
                 textureIndex(material.sheenRoughnessTexture),
                 textureIndex(material.diffuseRoughnessTexture),
                 textureIndex(material.diffuseWeightTexture)},
            });
        }
        std::map<std::array<float, 3>, std::uint32_t> displayColorMaterials;
        for (const SceneMesh& mesh : snapshot->meshes) {
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
                vertices.push_back({mesh.positions[i], mesh.positions[i + 1],
                                    mesh.positions[i + 2], 1.0F});
            }
            const auto foundMaterial = materialIndices.find(mesh.materialId);
            std::uint32_t materialIndex = foundMaterial == materialIndices.end()
                ? 0U : foundMaterial->second;
            if (mesh.materialId.empty()) {
                std::array<float, 3> color = mesh.displayColor;
                for (float& component : color) {
                    component = std::isfinite(component)
                        ? std::clamp(component, 0.0F, 1.0F) : 0.5F;
                }
                const auto found = displayColorMaterials.find(color);
                if (found != displayColorMaterials.end()) {
                    materialIndex = found->second;
                } else {
                    materialIndex = static_cast<std::uint32_t>(materials.size());
                    displayColorMaterials.emplace(color, materialIndex);
                    materials.push_back(defaultMaterial(color));
                }
            }
            for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
                const std::uint32_t i0 = mesh.indices[triangle];
                const std::uint32_t i1 = mesh.indices[triangle + 1];
                const std::uint32_t i2 = mesh.indices[triangle + 2];
                if (i0 < vertices.size() - base && i1 < vertices.size() - base &&
                    i2 < vertices.size() - base) {
                    indices.insert(indices.end(), {base + i0, base + i1, base + i2});
                    const std::size_t triangleIndex = triangle / 3U;
                    std::uint32_t triangleMaterialIndex = materialIndex;
                    if (triangleIndex < mesh.triangleMaterialIds.size() &&
                        mesh.triangleMaterialIds[triangleIndex] != mesh.materialId) {
                        const auto foundTriangleMaterial = materialIndices.find(
                            mesh.triangleMaterialIds[triangleIndex]);
                        triangleMaterialIndex = foundTriangleMaterial ==
                            materialIndices.end() ? 0U : foundTriangleMaterial->second;
                    }
                    triangleMaterials.push_back(triangleMaterialIndex);
                    for (std::size_t corner = 0; corner < 3U; ++corner) {
                        const std::size_t uvOffset = (triangle + corner) * 2U;
                        texcoords.push_back(uvOffset + 1U < mesh.texcoords.size()
                            ? GpuTexcoord{mesh.texcoords[uvOffset],
                                          mesh.texcoords[uvOffset + 1U]}
                            : GpuTexcoord{0.0F, 0.0F});
                        const std::size_t normalOffset = (triangle + corner) * 3U;
                        normals.push_back(normalOffset + 2U < mesh.normals.size()
                            ? GpuNormal{mesh.normals[normalOffset],
                                        mesh.normals[normalOffset + 1U],
                                        mesh.normals[normalOffset + 2U], 0.0F}
                            : GpuNormal{0.0F, 0.0F, 0.0F, 0.0F});
                    }
                }
            }
        }
        if (vertices.empty() || indices.empty()) return;

        const VkBufferUsageFlags inputUsage =
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        constexpr VkPipelineStageFlags geometryStages =
            VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags geometryAccess =
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_SHADER_READ_BIT;
        vertexBuffer = CreateDeviceBufferWithData(vertices.data(),
            vertices.size() * sizeof(GpuVertex), inputUsage, true,
            geometryStages, geometryAccess);
        indexBuffer = CreateDeviceBufferWithData(indices.data(),
            indices.size() * sizeof(std::uint32_t), inputUsage, true,
            geometryStages, geometryAccess);
        triangleMaterialBuffer = CreateDeviceBufferWithData(triangleMaterials.data(),
            triangleMaterials.size() * sizeof(std::uint32_t),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        materialBuffer = CreateDeviceBufferWithData(materials.data(),
            materials.size() * sizeof(GpuMaterial), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            false, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        texcoordBuffer = CreateDeviceBufferWithData(texcoords.data(),
            texcoords.size() * sizeof(GpuTexcoord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            false, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        normalBuffer = CreateDeviceBufferWithData(normals.data(),
            normals.size() * sizeof(GpuNormal), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            false, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        lightBuffer = CreateDeviceBufferWithData(lights.data(),
            lights.size() * sizeof(GpuLight), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
            false, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);

        VkAccelerationStructureGeometryKHR geometry{
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
        geometry.flags = sceneOpaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0U;
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
        instanceBuffer = CreateDeviceBufferWithData(&instance, sizeof(instance), inputUsage,
            true, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
            VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR);

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
        DestroyBuffer(readback);
        output = CreateBuffer(required,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
        readback = CreateBuffer(required, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
        Submit([&](VkCommandBuffer currentCommand) {
            vkCmdFillBuffer(currentCommand, output.handle, 0, output.size, 0U);
            const VkBufferMemoryBarrier toShader{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = output.handle,
                .offset = 0,
                .size = output.size,
            };
            vkCmdPipelineBarrier(currentCommand, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                1, &toShader, 0, nullptr);
        });
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
        const VkDescriptorBufferInfo triangleMaterialInfo{
            triangleMaterialBuffer.handle, 0, triangleMaterialBuffer.size};
        const VkDescriptorBufferInfo materialInfo{
            materialBuffer.handle, 0, materialBuffer.size};
        const VkDescriptorBufferInfo texcoordInfo{
            texcoordBuffer.handle, 0, texcoordBuffer.size};
        const VkDescriptorBufferInfo normalInfo{
            normalBuffer.handle, 0, normalBuffer.size};
        const VkDescriptorBufferInfo lightInfo{
            lightBuffer.handle, 0, lightBuffer.size};
        std::array<VkWriteDescriptorSet, 11> writes{};
        writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &accelerationWrite,
            descriptorSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, nullptr, nullptr, nullptr};
        const std::array infos = {&outputInfo, &vertexInfo, &indexInfo};
        for (std::uint32_t index = 0; index < infos.size(); ++index) {
            writes[index + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                descriptorSet, index + 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                nullptr, infos[index], nullptr};
        }
        writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &uniformInfo, nullptr};
        writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &triangleMaterialInfo, nullptr};
        writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            6, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &materialInfo, nullptr};
        writes[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            7, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &texcoordInfo, nullptr};
        writes[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            9, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &normalInfo, nullptr};
        writes[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
            10, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &lightInfo, nullptr};

        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(textures.size());
        for (const Texture& texture : textures) {
            imageInfos.push_back({
                .sampler = textureSampler,
                .imageView = texture.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            });
        }
        std::uint32_t writeCount = 10;
        if (!imageInfos.empty()) {
            writes[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
                8, 0, static_cast<std::uint32_t>(imageInfos.size()),
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageInfos.data(), nullptr, nullptr};
            writeCount = 11;
        }
        vkUpdateDescriptorSets(device, writeCount,
                               writes.data(), 0, nullptr);
    }

    std::vector<float> Trace(const PathTracerCamera& camera, std::uint32_t width,
                             std::uint32_t height, std::uint32_t sample,
                             std::uint32_t maxBounces,
                             std::uint32_t sampleCount)
    {
        if (!geometryReady || width == 0 || height == 0) return {};
        EnsureOutput(width, height);
        if (!fallbackAxisInitialized) {
            fallbackZUp = std::abs(camera.vertical[2]) >
                std::abs(camera.vertical[1]);
            fallbackAxisInitialized = true;
        }
        CameraUniform data{
            {camera.origin[0], camera.origin[1], camera.origin[2],
             static_cast<float>(std::max(sampleCount, 1U))},
            {camera.lowerLeft[0], camera.lowerLeft[1], camera.lowerLeft[2], 0.0F},
            {camera.horizontal[0], camera.horizontal[1], camera.horizontal[2], 0.0F},
            {camera.vertical[0], camera.vertical[1], camera.vertical[2],
             fallbackZUp ? 1.0F : 0.0F},
            {width, height, sample,
             std::clamp(maxBounces, 1U, kMaxPathBounces) |
                 (sceneOpaque ? kOpaqueSceneFlag : 0U) |
                 (std::min(lightCount, kMaxLights) << kLightCountShift)},
        };
        std::memcpy(uniform.mapped, &data, sizeof(data));
        Submit([&](VkCommandBuffer command) {
            const VkBufferMemoryBarrier toShader{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = output.handle,
                .offset = 0,
                .size = output.size,
            };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                1, &toShader, 0, nullptr);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                pipelineLayout, 0, 1, &descriptorSet, 0, nullptr);
            vkCmdDispatch(command, (width + 7U) / 8U, (height + 7U) / 8U, 1);
            const VkBufferMemoryBarrier toTransfer{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = output.handle,
                .offset = 0,
                .size = output.size,
            };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                1, &toTransfer, 0, nullptr);
            const VkBufferCopy copy{.size = output.size};
            vkCmdCopyBuffer(command, output.handle, readback.handle, 1, &copy);
            const VkBufferMemoryBarrier toHost{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = readback.handle,
                .offset = 0,
                .size = readback.size,
            };
            vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                1, &toHost, 0, nullptr);
        });
        std::vector<float> pixels(static_cast<std::size_t>(width) * height * 4U);
        std::memcpy(pixels.data(), readback.mapped, pixels.size() * sizeof(float));
        return pixels;
    }

    VkPhysicalDevice physical{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    std::uint32_t queueFamily{0};
    VkCommandPool commandPool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
    VkFence submissionFence{VK_NULL_HANDLE};
    VkDescriptorSetLayout descriptorLayout{VK_NULL_HANDLE};
    VkDescriptorPool descriptorPool{VK_NULL_HANDLE};
    VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
    VkPipelineLayout pipelineLayout{VK_NULL_HANDLE};
    VkPipeline pipeline{VK_NULL_HANDLE};
    ShaderCache* shaderCache{nullptr};
    ShadingMode shadingMode{ShadingMode::Fused};
    VkSampler textureSampler{VK_NULL_HANDLE};
    Buffer uniform;
    Buffer output;
    Buffer readback;
    Buffer vertexBuffer;
    Buffer indexBuffer;
    Buffer instanceBuffer;
    Buffer triangleMaterialBuffer;
    Buffer materialBuffer;
    Buffer texcoordBuffer;
    Buffer normalBuffer;
    Buffer lightBuffer;
    Buffer blasStorage;
    Buffer tlasStorage;
    VkAccelerationStructureKHR blas{VK_NULL_HANDLE};
    VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};
    std::vector<Texture> textures;
    bool geometryReady{false};
    bool sceneOpaque{true};
    bool fallbackAxisInitialized{false};
    bool fallbackZUp{false};
    std::uint32_t lightCount{0U};
};

VulkanPathTracer::VulkanPathTracer(VulkanContext& context, ShaderCache& cache)
    : _impl(std::make_unique<Impl>(context, cache)) {}
VulkanPathTracer::~VulkanPathTracer() = default;
void VulkanPathTracer::SetScene(const std::shared_ptr<const SceneSnapshot>& scene)
{
    _impl->BuildScene(scene);
}
void VulkanPathTracer::SetShadingMode(ShadingMode mode)
{
    _impl->SetShadingMode(mode);
}
ShadingMode VulkanPathTracer::GetShadingMode() const noexcept
{
    return _impl->shadingMode;
}
std::vector<float> VulkanPathTracer::Render(
    const PathTracerCamera& camera, std::uint32_t width, std::uint32_t height,
    std::uint32_t sampleIndex, std::uint32_t maxBounces,
    std::uint32_t sampleCount)
{
    return _impl->Trace(camera, width, height, sampleIndex, maxBounces,
                        sampleCount);
}
bool VulkanPathTracer::HasGeometry() const noexcept { return _impl->geometryReady; }

} // namespace hdcodex
