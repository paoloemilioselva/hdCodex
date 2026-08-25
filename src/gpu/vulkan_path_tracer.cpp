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

constexpr std::uint32_t kMaxMaterialTextures = 256U;
constexpr std::uint32_t kMissingTexture = UINT32_MAX;
constexpr std::uint32_t kMaxPathBounces = 5U;
constexpr std::uint32_t kOpaqueSceneFlag = 1U << 8U;

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

vec3 environment(vec3 direction)
{
    float t = 0.5 * (direction.y + 1.0);
    return mix(vec3(0.035, 0.045, 0.065), vec3(0.38, 0.55, 0.85), t);
}

vec4 sampleMaterialTexture(uint textureIndex, vec2 uv, vec4 fallbackValue)
{
    return textureIndex == 0xffffffffu ? fallbackValue
        : texture(materialTextures[nonuniformEXT(textureIndex)], uv);
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

vec3 applyNormalMap(uint textureIndex, vec2 uv, vec3 geometricNormal,
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
    vec3 result = normalize(tangent * mapped.x + bitangent * mapped.y +
                            geometricNormal * mapped.z);
    return dot(result, geometricNormal) < 0.0 ? -result : result;
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

bool occluded(vec3 origin, vec3 direction)
{
    rayQueryEXT shadow;
    uint rayFlags = gl_RayFlagsTerminateOnFirstHitEXT;
    if ((camera.frame.w & 0x100u) != 0u) rayFlags |= gl_RayFlagsOpaqueEXT;
    rayQueryInitializeEXT(shadow, scene,
        rayFlags,
        0xff, origin, 0.001, direction, 10000.0);
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
    return rayQueryGetIntersectionTypeEXT(shadow, true) !=
        gl_RayQueryCommittedIntersectionNoneEXT;
}
)glsl"
R"glsl(
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

    int maxBounces = int(clamp(camera.frame.w & 0xffu, 1u, 5u));
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
        GpuMaterial material = materials[triangleMaterials[primitive]];
        vec3 baseColor = sampleMaterialTexture(
            material.textureIndices0.x, surfaceUv,
            vec4(material.baseColorMetalness.rgb, 1.0)).rgb;
        float metalness = clamp(sampleMaterialTexture(
            material.textureIndices0.y, surfaceUv,
            vec4(material.baseColorMetalness.a)).r, 0.0, 1.0);
        float roughness = clamp(sampleMaterialTexture(
            material.textureIndices0.z, surfaceUv,
            vec4(material.emissionRoughness.a)).r, 0.02, 1.0);
        normal = applyNormalMap(material.textureIndices1.y, surfaceUv, normal,
                                p0, p1, p2, uv0, uv1, uv2);
        vec3 emission = material.textureIndices0.w == 0xffffffffu
            ? material.emissionRoughness.rgb
            : sampleMaterialTexture(material.textureIndices0.w, surfaceUv, vec4(0.0)).rgb *
                material.transmissionOpacityIor.w;
        radiance += throughput * emission;

        float transmission = clamp(material.transmissionOpacityIor.x, 0.0, 1.0);
        vec3 transmissionColor = material.transmissionColorThinWalled.rgb;
        if (transmission > 0.0) {
            transmissionColor = sampleMaterialTexture(
                material.textureIndices1.z, surfaceUv,
                vec4(transmissionColor, 1.0)).rgb;
        }
        float subsurface = 0.0;
        vec3 diffuseColor = baseColor;
        if (material.subsurfaceWeightScale.x > 0.0 ||
            material.textureIndices2.x != 0xffffffffu) {
            subsurface = clamp(sampleMaterialTexture(
                material.textureIndices2.x, surfaceUv,
                vec4(material.subsurfaceWeightScale.x)).r, 0.0, 1.0);
            if (subsurface > 0.0) {
                vec3 subsurfaceColor = sampleMaterialTexture(
                    material.textureIndices2.y, surfaceUv,
                    vec4(material.subsurfaceColor.rgb, 1.0)).rgb;
                vec3 subsurfaceRadius = max(sampleMaterialTexture(
                    material.textureIndices2.z, surfaceUv,
                    vec4(material.subsurfaceRadius.rgb, 1.0)).rgb, vec3(0.001));
                vec3 subsurfaceAttenuation = exp(
                    -max(material.subsurfaceWeightScale.y, 0.0) / subsurfaceRadius);
                diffuseColor = mix(baseColor,
                    subsurfaceColor * subsurfaceAttenuation, subsurface);
            }
        }
        float ior = max(material.transmissionOpacityIor.z, 1.0001);
        float specularWeight = clamp(sampleMaterialTexture(
            material.textureIndices3.x, surfaceUv,
            vec4(material.specularWeightColor.x)).r, 0.0, 1.0);
        vec3 specularColor = sampleMaterialTexture(
            material.textureIndices3.y, surfaceUv,
            vec4(material.specularWeightColor.yzw, 1.0)).rgb;
        float dielectricReflectance = (ior - 1.0) / (ior + 1.0);
        dielectricReflectance *= dielectricReflectance;
        vec3 f0 = mix(clamp(vec3(dielectricReflectance * specularWeight) *
                            specularColor, vec3(0.0), vec3(1.0)),
                      baseColor, metalness);
        float coat = 0.0;
        float coatRoughness = material.coatWeightRoughnessIor.y;
        vec3 coatF0 = vec3(0.0);
        if (material.coatWeightRoughnessIor.x > 0.0 ||
            material.textureIndices3.z != 0xffffffffu) {
            coat = clamp(sampleMaterialTexture(
                material.textureIndices3.z, surfaceUv,
                vec4(material.coatWeightRoughnessIor.x)).r, 0.0, 1.0);
            if (coat > 0.0) {
                vec3 coatColor = sampleMaterialTexture(
                    material.textureIndices3.w, surfaceUv,
                    vec4(material.coatColor.rgb, 1.0)).rgb;
                coatRoughness = clamp(sampleMaterialTexture(
                    material.textureIndices4.x, surfaceUv,
                    vec4(coatRoughness)).r, 0.02, 1.0);
                float coatIor = max(material.coatWeightRoughnessIor.z, 1.0001);
                float coatReflectance = (coatIor - 1.0) / (coatIor + 1.0);
                coatReflectance *= coatReflectance;
                coatF0 = clamp(vec3(coatReflectance) * coatColor,
                               vec3(0.0), vec3(1.0));
            }
        }

        vec3 sunDirection = normalize(vec3(0.6, 0.85, 0.35));
        vec3 viewDirection = normalize(-rayDirection);
        float nDotL = max(dot(normal, sunDirection), 0.0);
        float wrappedNdotL = max((dot(normal, sunDirection) + 0.5 * subsurface) /
                                 (1.0 + 0.5 * subsurface), 0.0);
        if ((wrappedNdotL > 0.0 || nDotL > 0.0) &&
            !occluded(hit + orientedGeometricNormal * 0.002, sunDirection)) {
            vec3 halfway = normalize(viewDirection + sunDirection);
            vec3 baseFresnel = fresnelSchlick(
                max(dot(viewDirection, halfway), 0.0), f0);
            vec3 diffuse = (vec3(1.0) - baseFresnel) * (1.0 - metalness) *
                diffuseColor * (wrappedNdotL / 3.14159265359) *
                (1.0 - transmission);
            vec3 specular = nDotL * evaluateGGX(
                normal, viewDirection, sunDirection, roughness, f0);
            vec3 coatFresnel = vec3(0.0);
            vec3 coatSpecular = vec3(0.0);
            if (coat > 0.0) {
                coatFresnel = fresnelSchlick(
                    max(dot(viewDirection, halfway), 0.0), coatF0);
                coatSpecular = coat * nDotL * evaluateGGX(
                    normal, viewDirection, sunDirection, coatRoughness, coatF0);
            }
            vec3 direct = (vec3(1.0) - coat * coatFresnel) *
                (diffuse + specular) + coatSpecular;
            const vec3 sunRadiance = vec3(3.45575, 3.14159, 2.82743);
            radiance += throughput * direct * sunRadiance;
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
            if (totalInternalReflection || randomFloat(rng) < fresnel) {
                rayDirection = reflect(rayDirection, normal);
            } else {
                rayDirection = thinWalled ? rayDirection : normalize(refracted);
            }
            throughput *= transmissionColor;
            rayOrigin = hit + rayDirection * 0.002;
            continue;
        }

        rayOrigin = hit + orientedGeometricNormal * 0.002;
        vec3 coatContribution = coat > 0.0
            ? coat * fresnelSchlick(max(dot(normal, viewDirection), 0.0), coatF0)
            : vec3(0.0);
        float coatProbability = clamp(luminance(coatContribution), 0.0, 0.95);
        if (coatProbability > 0.0 && randomFloat(rng) < coatProbability) {
            vec3 incident = rayDirection;
            rayDirection = sampleGGXReflection(
                rayDirection, normal, coatRoughness, rng);
            throughput *= coat * ggxSampleWeight(
                incident, rayDirection, normal, coatRoughness, coatF0) /
                coatProbability;
        } else {
            throughput *= (vec3(1.0) - coatContribution) /
                max(1.0 - coatProbability, 1e-4);
            vec3 baseFresnel = fresnelSchlick(
                max(dot(normal, viewDirection), 0.0), f0);
            vec3 specularContribution = baseFresnel;
            vec3 diffuseContribution = (vec3(1.0) - baseFresnel) *
                (1.0 - metalness) * diffuseColor;
            float specularEnergy = luminance(specularContribution);
            float diffuseEnergy = luminance(diffuseContribution);
            float specularProbability = clamp(
                specularEnergy / max(specularEnergy + diffuseEnergy, 1e-5),
                0.05, 0.95);
            if (randomFloat(rng) < specularProbability) {
                vec3 incident = rayDirection;
                rayDirection = sampleGGXReflection(
                    rayDirection, normal, roughness, rng);
                throughput *= ggxSampleWeight(
                    incident, rayDirection, normal, roughness, f0) /
                    specularProbability;
            } else {
                throughput *= diffuseContribution /
                    max(1.0 - specularProbability, 1e-4);
                rayDirection = cosineHemisphere(normal, rng);
            }
        }
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
    std::array<std::uint32_t, 4> textureIndices0;
    std::array<std::uint32_t, 4> textureIndices1;
    std::array<std::uint32_t, 4> textureIndices2;
    std::array<std::uint32_t, 4> textureIndices3;
    std::array<std::uint32_t, 4> textureIndices4;
};

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
          queueFamily(context.ComputeQueueFamily())
    {
        CreateCommandPool();
        CreateSubmissionResources();
        CreateSampler();
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
        };
        std::array<VkDescriptorBindingFlags, 10> bindingFlags{};
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
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7},
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

    void CreatePipeline(ShaderCache& cache)
    {
        GlslCompileOptions options;
        options.generatorVersion = "hdCodex.pathtracer.v6";
        options.materialAbi = "hdcodex.pathtracer.materialx-surface.v3";
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
        DestroyBuffer(triangleMaterialBuffer);
        DestroyBuffer(materialBuffer);
        DestroyBuffer(texcoordBuffer);
        DestroyBuffer(normalBuffer);
        for (Texture& texture : textures) DestroyTexture(texture);
        textures.clear();
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
        std::map<std::string, std::uint32_t, std::less<>> textureIndices;
        const std::size_t textureCount = std::min<std::size_t>(
            snapshot->textures.size(), kMaxMaterialTextures);
        textures.reserve(textureCount);
        for (std::size_t index = 0; index < textureCount; ++index) {
            const SceneTexture& texture = snapshot->textures[index];
            textureIndices[texture.id] = static_cast<std::uint32_t>(index);
            textures.push_back(CreateTexture(texture));
        }
        const auto textureIndex = [&textureIndices](const std::string& id) {
            const auto found = textureIndices.find(id);
            return found == textureIndices.end() ? kMissingTexture : found->second;
        };

        materials.push_back({
            {0.8F, 0.8F, 0.8F, 0.0F},
            {0.0F, 0.0F, 0.0F, 0.5F},
            {0.0F, 1.0F, 1.5F, 0.0F},
            {1.0F, 1.0F, 1.0F, 0.0F},
            {0.0F, 1.0F, 0.0F, 0.0F},
            {0.8F, 0.8F, 0.8F, 0.0F},
            {1.0F, 0.2F, 0.1F, 0.0F},
            {1.0F, 1.0F, 1.0F, 1.0F},
            {0.0F, 0.1F, 1.5F, 0.0F},
            {1.0F, 1.0F, 1.0F, 0.0F},
            {kMissingTexture, kMissingTexture, kMissingTexture, kMissingTexture},
            {kMissingTexture, kMissingTexture, kMissingTexture, kMissingTexture},
            {kMissingTexture, kMissingTexture, kMissingTexture, kMissingTexture},
            {kMissingTexture, kMissingTexture, kMissingTexture, kMissingTexture},
            {kMissingTexture, kMissingTexture, kMissingTexture, kMissingTexture},
        });
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
                {material.subsurface, material.subsurfaceScale, 0.0F, 0.0F},
                {material.subsurfaceColor[0], material.subsurfaceColor[1],
                 material.subsurfaceColor[2], 0.0F},
                {material.subsurfaceRadius[0], material.subsurfaceRadius[1],
                 material.subsurfaceRadius[2], 0.0F},
                {material.specularWeight, material.specularColor[0],
                 material.specularColor[1], material.specularColor[2]},
                {material.coat, material.coatRoughness,
                 material.coatIndexOfRefraction, 0.0F},
                {material.coatColor[0], material.coatColor[1],
                 material.coatColor[2], 0.0F},
                {textureIndex(material.baseColorTexture),
                 textureIndex(material.metalnessTexture),
                 textureIndex(material.roughnessTexture),
                 textureIndex(material.emissionTexture)},
                {textureIndex(material.opacityTexture),
                 textureIndex(material.normalTexture),
                 textureIndex(material.transmissionTexture), kMissingTexture},
                {textureIndex(material.subsurfaceTexture),
                 textureIndex(material.subsurfaceColorTexture),
                 textureIndex(material.subsurfaceRadiusTexture), kMissingTexture},
                {textureIndex(material.specularTexture),
                 textureIndex(material.specularColorTexture),
                 textureIndex(material.coatTexture),
                 textureIndex(material.coatColorTexture)},
                {textureIndex(material.coatRoughnessTexture),
                 kMissingTexture, kMissingTexture, kMissingTexture},
            });
        }
        for (const SceneMesh& mesh : snapshot->meshes) {
            const std::uint32_t base = static_cast<std::uint32_t>(vertices.size());
            for (std::size_t i = 0; i + 2 < mesh.positions.size(); i += 3) {
                vertices.push_back({mesh.positions[i], mesh.positions[i + 1],
                                    mesh.positions[i + 2], 1.0F});
            }
            const auto foundMaterial = materialIndices.find(mesh.materialId);
            const std::uint32_t materialIndex = foundMaterial == materialIndices.end()
                ? 0U : foundMaterial->second;
            for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
                const std::uint32_t i0 = mesh.indices[triangle];
                const std::uint32_t i1 = mesh.indices[triangle + 1];
                const std::uint32_t i2 = mesh.indices[triangle + 2];
                if (i0 < vertices.size() - base && i1 < vertices.size() - base &&
                    i2 < vertices.size() - base) {
                    indices.insert(indices.end(), {base + i0, base + i1, base + i2});
                    triangleMaterials.push_back(materialIndex);
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
        std::array<VkWriteDescriptorSet, 10> writes{};
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

        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(textures.size());
        for (const Texture& texture : textures) {
            imageInfos.push_back({
                .sampler = textureSampler,
                .imageView = texture.view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            });
        }
        std::uint32_t writeCount = 9;
        if (!imageInfos.empty()) {
            writes[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptorSet,
                8, 0, static_cast<std::uint32_t>(imageInfos.size()),
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, imageInfos.data(), nullptr, nullptr};
            writeCount = 10;
        }
        vkUpdateDescriptorSets(device, writeCount,
                               writes.data(), 0, nullptr);
    }

    std::vector<float> Trace(const PathTracerCamera& camera, std::uint32_t width,
                             std::uint32_t height, std::uint32_t sample,
                             std::uint32_t maxBounces)
    {
        if (!geometryReady || width == 0 || height == 0) return {};
        EnsureOutput(width, height);
        CameraUniform data{
            {camera.origin[0], camera.origin[1], camera.origin[2], 0.0F},
            {camera.lowerLeft[0], camera.lowerLeft[1], camera.lowerLeft[2], 0.0F},
            {camera.horizontal[0], camera.horizontal[1], camera.horizontal[2], 0.0F},
            {camera.vertical[0], camera.vertical[1], camera.vertical[2], 0.0F},
            {width, height, sample,
             std::clamp(maxBounces, 1U, kMaxPathBounces) |
                 (sceneOpaque ? kOpaqueSceneFlag : 0U)},
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
    Buffer blasStorage;
    Buffer tlasStorage;
    VkAccelerationStructureKHR blas{VK_NULL_HANDLE};
    VkAccelerationStructureKHR tlas{VK_NULL_HANDLE};
    std::vector<Texture> textures;
    bool geometryReady{false};
    bool sceneOpaque{true};
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
    std::uint32_t sampleIndex, std::uint32_t maxBounces)
{
    return _impl->Trace(camera, width, height, sampleIndex, maxBounces);
}
bool VulkanPathTracer::HasGeometry() const noexcept { return _impl->geometryReady; }

} // namespace hdcodex
