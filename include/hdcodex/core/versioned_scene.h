#pragma once

#include "hdcodex/core/shading_mode.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hdcodex {

struct SceneMesh {
    std::string id;
    std::vector<float> positions;
    std::vector<std::uint32_t> indices;
    /// Two floats for every triangulated face corner. Keeping UVs in corner
    /// order preserves Hydra face-varying seams without duplicating positions.
    std::vector<float> texcoords;
    /// Three floats for every triangulated face corner. A zero vector requests
    /// the geometric face-normal fallback in the path tracer.
    std::vector<float> normals;
    std::string materialId;
    /// Optional material binding for each triangle in indices order. Hydra
    /// face subsets are expanded through triangulation into this array.
    std::vector<std::string> triangleMaterialIds;
    /// Linear Hydra display color used by the default material when this mesh
    /// has no authored material binding.
    std::array<float, 3> displayColor{0.5F, 0.5F, 0.5F};
};

struct SceneMaterial {
    enum class DiffuseModel : std::uint32_t {
        Lambert = 0U,
        OrenNayar = 1U,
        Burley = 2U,
        OrenNayarEnergyCompensated = 3U,
    };

    struct GeneratedInput {
        std::string name;
        std::string type;
        std::string value;
    };

    struct GeneratedTexture {
        std::string uniformName;
        std::string textureId;
        std::string colorSpace;
    };

    enum class GeneratedDescriptorKind {
        Unknown,
        UniformBuffer,
        StorageBuffer,
        SampledImage,
        StorageImage,
        AccelerationStructure,
    };

    struct GeneratedDescriptorMember {
        std::string name;
        std::uint32_t offset{0};
    };

    struct GeneratedDescriptor {
        std::string name;
        std::uint32_t set{0};
        std::uint32_t binding{0};
        GeneratedDescriptorKind kind{GeneratedDescriptorKind::Unknown};
        std::vector<GeneratedDescriptorMember> members;
    };

    struct GeneratedNodeInput {
        std::string name;
        std::string type;
        std::string value;
        std::string upstreamNode;
        std::string upstreamOutput;
        std::string colorSpace;
    };

    struct GeneratedNode {
        std::string name;
        std::string category;
        std::string nodeDef;
        std::string type;
        std::vector<GeneratedNodeInput> inputs;
    };

    std::string id;
    /// Authored terminal NodeDef identifier. Empty only for fallback materials.
    std::string shaderNodeId;
    ShadingMode materialXMode{ShadingMode::Fused};
    /// Generated MaterialX raster modules retained for the raster-preview backend
    /// and as the semantic source for generated closure compilation.
    std::vector<std::uint32_t> materialXVertexSpirv;
    std::vector<std::uint32_t> materialXPixelSpirv;
    std::vector<GeneratedDescriptor> materialXVertexDescriptors;
    std::vector<GeneratedDescriptor> materialXPixelDescriptors;
    /// Reflected inputs preserve the generated raster-program ABI. They are
    /// deliberately separate from the compact path-closure ABI below.
    std::vector<GeneratedInput> materialXPublicUniforms;
    std::vector<GeneratedTexture> materialXTextures;
    /// Dependency-ordered graph produced by expanding MaterialX NodeGraph
    /// implementations. The MaterialX closure compiler executes this source
    /// IR into the compact renderer closure ABI below. The transport kernel
    /// never inspects the authored high-level surface-model identifier.
    std::string materialXOutputNode;
    std::vector<GeneratedNode> materialXProgram;
    std::string materialXDisplacementOutputNode;
    std::vector<GeneratedNode> materialXDisplacementProgram;
    std::array<float, 3> baseColor{0.8F, 0.8F, 0.8F};
    std::array<float, 3> emission{0.0F, 0.0F, 0.0F};
    std::array<float, 3> transmissionColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> transmissionScatter{0.0F, 0.0F, 0.0F};
    std::array<float, 3> specularColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> coatColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> sheenColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> translucentColor{1.0F, 1.0F, 1.0F};
    std::array<float, 3> subsurfaceColor{0.8F, 0.8F, 0.8F};
    std::array<float, 3> subsurfaceRadius{1.0F, 0.2F, 0.1F};
    DiffuseModel diffuseModel{DiffuseModel::Lambert};
    float diffuseWeight{1.0F};
    float diffuseRoughness{0.0F};
    float metalness{0.0F};
    float roughness{0.5F};
    float roughnessV{0.5F};
    float opacity{1.0F};
    float emissionWeight{0.0F};
    float transmission{0.0F};
    float transmissionDepth{0.0F};
    float transmissionScatterAnisotropy{0.0F};
    float transmissionDispersionScale{0.0F};
    float transmissionDispersionAbbeNumber{20.0F};
    float indexOfRefraction{1.5F};
    float specularWeight{1.0F};
    float coat{0.0F};
    float coatRoughness{0.1F};
    float coatRoughnessV{0.1F};
    float coatIndexOfRefraction{1.5F};
    float sheen{0.0F};
    float sheenRoughness{0.3F};
    /// MaterialX sheen mode: 0 is Conty-Kulla, 1 is Zeltner.
    std::uint32_t sheenMode{0U};
    float translucentWeight{0.0F};
    float subsurface{0.0F};
    float subsurfaceScale{1.0F};
    float subsurfaceScatterAnisotropy{0.0F};
    bool thinWalled{false};
    /// MaterialX normal graphs may explicitly invert the green tangent-space
    /// channel (DirectX convention). This is derived from the expanded value
    /// graph, not from a high-level shader-model input.
    bool normalTextureFlipY{false};
    float normalTextureScale{1.0F};
    std::string baseColorTexture;
    std::string metalnessTexture;
    std::string roughnessTexture;
    std::string diffuseWeightTexture;
    std::string diffuseRoughnessTexture;
    std::string emissionTexture;
    std::string opacityTexture;
    std::string normalTexture;
    std::string transmissionTexture;
    std::string specularTexture;
    std::string specularColorTexture;
    std::string coatTexture;
    std::string coatColorTexture;
    std::string coatRoughnessTexture;
    std::string sheenTexture;
    std::string sheenColorTexture;
    std::string sheenRoughnessTexture;
    std::string translucentWeightTexture;
    std::string translucentColorTexture;
    std::string subsurfaceTexture;
    std::string subsurfaceColorTexture;
    std::string subsurfaceRadiusTexture;
};

struct SceneTexture {
    std::string id;
    std::string sourcePath;
    /// Non-empty for a tile belonging to one logical MaterialX UDIM image.
    /// Tiles in a set are uploaded independently and selected from the UV tile
    /// number in the path tracer, avoiding a potentially enormous sparse atlas.
    std::string udimSetId;
    std::uint32_t udimTile{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    bool srgb{false};
    std::vector<std::uint8_t> rgba;
    /// Linear floating-point pixels are used for HDR light textures. Material
    /// textures retain the compact RGBA8 path above.
    std::vector<float> rgbaFloat;
};

enum class SceneLightType : std::uint32_t {
    Dome,
    Rect,
    Disk,
    Sphere,
    Cylinder,
    Distant,
};

enum class DomeTextureFormat : std::uint32_t {
    Automatic,
    LatLong,
    MirroredBall,
    Angular,
    CubeMapVerticalCross,
};

struct SceneLight {
    std::string id;
    SceneLightType type{SceneLightType::Dome};
    bool visible{true};
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    std::array<float, 3> temperatureColor{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    float exposure{0.0F};
    float diffuse{1.0F};
    float specular{1.0F};
    bool normalize{false};

    std::string texture;
    DomeTextureFormat textureFormat{DomeTextureFormat::Automatic};

    /// Orthonormal local axes in world space. Dome maps world directions back
    /// through these axes; Rect uses basisZ as its emitting (-Z) direction.
    std::array<float, 3> basisX{1.0F, 0.0F, 0.0F};
    std::array<float, 3> basisY{0.0F, 1.0F, 0.0F};
    std::array<float, 3> basisZ{0.0F, 0.0F, 1.0F};
    std::array<float, 3> position{0.0F, 0.0F, 0.0F};
    /// Full world-space edge vectors for the authored RectLight dimensions.
    std::array<float, 3> axisU{1.0F, 0.0F, 0.0F};
    std::array<float, 3> axisV{0.0F, 1.0F, 0.0F};
    float width{1.0F};
    float height{1.0F};
    float radius{0.5F};
    float length{1.0F};
    float angle{0.53F};
    float area{1.0F};

    float shapingFocus{0.0F};
    std::array<float, 3> shapingFocusTint{0.0F, 0.0F, 0.0F};
    float shapingConeAngle{180.0F};
    float shapingConeSoftness{0.0F};

    bool shadowEnable{true};
    std::array<float, 3> shadowColor{0.0F, 0.0F, 0.0F};
    float shadowDistance{-1.0F};
    float shadowFalloff{-1.0F};
    float shadowFalloffGamma{1.0F};
};

struct SceneSnapshot {
    std::uint64_t revision{0};
    std::vector<SceneMesh> meshes;
    std::vector<SceneMaterial> materials;
    std::vector<SceneTexture> textures;
    std::vector<SceneLight> lights;
};

/// Tracks staging and published scene revisions without exposing Hydra types to
/// the renderer core. Mutations can arrive concurrently from Hydra Sync calls.
class VersionedScene final {
public:
    using Revision = std::uint64_t;

    [[nodiscard]] Revision MarkDirty() noexcept
    {
        return _staging.fetch_add(1, std::memory_order_acq_rel) + 1;
    }

    [[nodiscard]] Revision StagingRevision() const noexcept
    {
        return _staging.load(std::memory_order_acquire);
    }

    [[nodiscard]] Revision PublishedRevision() const noexcept
    {
        return _published.load(std::memory_order_acquire);
    }

    [[nodiscard]] Revision Publish()
    {
        const std::scoped_lock lock(_mutex);
        const Revision revision = StagingRevision();
        auto snapshot = std::make_shared<SceneSnapshot>();
        snapshot->revision = revision;
        snapshot->meshes.reserve(_meshes.size());
        for (const auto& [id, mesh] : _meshes) {
            (void)id;
            snapshot->meshes.push_back(mesh);
        }
        snapshot->materials.reserve(_materials.size());
        for (const auto& [id, material] : _materials) {
            (void)id;
            snapshot->materials.push_back(material);
        }
        snapshot->textures.reserve(_textures.size());
        for (const auto& [id, texture] : _textures) {
            (void)id;
            snapshot->textures.push_back(texture);
        }
        snapshot->lights.reserve(_lights.size());
        for (const auto& [id, light] : _lights) {
            (void)id;
            snapshot->lights.push_back(light);
        }
        _snapshot = std::move(snapshot);
        _published.store(revision, std::memory_order_release);
        return revision;
    }

    void UpsertMesh(SceneMesh mesh)
    {
        if (mesh.id.empty()) return;
        const std::scoped_lock lock(_mutex);
        _meshes[mesh.id] = std::move(mesh);
        (void)MarkDirty();
    }

    void RemoveMesh(const std::string& id)
    {
        const std::scoped_lock lock(_mutex);
        if (_meshes.erase(id) != 0U) (void)MarkDirty();
    }

    void UpsertMaterial(SceneMaterial material)
    {
        if (material.id.empty()) return;
        const std::scoped_lock lock(_mutex);
        _materials[material.id] = std::move(material);
        (void)MarkDirty();
    }

    void RemoveMaterial(const std::string& id)
    {
        const std::scoped_lock lock(_mutex);
        if (_materials.erase(id) != 0U) (void)MarkDirty();
    }

    [[nodiscard]] bool HasTexture(const std::string& id) const
    {
        const std::scoped_lock lock(_mutex);
        return _textures.contains(id);
    }

    [[nodiscard]] std::optional<SceneTexture> GetTexture(
        const std::string& id) const
    {
        const std::scoped_lock lock(_mutex);
        const auto found = _textures.find(id);
        return found == _textures.end()
            ? std::optional<SceneTexture>{}
            : std::optional<SceneTexture>{found->second};
    }

    void UpsertTexture(SceneTexture texture)
    {
        const std::size_t expected = static_cast<std::size_t>(texture.width) *
            texture.height * 4U;
        if (texture.id.empty() || texture.width == 0 || texture.height == 0 ||
            (texture.rgba.size() != expected && texture.rgbaFloat.size() != expected)) return;
        const std::scoped_lock lock(_mutex);
        _textures[texture.id] = std::move(texture);
        (void)MarkDirty();
    }

    void UpsertLight(SceneLight light)
    {
        if (light.id.empty()) return;
        const std::scoped_lock lock(_mutex);
        _lights[light.id] = std::move(light);
        (void)MarkDirty();
    }

    void RemoveLight(const std::string& id)
    {
        const std::scoped_lock lock(_mutex);
        if (_lights.erase(id) != 0U) (void)MarkDirty();
    }

    [[nodiscard]] std::shared_ptr<const SceneSnapshot> Snapshot() const
    {
        const std::scoped_lock lock(_mutex);
        return _snapshot;
    }

private:
    std::atomic<Revision> _staging{0};
    std::atomic<Revision> _published{0};
    mutable std::mutex _mutex;
    std::map<std::string, SceneMesh, std::less<>> _meshes;
    std::map<std::string, SceneMaterial, std::less<>> _materials;
    std::map<std::string, SceneTexture, std::less<>> _textures;
    std::map<std::string, SceneLight, std::less<>> _lights;
    std::shared_ptr<const SceneSnapshot> _snapshot =
        std::make_shared<const SceneSnapshot>();
};

} // namespace hdcodex
